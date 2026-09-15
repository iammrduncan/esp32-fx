#!/usr/bin/env python3
"""Serial framing must ignore echoed commands and preserve the target exit code."""
import base64
import hashlib
import os
from pathlib import Path
import pty
import re
import select
import sys
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from board_io import Console, download_root


class SerialTests(unittest.TestCase):
    def test_echo_is_not_completion(self):
        master, slave = pty.openpty()
        console = Console(os.ttyname(slave))
        results = []
        worker = threading.Thread(target=lambda: results.append(console.run("false", timeout=3)))
        try:
            worker.start()
            wire = b""
            while not re.search(rb'__FX_CMD_[0-9a-f]+=%s', wire):
                self.assertTrue(select.select([master], [], [], 3)[0])
                wire += os.read(master, 4096)
            marker = re.search(rb"__FX_CMD_[0-9a-f]+", wire)[0]
            os.write(master, wire)
            self.assertFalse(results)
            os.write(master, b"\r\n" + marker + b"=1\r\n")
            worker.join(timeout=4)
            self.assertFalse(worker.is_alive())
            self.assertEqual(results[0][0], 1)
        finally:
            worker.join(timeout=4)
            console.close()
            os.close(master)
            os.close(slave)

    def test_download_paths_stay_in_working_directory(self):
        self.assertEqual(download_root("/media/sd/fx-coding/run/stdout.txt"), "/media/sd/fx-coding")
        for path in ("/etc/shadow", "/media/sd/fx-coding/../secret", "/media/sd/fx-coding/"):
            with self.assertRaises(ValueError):
                download_root(path)

    def test_download_empty_and_nonempty_files(self):
        for data in (b"", b"a binary log\x00\xff\n"):
            with self.subTest(data=data):
                master, slave = pty.openpty()
                console = Console(os.ttyname(slave))
                results = []
                worker = threading.Thread(target=lambda: results.append(
                    console.download("/media/sd/fx-coding/run/stdout.txt", maximum=1024)))

                def receive(pattern):
                    wire = b""
                    while not re.search(pattern, wire):
                        self.assertTrue(select.select([master], [], [], 3)[0])
                        wire += os.read(master, 4096)
                    return wire

                try:
                    worker.start()
                    wire = receive(rb"__FX_CMD_[0-9a-f]+=%s.*\n")
                    marker = re.search(rb"__FX_CMD_[0-9a-f]+", wire)[0]
                    os.write(master, b"\n" + marker + b"=0\n")
                    wire = receive(rb"__FX_DOWNLOAD_END_[0-9a-f]+.*\n")
                    begin = re.search(rb"__FX_DOWNLOAD_[0-9a-f]+", wire)[0]
                    end = re.search(rb"__FX_DOWNLOAD_END_[0-9a-f]+", wire)[0]
                    payload = base64.b64encode(data) + b"\n" if data else b""
                    os.write(master, b"\n" + begin + b" " + str(len(data)).encode()
                             + b" " + hashlib.sha256(data).hexdigest().encode()
                             + b"\n" + payload + end + b"\n")
                    worker.join(timeout=4)
                    self.assertFalse(worker.is_alive())
                    self.assertEqual(results, [data])
                finally:
                    worker.join(timeout=4)
                    console.close()
                    os.close(master)
                    os.close(slave)


if __name__ == "__main__":
    unittest.main()
