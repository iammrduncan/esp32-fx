// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal text renderer and UC8179/GD7965 userspace driver for the
 * Seeed reTerminal E1001 (GDEY075T7, 800x480 monochrome e-paper).
 *
 * The panel command sequence follows Seeed_GxEPD2's
 * GxEPD2_750_GDEY075T7 driver, based on Jean-Marc Zingg's GxEPD2 and
 * Good Display's panel sequences. See CREDITS.md for source pins and terms.
 * SPI chip-select is handled by the ESP32-S3
 * SPI controller; GPIO11/12/13 are DC/reset/busy through Linux GPIO sysfs.
 */

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/spi/spidev.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/*
 * This Linux port executes application .rodata directly from SPI flash.
 * Byte loads from that mapping are not reliable on the ESP32-S3, so data
 * consumed byte-by-byte must live in the copied, writable data segment.
 */
#if defined(__GNUC__)
#define EINKCTL_RAM_DATA __attribute__((section(".data")))
#else
#define EINKCTL_RAM_DATA
#endif

#include "font8x16_ascii.inc"

#define PANEL_WIDTH 800
#define PANEL_HEIGHT 480
#define FRAMEBUFFER_BYTES ((PANEL_WIDTH * PANEL_HEIGHT) / 8)
#define FONT_WIDTH 8
#define FONT_HEIGHT 16
#define BODY_X 8
#define BODY_Y 32
#define BODY_WIDTH (PANEL_WIDTH - 2 * BODY_X)
#define BODY_BOTTOM 448
#define TEXT_COLUMNS (BODY_WIDTH / FONT_WIDTH)
#define TEXT_ROWS ((BODY_BOTTOM - BODY_Y) / FONT_HEIGHT)
#define DEFAULT_SPI_DEVICE "/dev/spidev0.0"
#define DEFAULT_SPI_HZ 2000000U
#define GPIO_LABEL "esp32s3-gpio"
#define PIN_DC 11
#define PIN_RESET 12
#define PIN_BUSY 13
#define MAX_INPUT_BYTES (256U * 1024U)

struct gpio_line {
    unsigned int number;
    int value_fd;
    bool exported_here;
};

struct panel {
    int spi_fd;
    struct gpio_line dc;
    struct gpio_line reset;
    struct gpio_line busy;
    uint32_t speed_hz;
};

struct text_lines {
    char *data;
    size_t count;
    size_t capacity;
};

static void usage(FILE *stream)
{
    fprintf(stream,
            "Usage:\n"
            "  einkctl render [-o FILE] [-p PAGE] [-t TITLE] [INPUT|-]\n"
            "  einkctl display [-d SPI] [-s HZ] [-p PAGE] [-t TITLE] [INPUT|-]\n"
            "  einkctl clear [-d SPI] [-s HZ]\n"
            "  einkctl probe [-d SPI]\n"
            "\n"
            "INPUT defaults to stdin. PAGE is zero-based. The PBM output uses\n"
            "800x480 pixels. Display performs a full refresh; clear performs\n"
            "a full white refresh.\n");
}

static void sleep_ms(unsigned int milliseconds)
{
    struct timespec requested = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L,
    };

    while (nanosleep(&requested, &requested) < 0 && errno == EINTR)
        ;
}

