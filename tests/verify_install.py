#!/usr/bin/env python3
"""Verify a .wsplugin through the real user-install path, not a dev junction.

Workflow:
  1. Inspect the archive: manifest at the root, matching id/version, the
     runtime exe present, no path traversal, WinShun's size limits respected.
  2. Refuse to run if the target plugin folder is a junction or symlink. A
     junction makes the host load the checkout directly, so it cannot catch a
     missing or stale file in the package. This script never removes a link;
     remove it yourself with `cmd /c rmdir <link>` (link only, not its target).
  3. Unpack the archive into %LOCALAPPDATA%\\WinShun\\plugins\\<id> exactly as
     the host's installer would, then run tests/integration.py and
     tests/gui_preview.py --no-hold against that installed exe.
  4. Both child tests use isolated HKCU subkeys and a scratch hosts file, so
     the real Environment key and the real hosts file are never written.

The script never deletes an existing plugin folder. Use --reuse to verify an
already unpacked installation, or remove/rename that folder yourself first.

Standard library only, Python 3.10+, Windows.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+$")
MAX_PACKAGE = 64 << 20      # WinShun-plugin registry.MAX_PACKAGE (Gallery.h)
MAX_UNPACKED = 256 << 20    # WinShun-plugin registry.MAX_UNPACKED (Install.h)
MAX_FILE = 64 << 20
FILE_ATTRIBUTE_REPARSE_POINT = 0x400
INVALID_FILE_ATTRIBUTES = 0xFFFFFFFF

ERRORS: list[str] = []


def fail(msg: str) -> None:
    ERRORS.append(msg)
    print(f"  FAIL  {msg}", flush=True)


def check(cond: bool, label: str, extra: str = "") -> None:
    if cond:
        print(f"  PASS  {label}", flush=True)
    else:
        fail(f"{label}{(' -- ' + extra) if extra else ''}")


def is_link(path: Path) -> bool:
    """Junctions are not reliably reported by Path.is_symlink() on Windows."""
    if not path.exists():
        return False
    attrs = ctypes.windll.kernel32.GetFileAttributesW(str(path))
    if attrs == INVALID_FILE_ATTRIBUTES:
        return False
    return bool(attrs & FILE_ATTRIBUTE_REPARSE_POINT)


def default_plugin_dir(plugin_id: str) -> Path:
    local = os.environ.get("LOCALAPPDATA")
    if not local:
        raise RuntimeError("LOCALAPPDATA is not set")
    return Path(local) / "WinShun" / "plugins" / plugin_id


def inspect_archive(package: Path) -> dict:
    print(f"== ARCHIVE  {package}", flush=True)
    data = package.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    print(f"   sha256 {digest}  ({len(data)} bytes)", flush=True)
    check(len(data) <= MAX_PACKAGE, "archive within the 64 MiB limit",
          f"{len(data)} bytes")

    with zipfile.ZipFile(package) as z:
        names = z.namelist()
        total = 0
        unsafe = []
        for info in z.infolist():
            name = info.filename
            if (name.startswith("/") or name.startswith("\\")
                    or re.match(r"^[A-Za-z]:", name)
                    or ".." in Path(name.replace("\\", "/")).parts):
                unsafe.append(name)
            total += info.file_size
            if info.file_size > MAX_FILE:
                fail(f"single file over 64 MiB: {name}")
        check(not unsafe, "archive entry names are all safe", ", ".join(unsafe[:5]))
        check("plugin.json" in names, "plugin.json is at the archive root")
        check(total <= MAX_UNPACKED, "unpacked size within the 256 MiB limit",
              f"{total} bytes")
        if "plugin.json" not in names:
            return {}
        manifest = json.loads(z.read("plugin.json").decode("utf-8"))

    pid = manifest.get("id", "")
    version = manifest.get("version", "")
    program = str(manifest.get("runtime", {}).get("program", ""))
    minimum = str(manifest.get("minWinShun", ""))
    check(pid == "ra9fael.envhosts", "manifest id", pid)
    check(bool(VERSION_RE.fullmatch(version)), "three-part version", version)
    check(program.lower().endswith(".exe") and program in names,
          "runtime.program is an exe present in the package", program)
    check(minimum == "0.22.0", "minWinShun is the unreviewed-native floor", minimum)
    print(f"   {len(names)} files, {total} bytes unpacked, version {version}", flush=True)
    return manifest


def install(package: Path, target: Path, reuse: bool) -> bool:
    print(f"== INSTALL  {target}", flush=True)
    if is_link(target):
        fail("target is a junction/symlink -- refuse to test a development link; "
             "remove the link with `cmd /c rmdir \"%s\"` (link only)" % target)
        return False
    if target.exists():
        if not target.is_dir():
            fail("target exists and is not a directory")
            return False
        if any(target.iterdir()):
            if not reuse:
                fail("target is a non-empty directory; pass --reuse to verify it "
                     "as-is, or remove/rename it yourself first")
                return False
            print("   --reuse: verifying the existing installation", flush=True)
            return True
    target.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(package) as z:
        entries = z.infolist()
        for info in entries:
            z.extract(info, target)
    print(f"   unpacked {len(entries)} files", flush=True)
    return True


def verify_installed(target: Path, manifest: dict, skip_gui: bool,
                     out_dir: Path) -> None:
    exe = target / str(manifest.get("runtime", {}).get("program", ""))
    print(f"== INSTALLED  {exe}", flush=True)
    check(exe.is_file(), "installed executable exists")
    if not exe.is_file():
        return
    check((target / "plugin.json").is_file(), "installed manifest exists")
    installed = json.loads((target / "plugin.json").read_text(encoding="utf-8"))
    check(installed.get("version") == manifest.get("version"),
          "installed manifest matches the package")

    env = dict(os.environ)
    env["WS_ENVHOSTS_EXE"] = str(exe)
    env["WS_ENVHOSTS_PLUGIN_DIR"] = str(target)
    env["WS_ENVHOSTS_OUTDIR"] = str(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    steps = [("protocol integration", [sys.executable, str(HERE / "integration.py")])]
    if not skip_gui:
        steps.append(("GUI assertions", [sys.executable, str(HERE / "gui_preview.py"),
                                         "--no-hold"]))
    for label, cmd in steps:
        print(f"== RUN  {label}", flush=True)
        try:
            proc = subprocess.run(cmd, cwd=str(ROOT), env=env, timeout=900,
                                  capture_output=True, text=True,
                                  encoding="utf-8", errors="replace")
        except subprocess.TimeoutExpired:
            fail(f"{label} timed out after 900 s")
            continue
        tail = (proc.stdout or "").strip().splitlines()[-6:]
        for line in tail:
            print("   " + line, flush=True)
        if proc.returncode != 0:
            for line in (proc.stderr or "").strip().splitlines()[-8:]:
                print("   stderr: " + line, flush=True)
        check(proc.returncode == 0, f"{label} passed against the installed exe",
              f"exit {proc.returncode}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--package", help="path to the .wsplugin (default: dist/<id>-<version>)")
    ap.add_argument("--plugin-dir", help="install target (default: %%LOCALAPPDATA%%\\WinShun\\plugins\\<id>)")
    ap.add_argument("--reuse", action="store_true",
                    help="verify an existing non-empty install folder without unpacking")
    ap.add_argument("--skip-gui", action="store_true", help="run only the protocol tests")
    ap.add_argument("--out-dir", help="folder for GUI screenshots (default: a temp folder)")
    args = ap.parse_args()

    manifest_path = ROOT / "plugin.json"
    source_manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    pid, version = source_manifest["id"], source_manifest["version"]
    package = Path(args.package) if args.package else (
        ROOT / "dist" / f"{pid}-{version}.wsplugin")
    if not package.is_file():
        print(f"error: package not found: {package}", file=sys.stderr)
        return 1
    target = Path(args.plugin_dir) if args.plugin_dir else default_plugin_dir(pid)
    out_dir = Path(args.out_dir) if args.out_dir else Path(
        tempfile.mkdtemp(prefix="ws-envhosts-install-verify-"))

    manifest = inspect_archive(package)
    if ERRORS or not manifest:
        print(f"\n== RESULT: {len(ERRORS)} failure(s) ==", flush=True)
        return 1
    if not install(package, target, args.reuse):
        print(f"\n== RESULT: {len(ERRORS)} failure(s) ==", flush=True)
        return 1
    verify_installed(target, manifest, args.skip_gui, out_dir)
    print(f"\n== RESULT: {len(ERRORS)} failure(s), screenshots in {out_dir} ==",
          flush=True)
    return 1 if ERRORS else 0


if __name__ == "__main__":
    sys.exit(main())
