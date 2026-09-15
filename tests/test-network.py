#!/usr/bin/env python3
"""Connection reuse must preserve working Wi-Fi and detect changed settings."""
import hashlib
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from network import connect


class NetworkTests(unittest.TestCase):
    def test_reuse_requires_matching_configuration_and_live_route(self):
        ssid, psk = b"test-network", "a" * 64
        config = f"network={{\nssid={ssid.hex()}\npsk={psk}\nkey_mgmt=WPA-PSK\n}}\n"
        digest = hashlib.sha256(config.encode()).hexdigest()
        healthy = digest + "  /etc/wpa_supplicant.conf\nConnected to ap\ninet 192.0.2.2\ndefault via 192.0.2.1\n"
        console = Mock()
        console.run.return_value = (0, healthy)
        connect(console, None, ssid, psk)
        self.assertEqual(console.run.call_count, 1)
        for output in (healthy.replace(digest, "0" * 64),
                       healthy.replace("Connected to ", "Not connected "),
                       healthy.replace("inet ", ""), healthy.replace("default ", "")):
            with self.subTest(output=output):
                console = Mock()
                # Stop at the first configuration write; no board is involved.
                console.run.side_effect = [(0, output), (1, "")]
                with self.assertRaisesRegex(RuntimeError, "cannot create private Wi-Fi"):
                    connect(console, None, ssid, psk)
                self.assertEqual(console.run.call_count, 2)


if __name__ == "__main__":
    unittest.main()
