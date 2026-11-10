#!/usr/bin/env python3
"""Build a checked GDI/QML wsplugin without changing its installation identity.

GDI: python tools/package.py --tag gdi/v0.7.5
QML (host Qt): python tools/package.py --exe build/bin/envhosts.exe --tag qml/v0.8.1
QML (bundled Qt): python tools/package.py --runtime-dir build-qml/deploy --tag qml/v0.8.0
Versions come only from plugin.json; edition.json chooses the backend.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import re
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAX_UNPACKED = 256 << 20
MAX_PACKAGE = 64 << 20
FIXED_TIME = (2020, 1, 1, 0, 0, 0)


def changelog_section(version: str) -> str:
    text = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    match = re.search(r"^## \[" + re.escape(version) + r"\][^\n]*\n(.*?)(?=^## |\Z)", text, re.M | re.S)
    if not match:
        raise ValueError(f"CHANGELOG.md has no [{version}] entry")
    return match[1].strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag")
    parser.add_argument("--out", type=Path, default=ROOT / "dist")
    parser.add_argument("--runtime-dir", type=Path)
    parser.add_argument("--exe", type=Path)
    args = parser.parse_args()
    manifest = json.loads((ROOT / "plugin.json").read_text(encoding="utf-8-sig"))
    edition = json.loads((ROOT / "edition.json").read_text(encoding="utf-8"))
    backend, version, pid = edition["backend"], manifest["version"], manifest["id"]
    if backend not in ("gdi", "qml") or pid != "ra9fael.envhosts" or not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise ValueError("Invalid edition, shared plugin ID or three-part version")
    if args.tag and args.tag != f"{backend}/v{version}":
        raise ValueError(f"Expected tag {backend}/v{version}")
    notes = changelog_section(version)
    program = manifest["runtime"]["program"]
    if program != "envhosts.exe":
        raise ValueError("Unexpected runtime.program")
    paths = {name: ROOT / name for name in ("plugin.json", "edition.json", "LICENSE", "README.md", "README.en.md")}
    paths["DUAL_EDITIONS.md"] = ROOT / "docs/DUAL_EDITIONS.md"
    paths["app.ico"] = ROOT / "app.ico"
    if backend == "qml":
        if manifest["runtime"].get("qt") == "host":
            if args.runtime_dir:
                raise ValueError("Host-Qt QML packages must not bundle Qt runtime files")
            exe = (args.exe or ROOT / "build" / "bin" / program).resolve(strict=True)
            paths[program] = exe
        else:
            if not args.runtime_dir or args.exe:
                raise ValueError("QML requires --runtime-dir containing the matching exe and Qt DLLs")
            runtime = args.runtime_dir.resolve(strict=True)
            required = (program, "envhosts.exe", "Qt6Qml.dll", "Qt6Quick.dll", "platforms/qwindows.dll",
                        "qml/QtQuick/Controls/Basic/qmldir", "licenses/qt/LICENSE")
            for name in required:
                if not (runtime / name).is_file():
                    raise ValueError(f"Missing QML runtime file: {name}")
            for path in runtime.rglob("*"):
                if path.is_symlink():
                    raise ValueError("Runtime must contain real files, not symbolic links")
                if path.is_file():
                    name = path.relative_to(runtime).as_posix()
                    if name not in paths:
                        paths[name] = path
            exe = runtime / program
    else:
        if args.runtime_dir:
            raise ValueError("GDI packages must not bundle Qt runtime files")
        exe = (args.exe or ROOT / program).resolve(strict=True)
        paths[program] = exe
    # This is our own built binary. No host, GUI, registry or protocol is started.
    result = subprocess.run([str(exe), "--build-info"], cwd=exe.parent, check=True,
                            capture_output=True, text=True, timeout=15)
    identity = json.loads(result.stdout)
    if identity != {"id": pid, "version": version, "backend": backend}:
        raise ValueError(f"Executable does not match manifest/edition: {identity}")
    name = f"{pid}{'-qml' if backend == 'qml' else ''}-{version}.wsplugin"
    target = args.out / name
    if target.exists() or target.with_suffix(target.suffix + ".sha256").exists():
        raise ValueError("Published packages are immutable; use a new output directory or version")
    sizes = sum(path.stat().st_size for path in paths.values())
    if sizes > MAX_UNPACKED:
        raise ValueError("Unpacked package exceeds 256 MiB")
    args.out.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(target, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in sorted(paths):
            info = zipfile.ZipInfo(name, FIXED_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(info, paths[name].read_bytes(), compresslevel=9)
    if target.stat().st_size > MAX_PACKAGE:
        target.unlink()
        raise ValueError("Package exceeds 64 MiB")
    checksum = hashlib.sha256(target.read_bytes()).hexdigest()
    target.with_suffix(target.suffix + ".sha256").write_text(f"{checksum}  {target.name}\n", encoding="ascii")
    notice = (f"# {backend.upper()} {version}\n\n"
              "同一个插件 ID；安装另一版会替换当前版，设置和备份保留，草稿按版本类型隔离。\n"
              "Both editions share one plugin ID. Installing the other replaces this edition. "
              "Settings/backups are retained; drafts are stored separately per backend.\n\n"
              "下载 / Downloads: [GDI](https://github.com/ra9fael/winshun-envhosts/releases/latest) · "
              "[QML](https://github.com/ra9fael/winshun-envhosts/tree/qml#安装)\n\n"
              "插件库目前不区分后端；QML 请从 QML 发布页更新。\n"
              "The plugin library does not track backends; update QML from its release page.\n\n")
    (args.out / "release-notes.md").write_text(notice + notes + f"\n\nSHA-256 `{checksum}`\n", encoding="utf-8")
    print(f"{target}\nbackend={backend} version={version} size={target.stat().st_size} sha256={checksum}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        sys.exit(f"error: {error}")
