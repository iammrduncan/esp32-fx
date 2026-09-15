#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location("deploy_wasm", ROOT / "tools/deploy.py")
deploy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deploy)


class DeployWasmTests(unittest.TestCase):
    def test_accepts_bounded_raw_wasm(self):
        with tempfile.TemporaryDirectory() as scratch:
            module = Path(scratch) / "app.wasm"
            module.write_bytes(b"\0asm\x01\0\0\0")
            self.assertEqual(deploy.read_module(module), b"\0asm\x01\0\0\0")

    def test_rejects_non_wasm_and_oversize_metadata(self):
        with tempfile.TemporaryDirectory() as scratch:
            module = Path(scratch) / "bad.wasm"
            module.write_bytes(b"not wasm")
            with self.assertRaises(ValueError):
                deploy.read_module(module)
            module.write_bytes(b"\0asm\x01\0\0\0")
            with module.open("r+b") as stream:
                stream.truncate(deploy.MAX_MODULE_BYTES + 1)
            with self.assertRaises(ValueError):
                deploy.read_module(module)

    def test_paths_are_content_addressed_and_in_fixed_roots(self):
        digest = "ab" * 32
        self.assertEqual(
            deploy.deployment_path("ram", "demo_1", digest),
            f"/tmp/microwasm/apps/demo_1/{digest}.wasm",
        )
        self.assertEqual(
            deploy.deployment_path("sd", "demo-1", digest),
            f"/media/sd/microwasm/apps/demo-1/{digest}.wasm",
        )
        for storage, name in (("flash", "demo"), ("sd", "../demo"),
                              ("ram", "UPPER"), ("ram", "")):
            with self.subTest(storage=storage, name=name), self.assertRaises(ValueError):
                deploy.deployment_path(storage, name, digest)


if __name__ == "__main__":
    unittest.main()
