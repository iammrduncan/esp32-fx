// SPDX-License-Identifier: MIT
/* Mbed TLS public-RSA shim for Linux-on-ESP32-S3's hardware accelerator. */
#define _GNU_SOURCE

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <mbedtls/bignum.h>
#include <mbedtls/rsa.h>

#define ESP_RSA_MAX_BYTES 512U

struct esp_rsa_modexp {
    uint32_t modulus_len;
    uint32_t exponent_len;
    uint32_t input_len;
    uint32_t output_len;
    uint8_t modulus[ESP_RSA_MAX_BYTES];
    uint8_t exponent[ESP_RSA_MAX_BYTES];
    uint8_t input[ESP_RSA_MAX_BYTES];
    uint8_t output[ESP_RSA_MAX_BYTES];
};

#define ESP_RSA_IOC_MODEXP _IOWR('R', 0x53, struct esp_rsa_modexp)

static int
hardware_modexp(struct esp_rsa_modexp *operation)
{
    int device = open("/dev/esp32-rsa", O_RDWR | O_CLOEXEC);
    int result;

    if (device < 0)
        return -1;
    result = ioctl(device, ESP_RSA_IOC_MODEXP, operation);
    close(device);
    return result;
}

/* Executables link this symbol with --export-dynamic so Mbed TLS's PKCS#1
 * verifier delegates only its raw public modular exponentiation. */
int
mbedtls_rsa_public(mbedtls_rsa_context *context, const unsigned char *input,
                   unsigned char *output)
{
    struct esp_rsa_modexp *operation;
    size_t modulus_length;
    size_t exponent_length;
    int result = MBEDTLS_ERR_RSA_PUBLIC_FAILED;

    if (!context || !input || !output)
        return MBEDTLS_ERR_RSA_BAD_INPUT_DATA;
    modulus_length = mbedtls_rsa_get_len(context);
    exponent_length = mbedtls_mpi_size(&context->E);
    if (modulus_length < 64U || modulus_length > ESP_RSA_MAX_BYTES
        || exponent_length == 0U || exponent_length > modulus_length)
        return MBEDTLS_ERR_RSA_BAD_INPUT_DATA;
    operation = calloc(1, sizeof(*operation));
    if (!operation)
        return MBEDTLS_ERR_RSA_PUBLIC_FAILED + MBEDTLS_ERR_MPI_ALLOC_FAILED;
    operation->modulus_len = (uint32_t)modulus_length;
    operation->exponent_len = (uint32_t)exponent_length;
    operation->input_len = (uint32_t)modulus_length;
    if (mbedtls_mpi_write_binary(&context->N, operation->modulus,
                                 modulus_length) != 0
        || mbedtls_mpi_write_binary(&context->E, operation->exponent,
                                    exponent_length) != 0)
        goto out;
    memcpy(operation->input, input, modulus_length);
    if (hardware_modexp(operation) != 0
        || operation->output_len != modulus_length)
        goto out;
    memcpy(output, operation->output, modulus_length);
    result = 0;
out:
    memset(operation, 0, sizeof(*operation));
    free(operation);
    return result;
}
