#!/usr/bin/env python3
"""Fetch exact source commits without replacing existing worktrees."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
SOURCES = {
    "fx": "fx", "wamr": "wasm-micro-runtime",
    "linux_xtensa": "linux-xtensa", "seeed_gxepd2": "Seeed_GxEPD2",
    "linux_on_esp32_s3": "Linux-on-esp32-S3",
}


def git(path, *args):
    return subprocess.check_output(["git", "-C", str(path), *args], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify without fetching")
    args = parser.parse_args()
    lock = json.loads((ROOT / "sources.lock.json").read_text())["sources"]
    allocator = lock["dlmalloc"]
    allocator_path = ROOT / "third_party/dlmalloc/malloc.c"
    if not allocator_path.exists():
        if args.check:
            raise SystemExit("missing dlmalloc source; run fetch-sources.py")
        with urllib.request.urlopen(allocator["url"], timeout=30) as response:
            data = response.read(1024 * 1024)
        if hashlib.sha256(data).hexdigest() != allocator["sha256"]:
            raise SystemExit("dlmalloc upstream hash changed; refused")
        allocator_path.parent.mkdir(parents=True, exist_ok=True)
        with allocator_path.open("xb") as output:
            output.write(data)
    if hashlib.sha256(allocator_path.read_bytes()).hexdigest() != allocator["sha256"]:
        raise SystemExit("dlmalloc source digest mismatch; left unchanged")
    print("verified dlmalloc: " + allocator["sha256"])
    for key, directory in SOURCES.items():
        source = lock[key]
        path = ROOT / "third_party" / directory
        if not path.exists():
            if args.check:
                raise SystemExit(f"missing checkout: {path}")
            path.mkdir(parents=True)
            git(path, "init", "-q")
            git(path, "remote", "add", "origin", source["url"])
            git(path, "fetch", "--depth=1", "origin", source["commit"])
            git(path, "checkout", "--detach", "FETCH_HEAD")
        if git(path, "rev-parse", "HEAD") != source["commit"]:
            raise SystemExit(f"wrong revision; left unchanged: {path}")
        # The separately checked WAMR patch is the only permitted source edit.
        dirty = git(path, "status", "--porcelain", "--untracked-files=no")
        if dirty and key != "wamr":
            raise SystemExit(f"modified source; left unchanged: {path}")
        print(f"verified {key}: {source['commit']}")


if __name__ == "__main__":
    main()
