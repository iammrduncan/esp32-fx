#!/usr/bin/env python3
"""Exercise the native capability boundary without WAMR, a board, or credentials."""
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class ExampleToolsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="fx-tools-build-")
        library = Path(cls.build.name) / "tools.so"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                        str(ROOT / "runtime/fx_example_tools.c"), "-o", str(library)], check=True)
        cls.library = ctypes.CDLL(str(library))
        cls.call = cls.library.fx_example_tool_call
        cls.call.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_char_p,
                             ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32,
                             ctypes.POINTER(ctypes.c_uint8)]
        cls.call.restype = ctypes.c_int32

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="fx-tools-test-")
        self.addCleanup(self.scratch.cleanup)
        self.jail = Path(self.scratch.name)
        self.project = self.jail / "project"
        shutil.copytree(ROOT / "example/project", self.project)
        self.previous = os.environ.get("FX_EXAMPLE_JAIL")
        os.environ["FX_EXAMPLE_JAIL"] = str(self.jail)
        self.addCleanup(self.restore_env)

    def restore_env(self):
        if self.previous is None:
            os.environ.pop("FX_EXAMPLE_JAIL", None)
        else:
            os.environ["FX_EXAMPLE_JAIL"] = self.previous

    def invoke(self, name, arguments, raw=False):
        arguments = arguments if raw else json.dumps(arguments).encode()
        out = ctypes.create_string_buffer(16384)
        status = ctypes.c_uint8(99)
        count = self.call(name.encode(), len(name), arguments, len(arguments), out,
                          len(out), ctypes.byref(status))
        return count, status.value, out.raw[:max(0, count)].decode()

    def test_read_and_atomic_write(self):
        for name in ("battery.sh", "SPEC.md", "test.sh"):
            count, status, text = self.invoke("read_project", {"path": name})
            self.assertGreater(count, 0)
            self.assertEqual(status, 0)
            self.assertEqual(text, (self.project / name).read_text())
        source = 'battery_status() { BATTERY_STATUS="ok"; }\n'
        _, status, text = self.invoke("replace_battery", {"path": "battery.sh", "content": source})
        self.assertEqual((status, text), (0, "wrote battery.sh\n"))
        self.assertEqual((self.project / "battery.sh").read_text(), source)

    def test_denied_paths_and_tests_immutable(self):
        for name in ("../battery.sh", "/etc/shadow", ".env", "project/battery.sh", "battery.sh/..", ""):
            self.assertEqual(self.invoke("read_project", {"path": name})[1], 1)
        for name in ("test.sh", "SPEC.md", "../battery.sh"):
            self.assertEqual(self.invoke("replace_battery", {"path": name, "content": "oops"})[1], 1)
        self.assertEqual(self.invoke("exec", {"command": "true"})[1], 1)

    def test_strict_json_and_bounds(self):
        for raw in (b'{"path":"battery.sh","path":"test.sh"}', b'{"path":"battery.sh\\u0000"}',
                    b'{"path":"battery.sh",}', b'{"path":"battery.sh"} trailing',
                    b'{"path":"battery.sh","extra":1}', b'{"path":"battery.sh"'):
            self.assertEqual(self.invoke("read_project", raw, raw=True)[1], 1)
        self.assertEqual(self.invoke("replace_battery", {"path": "battery.sh", "content": "x" * 4097})[1], 1)
        self.assertEqual(self.invoke("run_tests", {"command": "true"})[1], 1)

    def test_links_and_nonregular_files_denied(self):
        battery = self.project / "battery.sh"
        battery.unlink()
        battery.symlink_to(self.project / "test.sh")
        self.assertEqual(self.invoke("read_project", {"path": "battery.sh"})[1], 1)
        self.assertEqual(self.invoke("replace_battery", {"path": "battery.sh", "content": "x"})[1], 1)
        battery.unlink()
        os.link(self.project / "test.sh", battery)
        self.assertEqual(self.invoke("read_project", {"path": "battery.sh"})[1], 1)
        battery.unlink()
        os.mkfifo(battery)
        self.assertEqual(self.invoke("read_project", {"path": "battery.sh"})[1], 1)

    def test_disabled_by_default(self):
        os.environ.pop("FX_EXAMPLE_JAIL")
        self.assertEqual(self.invoke("read_project", {"path": "battery.sh"})[1], 1)

    @unittest.skipUnless(os.geteuid() == 0, "requires isolated container root")
    def test_real_helper_isolation_and_limits(self):
        helper = Path(self.build.name) / "fx-example-test"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "runtime/fx_example_test.c"), "-o", str(helper)], check=True)
        actual_jail = self.jail / "isolated"
        subprocess.run(["/bin/sh", str(ROOT / "example/prepare-jail.sh"),
                        str(ROOT / "example/project"), str(actual_jail)], check=True,
                       stdout=subprocess.DEVNULL)
        os.environ["FX_EXAMPLE_JAIL"] = str(actual_jail)
        old_helper = os.environ.get("FX_EXAMPLE_TEST_HELPER")
        old_key = os.environ.get("AI_GATEWAY_API_KEY")
        os.environ["FX_EXAMPLE_TEST_HELPER"] = str(helper)
        os.environ["AI_GATEWAY_API_KEY"] = "isolation-test-sentinel"
        try:
            self.assertIn("EXAMPLE_FAIL checks=14 failures=4", self.invoke("run_tests", {})[2])
            battery = actual_jail / "project/battery.sh"
            repaired = (ROOT / "example/project/battery.sh").read_text().replace(
                '"$1" -le 20', '"$1" -lt 20').replace('"$1" -gt 80', '"$1" -ge 80')
            probes = '''
test -z "${AI_GATEWAY_API_KEY+x}" || exit 91
test ! -e /etc/shadow || exit 92
test ! -e /proc/self/environ || exit 93
test ! -w /project/battery.sh || exit 94
test ! -w /project/test.sh || exit 95
test ! -w /project || exit 96
'''
            battery.write_text(probes + repaired)
            _, status, text = self.invoke("run_tests", {})
            self.assertEqual(status, 0, text)
            self.assertIn("EXAMPLE_PASS checks=14", text)
            battery.write_text("exec 1>&- 2>&-\nwhile :; do :; done\n")
            self.assertEqual(self.invoke("run_tests", {})[1], 1)
            battery.write_text("while :; do printf '%080d\\n' 0; done\n")
            self.assertEqual(self.invoke("run_tests", {})[1], 1)
        finally:
            if old_key is None:
                os.environ.pop("AI_GATEWAY_API_KEY", None)
            else:
                os.environ["AI_GATEWAY_API_KEY"] = old_key
            if old_helper is None:
                os.environ.pop("FX_EXAMPLE_TEST_HELPER", None)
            else:
                os.environ["FX_EXAMPLE_TEST_HELPER"] = old_helper


if __name__ == "__main__":
    unittest.main()
