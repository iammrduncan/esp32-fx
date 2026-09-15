#!/usr/bin/env python3
"""Deployment guards: validate image, isolate module loading, and never reuse a bad backup."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import cli


class LoadTests(unittest.TestCase):
    def test_image_size_and_digest_checked_before_board_access(self):
        with tempfile.TemporaryDirectory() as scratch:
            image = Path(scratch) / "image.bin"
            image.write_bytes(b"\xff" * (16*1024*1024))
            manifest = {"files": {image.name: {"sha256": hashlib.sha256(image.read_bytes()).hexdigest()}}}
            (image.parent / "manifest.json").write_text(json.dumps(manifest))
            cli.validate_image(image)
            with image.open("r+b") as stream:
                stream.write(b"\0")
            with self.assertRaises(RuntimeError):
                cli.validate_image(image)

    def test_module_load_never_installs_or_reconfigures_host(self):
        with tempfile.TemporaryDirectory() as scratch:
            module = Path(scratch) / "app.wasm"
            module.write_bytes(b"\0asm\1\0\0\0")
            with patch.object(cli, "serial_port", return_value="device"), \
                 patch.object(cli, "login"), patch.object(cli, "install") as install, \
                 patch("network.configure") as configure, patch("deploy.load") as upload:
                self.assertEqual(cli.main(["load", str(module), "--storage", "ram"]), 0)
                upload.assert_called_once_with("device", module, "ram", "app")
                install.assert_not_called()
                configure.assert_not_called()

    def test_invalid_backup_prevents_flash_write(self):
        with tempfile.TemporaryDirectory() as scratch:
            private = Path(scratch)
            identity = ":".join(["01"]*6)
            (private / ("backup-" + identity.replace(":", "") + ".bin")).write_bytes(b"incomplete")
            with patch.object(cli, "PRIVATE", private), patch.object(cli, "validate_image"), \
                 patch.object(cli, "esptool", return_value="Detected flash size: 32MB\nMAC: " + identity) as flash:
                with self.assertRaises(RuntimeError):
                    cli.install("device", private / "absent.env")
                self.assertEqual(flash.call_count, 1)
                self.assertEqual(flash.call_args.args[1], "flash-id")


if __name__ == "__main__":
    unittest.main()