static int write_all(int fd, const void *buffer, size_t length)
{
    const uint8_t *cursor = buffer;

    while (length > 0) {
        ssize_t written = write(fd, cursor, length);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (written == 0) {
            errno = EIO;
            return -1;
        }
        cursor += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

static char *read_text(const char *path, size_t *length_out)
{
    FILE *input = stdin;
    char *buffer;
    size_t capacity = 4096;
    size_t length = 0;

    if (path && strcmp(path, "-") != 0) {
        input = fopen(path, "rb");
        if (!input) {
            fprintf(stderr, "cannot open input %s: %s\n", path, strerror(errno));
            return NULL;
        }
    }

    buffer = malloc(capacity + 1);
    if (!buffer) {
        fprintf(stderr, "out of memory reading input\n");
        if (input != stdin)
            fclose(input);
        return NULL;
    }

    while (!feof(input)) {
        size_t amount;

        if (length == capacity) {
            char *grown;

            if (capacity >= MAX_INPUT_BYTES) {
                fprintf(stderr, "input exceeds %u bytes\n", MAX_INPUT_BYTES);
                free(buffer);
                if (input != stdin)
                    fclose(input);
                return NULL;
            }
            capacity *= 2;
            if (capacity > MAX_INPUT_BYTES)
                capacity = MAX_INPUT_BYTES;
            grown = realloc(buffer, capacity + 1);
            if (!grown) {
                fprintf(stderr, "out of memory reading input\n");
                free(buffer);
                if (input != stdin)
                    fclose(input);
                return NULL;
            }
            buffer = grown;
        }

        amount = fread(buffer + length, 1, capacity - length, input);
        length += amount;
        if (ferror(input)) {
            fprintf(stderr, "cannot read input: %s\n", strerror(errno));
            free(buffer);
            if (input != stdin)
                fclose(input);
            return NULL;
        }
    }

    if (input != stdin && fclose(input) != 0) {
        fprintf(stderr, "cannot close input %s: %s\n", path, strerror(errno));
        free(buffer);
        return NULL;
    }

    buffer[length] = '\0';
    *length_out = length;
    return buffer;
}

static void framebuffer_pixel(uint8_t *framebuffer, int x, int y, bool black)
{
    size_t index;
    uint8_t mask;

    if (x < 0 || x >= PANEL_WIDTH || y < 0 || y >= PANEL_HEIGHT)
        return;
    index = (size_t)y * (PANEL_WIDTH / 8) + (size_t)x / 8;
    mask = (uint8_t)(0x80U >> (x & 7));
    if (black)
        framebuffer[index] &= (uint8_t)~mask;
    else
        framebuffer[index] |= mask;
}

static void framebuffer_rect(uint8_t *framebuffer, int x, int y, int width,
                             int height, bool black)
{
    int row;
    int column;

    for (row = y; row < y + height; row++)
        for (column = x; column < x + width; column++)
            framebuffer_pixel(framebuffer, column, row, black);
}

static void framebuffer_glyph(uint8_t *framebuffer, int x, int y,
                              unsigned char character, bool black,
                              bool opaque)
{
    const unsigned char *glyph;
    int row;
    int column;

    if (character < 0x20 || character > 0x7e)
        character = '?';
    glyph = font8x16_ascii[character - 0x20];
    for (row = 0; row < FONT_HEIGHT; row++) {
        for (column = 0; column < FONT_WIDTH; column++) {
            bool set = (glyph[row] & (0x80U >> column)) != 0;

            if (set)
                framebuffer_pixel(framebuffer, x + column, y + row, black);
            else if (opaque)
                framebuffer_pixel(framebuffer, x + column, y + row, !black);
        }
    }
}

static void framebuffer_text(uint8_t *framebuffer, int x, int y,
                             const char *text, size_t length, bool black,
                             bool opaque)
{
    size_t index;

    for (index = 0; index < length; index++) {
        framebuffer_glyph(framebuffer, x, y, (unsigned char)text[index], black,
                          opaque);
        x += FONT_WIDTH;
    }
}

static int lines_reserve(struct text_lines *lines, size_t wanted)
{
    char *grown;
    size_t capacity;

    if (wanted <= lines->capacity)
        return 0;
    capacity = lines->capacity ? lines->capacity : 64;
    while (capacity < wanted)
        capacity *= 2;
    grown = realloc(lines->data, capacity * (TEXT_COLUMNS + 1));
    if (!grown)
        return -1;
    lines->data = grown;
    lines->capacity = capacity;
    return 0;
}

static int lines_push(struct text_lines *lines, const char *text, size_t length)
{
    char *destination;

    if (length > TEXT_COLUMNS)
        length = TEXT_COLUMNS;
    if (lines_reserve(lines, lines->count + 1) < 0)
        return -1;
    destination = lines->data + lines->count * (TEXT_COLUMNS + 1);
    memcpy(destination, text, length);
    destination[length] = '\0';
    lines->count++;
    return 0;
}

static bool is_utf8_continuation(unsigned char byte)
{
    return (byte & 0xc0U) == 0x80U;
}

static size_t sanitize_text(const char *input, size_t input_length, char *output)
{
    size_t source = 0;
    size_t destination = 0;
    enum { ESCAPE_NONE, ESCAPE_START, ESCAPE_CSI, ESCAPE_OSC } escape = ESCAPE_NONE;

    while (source < input_length) {
        unsigned char byte = (unsigned char)input[source++];

        if (escape == ESCAPE_START) {
            if (byte == '[')
                escape = ESCAPE_CSI;
            else if (byte == ']')
                escape = ESCAPE_OSC;
            else
                escape = ESCAPE_NONE;
            continue;
        }
        if (escape == ESCAPE_CSI) {
            if (byte >= 0x40 && byte <= 0x7e)
                escape = ESCAPE_NONE;
            continue;
        }
        if (escape == ESCAPE_OSC) {
            if (byte == '\a')
                escape = ESCAPE_NONE;
            else if (byte == 0x1b && source < input_length && input[source] == '\\') {
                source++;
                escape = ESCAPE_NONE;
            }
            continue;
        }
        if (byte == 0x1b) {
            escape = ESCAPE_START;
            continue;
        }
        if (byte == '\r') {
            if (source < input_length && input[source] == '\n')
                continue;
            output[destination++] = '\n';
            continue;
        }
        if (byte == '\t') {
            size_t spaces = 4 - (destination & 3U);

            while (spaces--)
                output[destination++] = ' ';
            continue;
        }
        if (byte == '\n' || (byte >= 0x20 && byte <= 0x7e)) {
            output[destination++] = (char)byte;
            continue;
        }
        if (byte >= 0x80) {
            while (source < input_length &&
                   is_utf8_continuation((unsigned char)input[source]))
                source++;
            output[destination++] = '?';
        }
    }
    output[destination] = '\0';
    return destination;
}

static int wrap_text(const char *text, size_t length, struct text_lines *lines)
{
    size_t start = 0;

    while (start < length) {
        size_t physical_end = start;

        while (physical_end < length && text[physical_end] != '\n')
            physical_end++;

        if (physical_end == start) {
            if (lines_push(lines, "", 0) < 0)
                return -1;
        } else {
            size_t cursor = start;

            while (cursor < physical_end) {
                size_t remaining = physical_end - cursor;
                size_t take = remaining > TEXT_COLUMNS ? TEXT_COLUMNS : remaining;
                size_t split = take;

                if (take < remaining) {
                    while (split > 0 && text[cursor + split] != ' ')
                        split--;
                    if (split < TEXT_COLUMNS / 3)
                        split = take;
                }
                while (take > 0 && text[cursor + take - 1] == ' ')
                    take--;
                if (split != take && split != 0)
                    take = split;
                if (lines_push(lines, text + cursor, take) < 0)
                    return -1;
                cursor += split ? split : take;
                while (cursor < physical_end && text[cursor] == ' ')
                    cursor++;
            }
        }

        start = physical_end;
        if (start < length && text[start] == '\n')
            start++;
    }
    if (length == 0 || (length > 0 && text[length - 1] == '\n'))
        return lines_push(lines, "", 0);
    return 0;
}

static int render_text(uint8_t *framebuffer, const char *input,
                       size_t input_length, const char *title, size_t page,
                       size_t *page_count_out)
{
    struct text_lines lines = { 0 };
    char *sanitized;
    size_t sanitized_length;
    size_t pages;
    size_t first_line;
    size_t visible;
    size_t row;
    char footer[64];
    int footer_length;

    sanitized = malloc(input_length * 4 + 1);
    if (!sanitized)
        return -1;
    sanitized_length = sanitize_text(input, input_length, sanitized);
    if (wrap_text(sanitized, sanitized_length, &lines) < 0) {
        free(sanitized);
        free(lines.data);
        return -1;
    }
    free(sanitized);

    pages = (lines.count + TEXT_ROWS - 1) / TEXT_ROWS;
    if (pages == 0)
        pages = 1;
    if (page >= pages) {
        fprintf(stderr, "page %zu is out of range (document has %zu page%s)\n",
                page, pages, pages == 1 ? "" : "s");
        free(lines.data);
        errno = EINVAL;
        return -1;
    }

    memset(framebuffer, 0xff, FRAMEBUFFER_BYTES);
    framebuffer_rect(framebuffer, 0, 0, PANEL_WIDTH, 24, true);
    framebuffer_text(framebuffer, 8, 4, title, strlen(title) > 96 ? 96 : strlen(title),
                     false, true);
    framebuffer_rect(framebuffer, 0, 24, PANEL_WIDTH, 1, true);

    first_line = page * TEXT_ROWS;
    visible = lines.count - first_line;
    if (visible > TEXT_ROWS)
        visible = TEXT_ROWS;
    for (row = 0; row < visible; row++) {
        const char *line = lines.data + (first_line + row) * (TEXT_COLUMNS + 1);

        framebuffer_text(framebuffer, BODY_X, BODY_Y + (int)row * FONT_HEIGHT,
                         line, strlen(line), true, false);
    }

    framebuffer_rect(framebuffer, 0, BODY_BOTTOM + 7, PANEL_WIDTH, 1, true);
    footer_length = snprintf(footer, sizeof(footer),
                             "fx on Linux / ESP32-S3                    page %zu/%zu",
                             page + 1, pages);
    if (footer_length < 0)
        footer_length = 0;
    if ((size_t)footer_length > sizeof(footer) - 1)
        footer_length = (int)(sizeof(footer) - 1);
    framebuffer_text(framebuffer, 8, 460, footer, (size_t)footer_length, true,
                     false);

    *page_count_out = pages;
    free(lines.data);
    return 0;
}

static int write_pbm(const char *path, const uint8_t *framebuffer)
{
    FILE *output = stdout;

    if (path && strcmp(path, "-") != 0) {
        output = fopen(path, "wb");
        if (!output) {
            fprintf(stderr, "cannot open output %s: %s\n", path, strerror(errno));
            return -1;
        }
    }
    if (fprintf(output, "P4\n%d %d\n", PANEL_WIDTH, PANEL_HEIGHT) < 0) {
        fprintf(stderr, "cannot write PBM header: %s\n", strerror(errno));
        if (output != stdout)
            fclose(output);
        return -1;
    }

    /* Panel bytes use 1=white; binary PBM uses 1=black. */
    for (size_t index = 0; index < FRAMEBUFFER_BYTES; index++) {
        uint8_t inverted = (uint8_t)~framebuffer[index];

        if (fwrite(&inverted, 1, 1, output) != 1) {
            fprintf(stderr, "cannot write PBM pixels: %s\n", strerror(errno));
            if (output != stdout)
                fclose(output);
            return -1;
        }
    }
    if (fflush(output) != 0) {
        fprintf(stderr, "cannot flush PBM output: %s\n", strerror(errno));
        if (output != stdout)
            fclose(output);
        return -1;
    }
    if (output != stdout && fclose(output) != 0) {
        fprintf(stderr, "cannot close PBM output %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int read_small_file(const char *path, char *buffer, size_t capacity)
{
    int fd;
    ssize_t amount;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    amount = read(fd, buffer, capacity - 1);
    if (amount < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    close(fd);
    buffer[amount] = '\0';
    while (amount > 0 &&
           (buffer[amount - 1] == '\n' || buffer[amount - 1] == '\r'))
        buffer[--amount] = '\0';
    return 0;
}

static int find_gpio_base(unsigned int *base_out)
{
    DIR *directory;
    struct dirent *entry;
    int result = -1;

    directory = opendir("/sys/class/gpio");
    if (!directory)
        return -1;
    while ((entry = readdir(directory)) != NULL) {
        char path[256];
        char label[96];
        char base[32];
        char *end;
        unsigned long parsed;

        if (strncmp(entry->d_name, "gpiochip", 8) != 0)
            continue;
        if (snprintf(path, sizeof(path), "/sys/class/gpio/%s/label",
                     entry->d_name) >= (int)sizeof(path))
            continue;
        if (read_small_file(path, label, sizeof(label)) < 0 ||
            strcmp(label, GPIO_LABEL) != 0)
            continue;
        if (snprintf(path, sizeof(path), "/sys/class/gpio/%s/base",
                     entry->d_name) >= (int)sizeof(path))
            continue;
        if (read_small_file(path, base, sizeof(base)) < 0)
            continue;
        errno = 0;
        parsed = strtoul(base, &end, 10);
        if (errno != 0 || *end != '\0' || parsed > UINT32_MAX)
            continue;
        *base_out = (unsigned int)parsed;
        result = 0;
        break;
    }
    closedir(directory);
    if (result < 0)
        errno = ENODEV;
    return result;
}

static int sysfs_write_text(const char *path, const char *text)
{
    int fd;
    int saved;

    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (write_all(fd, text, strlen(text)) < 0) {
        saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return close(fd);
}

static int gpio_open_line(struct gpio_line *line, unsigned int base,
                          unsigned int offset, bool output, bool initial)
{
    char number[32];
    char path[128];
    int attempt;

    memset(line, 0, sizeof(*line));
    line->value_fd = -1;
    line->number = base + offset;
    snprintf(number, sizeof(number), "%u", line->number);
    if (sysfs_write_text("/sys/class/gpio/export", number) == 0) {
        line->exported_here = true;
    } else if (errno != EBUSY) {
        fprintf(stderr, "cannot export GPIO%u: %s\n", offset, strerror(errno));
        return -1;
    }

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/direction", line->number);
    for (attempt = 0; attempt < 100; attempt++) {
        if (access(path, W_OK) == 0)
            break;
        sleep_ms(10);
    }
    if (attempt == 100) {
        fprintf(stderr, "GPIO%u did not appear in sysfs\n", offset);
        errno = ETIMEDOUT;
        return -1;
    }
    if (sysfs_write_text(path, output ? (initial ? "high" : "low") : "in") < 0) {
        fprintf(stderr, "cannot configure GPIO%u: %s\n", offset, strerror(errno));
        return -1;
    }

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/value", line->number);
    line->value_fd = open(path, (output ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (line->value_fd < 0) {
        fprintf(stderr, "cannot open GPIO%u value: %s\n", offset, strerror(errno));
        return -1;
    }
    return 0;
}

static int gpio_set(struct gpio_line *line, bool value)
{
    char byte = value ? '1' : '0';

    if (lseek(line->value_fd, 0, SEEK_SET) < 0)
        return -1;
    return write_all(line->value_fd, &byte, 1);
}

static int gpio_get(struct gpio_line *line, bool *value_out)
{
    char byte;
    ssize_t amount;

    if (lseek(line->value_fd, 0, SEEK_SET) < 0)
        return -1;
    do {
        amount = read(line->value_fd, &byte, 1);
    } while (amount < 0 && errno == EINTR);
    if (amount != 1) {
        if (amount == 0)
            errno = EIO;
        return -1;
    }
    *value_out = byte == '1';
    return 0;
}

static void gpio_close_line(struct gpio_line *line)
{
    if (line->value_fd >= 0)
        close(line->value_fd);
    if (line->exported_here) {
        char number[32];

        snprintf(number, sizeof(number), "%u", line->number);
        (void)sysfs_write_text("/sys/class/gpio/unexport", number);
    }
    line->value_fd = -1;
}

static int panel_spi_write(struct panel *panel, const uint8_t *bytes, size_t length)
{
    while (length > 0) {
        size_t chunk = length > 4096 ? 4096 : length;

        if (write_all(panel->spi_fd, bytes, chunk) < 0)
            return -1;
        bytes += chunk;
        length -= chunk;
    }
    return 0;
}

static int panel_command(struct panel *panel, uint8_t command,
                         const uint8_t *data, size_t length)
{
    if (gpio_set(&panel->dc, false) < 0 ||
        panel_spi_write(panel, &command, 1) < 0)
        return -1;
    if (length > 0) {
        if (gpio_set(&panel->dc, true) < 0 ||
            panel_spi_write(panel, data, length) < 0)
            return -1;
    }
    return 0;
}

static int panel_wait_idle(struct panel *panel, unsigned int timeout_ms,
                           const char *operation)
{
    unsigned int elapsed = 0;

    while (elapsed <= timeout_ms) {
        bool idle;

        if (gpio_get(&panel->busy, &idle) < 0) {
            fprintf(stderr, "cannot read panel busy line: %s\n", strerror(errno));
            return -1;
        }
        if (idle)
            return 0;
        sleep_ms(50);
        elapsed += 50;
    }
    fprintf(stderr, "panel timed out during %s after %u ms\n", operation,
            timeout_ms);
    errno = ETIMEDOUT;
    return -1;
}

static int panel_open(struct panel *panel, const char *device, uint32_t speed_hz)
{
    unsigned int base;
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;

    memset(panel, 0, sizeof(*panel));
    panel->spi_fd = -1;
    panel->dc.value_fd = -1;
    panel->reset.value_fd = -1;
    panel->busy.value_fd = -1;
    panel->speed_hz = speed_hz;

    if (find_gpio_base(&base) < 0) {
        fprintf(stderr, "cannot find Linux GPIO controller %s: %s\n",
                GPIO_LABEL, strerror(errno));
        return -1;
    }
    if (gpio_open_line(&panel->dc, base, PIN_DC, true, false) < 0 ||
        gpio_open_line(&panel->reset, base, PIN_RESET, true, true) < 0 ||
        gpio_open_line(&panel->busy, base, PIN_BUSY, false, false) < 0)
        return -1;

    panel->spi_fd = open(device, O_WRONLY | O_CLOEXEC);
    if (panel->spi_fd < 0) {
        fprintf(stderr, "cannot open SPI device %s: %s\n", device,
                strerror(errno));
        return -1;
    }
    if (ioctl(panel->spi_fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(panel->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(panel->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed_hz) < 0) {
        fprintf(stderr, "cannot configure %s: %s\n", device, strerror(errno));
        return -1;
    }
    return 0;
}

static void panel_close(struct panel *panel)
{
    if (panel->spi_fd >= 0)
        close(panel->spi_fd);
    gpio_close_line(&panel->busy);
    gpio_close_line(&panel->reset);
    gpio_close_line(&panel->dc);
    panel->spi_fd = -1;
}

static int panel_reset(struct panel *panel)
{
    if (gpio_set(&panel->reset, true) < 0)
        return -1;
    sleep_ms(10);
    if (gpio_set(&panel->reset, false) < 0)
        return -1;
    sleep_ms(10);
    if (gpio_set(&panel->reset, true) < 0)
        return -1;
    sleep_ms(10);
    return 0;
}

static int panel_initialize(struct panel *panel)
{
    static uint8_t panel_setting[] EINKCTL_RAM_DATA = { 0x1f };
    static uint8_t power_setting[] EINKCTL_RAM_DATA = {
        0x07, 0x07, 0x3f, 0x3f, 0x09
    };
    static uint8_t booster[] EINKCTL_RAM_DATA = { 0x17, 0x17, 0x28, 0x17 };
    static uint8_t resolution[] EINKCTL_RAM_DATA = { 0x03, 0x20, 0x01, 0xe0 };
    static uint8_t dual_spi[] EINKCTL_RAM_DATA = { 0x00 };
    static uint8_t vcom_interval[] EINKCTL_RAM_DATA = { 0x29, 0x07 };
    static uint8_t tcon[] EINKCTL_RAM_DATA = { 0x22 };
    static uint8_t power_saving[] EINKCTL_RAM_DATA = { 0x22 };

    if (panel_reset(panel) < 0 ||
        panel_command(panel, 0x00, panel_setting, sizeof(panel_setting)) < 0 ||
        panel_command(panel, 0x01, power_setting, sizeof(power_setting)) < 0 ||
        panel_command(panel, 0x06, booster, sizeof(booster)) < 0 ||
        panel_command(panel, 0x61, resolution, sizeof(resolution)) < 0 ||
        panel_command(panel, 0x15, dual_spi, sizeof(dual_spi)) < 0 ||
        panel_command(panel, 0x50, vcom_interval, sizeof(vcom_interval)) < 0 ||
        panel_command(panel, 0x60, tcon, sizeof(tcon)) < 0 ||
        panel_command(panel, 0xe3, power_saving, sizeof(power_saving)) < 0 ||
        panel_command(panel, 0x00, panel_setting, sizeof(panel_setting)) < 0)
        return -1;
    return 0;
}

static int panel_fill(struct panel *panel, uint8_t command, uint8_t value)
{
    uint8_t chunk[256];
    size_t remaining = FRAMEBUFFER_BYTES;

    memset(chunk, value, sizeof(chunk));
    if (panel_command(panel, command, NULL, 0) < 0 ||
        gpio_set(&panel->dc, true) < 0)
        return -1;
    while (remaining > 0) {
        size_t amount = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;

        if (panel_spi_write(panel, chunk, amount) < 0)
            return -1;
        remaining -= amount;
    }
    return 0;
}

static int panel_display(struct panel *panel, const uint8_t *framebuffer)
{
    static uint8_t fast_cascade[] EINKCTL_RAM_DATA = { 0x02 };
    static uint8_t forced_temperature[] EINKCTL_RAM_DATA = { 0x5a };

    fprintf(stderr, "initializing GDEY075T7 panel\n");
    if (panel_initialize(panel) < 0)
        goto fail;
    if (panel_command(panel, 0x04, NULL, 0) < 0 ||
        panel_wait_idle(panel, 15000, "power-on") < 0)
        goto fail;

    fprintf(stderr, "%s 48,000-byte framebuffer\n",
            framebuffer ? "transferring" : "clearing");
    if (panel_fill(panel, 0x10, 0x00) < 0 ||
        (framebuffer ?
            (panel_command(panel, 0x13, NULL, 0) < 0 ||
             gpio_set(&panel->dc, true) < 0 ||
             panel_spi_write(panel, framebuffer, FRAMEBUFFER_BYTES) < 0) :
            panel_fill(panel, 0x13, 0xff) < 0) ||
        panel_command(panel, 0xe0, fast_cascade, sizeof(fast_cascade)) < 0 ||
        panel_command(panel, 0xe5, forced_temperature,
                      sizeof(forced_temperature)) < 0)
        goto fail;

    fprintf(stderr, "refreshing e-paper (this can take several seconds)\n");
    if (panel_command(panel, 0x12, NULL, 0) < 0 ||
        panel_wait_idle(panel, 60000, "full refresh") < 0)
        goto fail;
    if (panel_command(panel, 0x02, NULL, 0) < 0 ||
        panel_wait_idle(panel, 15000, "power-off") < 0)
        goto fail;
    fprintf(stderr, "display refresh complete\n");
    return 0;

fail:
    fprintf(stderr, "panel operation failed: %s\n", strerror(errno));
    (void)panel_command(panel, 0x02, NULL, 0);
    return -1;
}

static int probe_hardware(const char *device)
{
    unsigned int base;
    int fd;

    if (find_gpio_base(&base) < 0) {
        fprintf(stderr, "GPIO: unavailable (%s)\n", strerror(errno));
        return -1;
    }
    printf("GPIO controller: %s, base %u, DC=%u, RESET=%u, BUSY=%u\n",
           GPIO_LABEL, base, base + PIN_DC, base + PIN_RESET, base + PIN_BUSY);
    fd = open(device, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "SPI: %s unavailable (%s)\n", device, strerror(errno));
        return -1;
    }
    close(fd);
    printf("SPI device: %s\n", device);
    printf("panel: GDEY075T7 / UC8179, %dx%d, mode 0, %u Hz\n",
           PANEL_WIDTH, PANEL_HEIGHT, DEFAULT_SPI_HZ);
    return 0;
}

static int parse_size(const char *text, size_t *value_out)
{
    char *end;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' || value > SIZE_MAX) {
        errno = EINVAL;
        return -1;
    }
    *value_out = (size_t)value;
    return 0;
}

static int parse_speed(const char *text, uint32_t *value_out)
{
    size_t parsed;

    if (parse_size(text, &parsed) < 0 || parsed == 0 || parsed > UINT32_MAX) {
        errno = EINVAL;
        return -1;
    }
    *value_out = (uint32_t)parsed;
    return 0;
}

int main(int argc, char **argv)
{
    enum { COMMAND_RENDER, COMMAND_DISPLAY, COMMAND_CLEAR, COMMAND_PROBE } command;
    static char default_title[] EINKCTL_RAM_DATA = "fx | ESP32-S3 Linux";
    const char *output_path = "-";
    const char *input_path = NULL;
    const char *spi_device = DEFAULT_SPI_DEVICE;
    const char *title = default_title;
    uint32_t speed_hz = DEFAULT_SPI_HZ;
    size_t page = 0;
    size_t page_count = 0;
    size_t input_length = 0;
    char *input = NULL;
    uint8_t *framebuffer = NULL;
    int option;
    int result = EXIT_FAILURE;
    static const struct option options[] = {
        { "output", required_argument, NULL, 'o' },
        { "device", required_argument, NULL, 'd' },
        { "speed", required_argument, NULL, 's' },
        { "page", required_argument, NULL, 'p' },
        { "title", required_argument, NULL, 't' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };

    if (argc < 2) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    if (strcmp(argv[1], "render") == 0)
        command = COMMAND_RENDER;
    else if (strcmp(argv[1], "display") == 0)
        command = COMMAND_DISPLAY;
    else if (strcmp(argv[1], "clear") == 0)
        command = COMMAND_CLEAR;
    else if (strcmp(argv[1], "probe") == 0)
        command = COMMAND_PROBE;
    else {
        fprintf(stderr, "unknown command: %s\n", argv[1]);
        usage(stderr);
        return EXIT_FAILURE;
    }

    optind = 2;
    while ((option = getopt_long(argc, argv, "o:d:s:p:t:h", options, NULL)) != -1) {
        switch (option) {
        case 'o':
            output_path = optarg;
            break;
        case 'd':
            spi_device = optarg;
            break;
        case 's':
            if (parse_speed(optarg, &speed_hz) < 0) {
                fprintf(stderr, "invalid SPI speed: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'p':
            if (parse_size(optarg, &page) < 0) {
                fprintf(stderr, "invalid page: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 't':
            title = optarg;
            break;
        case 'h':
            usage(stdout);
            return EXIT_SUCCESS;
        default:
            usage(stderr);
            return EXIT_FAILURE;
        }
    }
    if (optind < argc)
        input_path = argv[optind++];
    if (optind != argc) {
        fprintf(stderr, "too many positional arguments\n");
        return EXIT_FAILURE;
    }

    if (command == COMMAND_PROBE)
        return probe_hardware(spi_device) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

    if (command == COMMAND_CLEAR) {
        struct panel panel;

        if (input_path) {
            fprintf(stderr, "clear does not accept an input file\n");
            return EXIT_FAILURE;
        }
        if (panel_open(&panel, spi_device, speed_hz) < 0) {
            panel_close(&panel);
            return EXIT_FAILURE;
        }
        result = panel_display(&panel, NULL) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        panel_close(&panel);
        return result;
    }

    input = read_text(input_path, &input_length);
    if (!input)
        goto cleanup;
    framebuffer = malloc(FRAMEBUFFER_BYTES);
    if (!framebuffer) {
        fprintf(stderr, "cannot allocate framebuffer\n");
        goto cleanup;
    }
    if (render_text(framebuffer, input, input_length, title, page, &page_count) < 0) {
        if (errno == ENOMEM)
            fprintf(stderr, "cannot allocate text layout\n");
        goto cleanup;
    }

    if (command == COMMAND_RENDER) {
        if (write_pbm(output_path, framebuffer) < 0)
            goto cleanup;
        fprintf(stderr, "rendered page %zu/%zu (%dx%d)\n", page + 1,
                page_count, PANEL_WIDTH, PANEL_HEIGHT);
    } else {
        struct panel panel;

        if (panel_open(&panel, spi_device, speed_hz) < 0) {
            panel_close(&panel);
            goto cleanup;
        }
        if (panel_display(&panel, framebuffer) < 0) {
            panel_close(&panel);
            goto cleanup;
        }
        panel_close(&panel);
    }
    result = EXIT_SUCCESS;

cleanup:
    free(framebuffer);
    free(input);
    return result;
}
