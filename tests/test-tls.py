#!/usr/bin/env python3
"""Check bounded CA loading against the exact Mbed TLS used by the board."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from package import BUILDROOT_OUT

# Dead-section elimination lets this exercise the actual callback without a
# Wasm interpreter or a provider. Compare every DER certificate, not just count.
HARNESS = r'''
#define FX_WAMR_MBEDTLS_STREAM_CA 1
#include "runtime/fx_wamr_native.c"
int main(int argc, char **argv) {
    struct request_trust trust = {0};
    mbedtls_ssl_config config;
    mbedtls_x509_crt reference, *a, *b;
    int result;
    if (argc != 3) return 2;
    trust.path = argv[1];
    mbedtls_ssl_config_init(&config);
    mbedtls_x509_crt_init(&trust.roots);
    mbedtls_x509_crt_init(&reference);
    result = load_trust_roots(NULL, &config, &trust);
    if (strcmp(argv[2], "reject") == 0) {
        result = result == CURLE_SSL_CACERT_BADFILE ? 0 : 1;
    } else {
        if (result || mbedtls_x509_crt_parse_file(&reference, argv[1])) return 1;
        a = &reference; b = &trust.roots;
        while (a && b) {
            if (a->raw.len != b->raw.len || memcmp(a->raw.p, b->raw.p, a->raw.len))
                return 1;
            a = a->next; b = b->next;
        }
        result = a != NULL || b != NULL || config.ca_chain != &trust.roots;
    }
    mbedtls_x509_crt_free(&trust.roots);
    mbedtls_x509_crt_free(&reference);
    mbedtls_ssl_config_free(&config);
    return result;
}
'''


class TrustTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = BUILDROOT_OUT / "build/mbedtls-2.28.8"
        if not source.is_dir():
            raise RuntimeError("Run full prep first to fetch the pinned Linux TLS sources")
        build = ROOT / "build/mbedtls-host"
        subprocess.run(["cmake", "-S", str(source), "-B", str(build),
                        "-DENABLE_TESTING=OFF", "-DENABLE_PROGRAMS=OFF",
                        "-DCMAKE_BUILD_TYPE=MinSizeRel"], check=True)
        subprocess.run(["cmake", "--build", str(build), "--parallel", "4"], check=True)
        cls.scratch = tempfile.TemporaryDirectory(prefix="fx-tls-")
        cls.addClassCleanup(cls.scratch.cleanup)
        cls.directory = Path(cls.scratch.name)
        harness = cls.directory / "trust.c"
        harness.write_text(HARNESS)
        cls.binary = cls.directory / "trust"
        subprocess.run(["cc", "-std=c11", "-Os", "-ffunction-sections", "-fdata-sections",
                        "-I" + str(ROOT), "-I" + str(source / "include"),
                        "-I" + str(ROOT / "third_party/wasm-micro-runtime/core/iwasm/include"),
                        "-I" + str(ROOT / "third_party/wasm-micro-runtime/core/shared/platform/include"),
                        str(harness), "-Wl,--gc-sections", "-L" + str(build / "library"),
                        "-lmbedtls", "-lmbedx509", "-lmbedcrypto", "-o", str(cls.binary)], check=True)
        cls.bundle = (BUILDROOT_OUT.parent / "buildroot/board/espressif/esp32s3/"
                      "rootfs_overlay/usr/share/ca-certificates/ca-bundle.crt").read_bytes()
        cls.cert = re.search(rb"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----\n",
                             cls.bundle, re.S)[0]

    def check(self, data, expected):
        path = self.directory / "roots.pem"
        path.write_bytes(data)
        subprocess.run([str(self.binary), str(path), expected], check=True)

    def test_preserves_every_trust_root(self):
        for data in (self.bundle, self.cert, self.cert.replace(b"\n", b"\r\n"),
                     self.cert.rstrip(b"\n")):
            with self.subTest(bytes=len(data)):
                self.check(data, "accept")

    def test_rejects_invalid_or_incomplete_bundles(self):
        for data in (b"", self.cert.split(b"-----END")[0],
                     b"-----BEGIN CERTIFICATE-----\nBAD\n-----END CERTIFICATE-----\n",
                     b"-----BEGIN CERTIFICATE-----\n" + b"A" * 8192,
                     self.cert + b"-----BEGIN CERTIFICATE-----\nBAD\n"):
            with self.subTest(bytes=len(data)):
                self.check(data, "reject")


if __name__ == "__main__":
    unittest.main()
