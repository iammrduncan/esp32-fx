#!/usr/bin/env python3
"""Exercise the overlay against a fresh pinned upstream archive (no compilation)."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from overlay import apply

ROOT = Path(__file__).resolve().parent.parent


class LinuxOverlayTests(unittest.TestCase):
    def test_fresh_checkout_and_idempotence(self):
        source = ROOT / "third_party/Linux-on-esp32-S3"
        self.assertTrue(source.is_dir(), "run tools/fetch.py first")
        with tempfile.TemporaryDirectory(prefix="esp32-overlay-") as directory:
            tree = Path(directory)
            archive = subprocess.Popen(
                ["git", "-C", str(source), "archive", "HEAD"],
                stdout=subprocess.PIPE)
            subprocess.run(["tar", "-x", "--exclude=images", "-C", str(tree)],
                           stdin=archive.stdout, check=True)
            archive.stdout.close()
            self.assertEqual(archive.wait(), 0)
            apply(tree)
            before = {str(p.relative_to(tree)): p.read_bytes()
                      for p in tree.rglob("*") if p.is_file()}
            apply(tree)
            after = {str(p.relative_to(tree)): p.read_bytes()
                     for p in tree.rglob("*") if p.is_file()}
            self.assertEqual(before, after)
            config = (tree / "new-files/board/espressif/esp32s3/"
                      "devkit_c1_16m_linux.config").read_text()
            self.assertIn('CONFIG_BUILTIN_DTB_SOURCE="esp32s3-reterminal-e1001"', config)
            self.assertIn("CONFIG_MMC=y", config)
            self.assertIn("CONFIG_MMC_SPI=y", config)
            self.assertIn("CONFIG_BASE_SMALL=y", config)
            self.assertIn("CONFIG_LOG_BUF_SHIFT=15", config)
            self.assertIn("CONFIG_INET_TABLE_PERTURB_ORDER=8", config)
            self.assertIn("# CONFIG_IPV6 is not set", config)
            self.assertIn("# CONFIG_NETFILTER is not set", config)
            sd_patch = (tree / "new-files/board/espressif/esp32s3/patches/linux/"
                        "05-kernel-reterminal-e1001-sd.patch").read_text()
            self.assertIn("GPIO_FUNC_IN_SEL(102) 0x88", sd_patch)
            self.assertIn('compatible = "mmc-spi-slot"', sd_patch)
            self.assertIn("gpios = <&gpio0 15 GPIO_ACTIVE_LOW>", sd_patch)
            rsa_patch = (tree / "new-files/board/espressif/esp32s3/patches/linux/"
                         "06-kernel-esp32s3-rsa-userspace.patch").read_text()
            self.assertIn('name = "esp32-rsa"', rsa_patch)
            self.assertIn("CAP_SYS_RAWIO", rsa_patch)
            self.assertIn("ESP_RSA_IOC_MODEXP", rsa_patch)
            wamr_patch = (tree / "new-files/board/espressif/esp32s3/patches/linux/"
                          "07-kernel-fx-wamr-reserved-psram.patch").read_text()
            self.assertIn("fx-wamr-arena@3def0000", wamr_patch)
            self.assertIn("reg = <0x3def0000 0x00110000>", wamr_patch)


if __name__ == "__main__":
    unittest.main()
