"""Apply the E1001 board configuration to the locked Linux build recipe."""
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parent.parent


def apply(tree):
    tree = Path(tree)
    board = tree / "new-files/board/espressif/esp32s3"
    config = board / "devkit_c1_16m_linux.config"
    text = config.read_text()
    for patch in sorted((ROOT / "board/kernel").glob("*.patch")):
        shutil.copyfile(patch, board / "patches/linux" / patch.name)
    settings = {
        "BUILTIN_DTB_SOURCE": '"esp32s3-reterminal-e1001"', "SPI_SPIDEV": "y",
        "SLUB_TINY": "y", "MMC": "y", "MMC_SPI": "y", "BASE_SMALL": "y",
        "LOG_BUF_SHIFT": "15", "PERF_EVENTS": None, "INET_TABLE_PERTURB_ORDER": "8",
        "INET_DIAG": None, "PACKET_DIAG": None, "UNIX_DIAG": None, "IPV6": None,
        "NETFILTER": None,
    }
    for key, value in settings.items():
        symbol = "CONFIG_" + key
        text = re.sub(r"^(?:# )?" + symbol + r"(?:=| is not set).*\n?", "", text, flags=re.M)
        text += f"{symbol}={value}\n" if value is not None else f"# {symbol} is not set\n"
    config.write_text(text)

    def edit(relative, replacements, required):
        path = tree / relative
        value = path.read_text()
        for before, after in replacements:
            if after not in value:
                value = value.replace(before, after)
        for marker in required:
            if marker not in value:
                raise RuntimeError(f"Unexpected upstream build recipe: {relative}")
        path.write_text(value)

    options = "detect_leaks=0:abort_on_error=1:halt_on_error=1:handle_segv=0"
    edit("build/container-build.sh",
         [('--output "$work/artifacts/rootfs.cramfs"', '--output "$work/artifacts/rootfs.cramfs" --replace')],
         ['--output "$work/artifacts/rootfs.cramfs" --replace'])
    edit("experiments/mmu-poc/test-host.sh",
         [("ASAN_OPTIONS=detect_leaks=0 ", f"ASAN_OPTIONS='{options}' "), ("-g -O1 ", "-g -O1 -no-pie ")],
         ["handle_segv=0", "-no-pie"])
    edit("experiments/mmu-poc/fork/test-reclaim.py",
         [("'ASAN_OPTIONS': 'detect_leaks=0'", f"'ASAN_OPTIONS': '{options}'"),
          ("halt_on_error=1')", "halt_on_error=1:handle_segv=0')"),
          ("'-fsanitize=address,undefined', '-g'", "'-fsanitize=address,undefined', '-no-pie', '-g'")],
         ["handle_segv=0", "'-no-pie'"])
    edit("experiments/mmu-poc/programs/test-bash-pid-cache.py",
         [('"-fsanitize=address,undefined", "-o"', '"-fsanitize=address,undefined", "-no-pie", "-o"'),
          ('"ASAN_OPTIONS": "detect_leaks=0"', f'"ASAN_OPTIONS": "{options}"')],
         ["handle_segv=0", '"-no-pie"'])
