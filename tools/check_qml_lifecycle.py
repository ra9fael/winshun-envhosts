"""Measure open/close cycles using only this probe's process and scratch data."""
import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time
import uuid
import winreg

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from qml_protocol import eventually, memory_mib, registry, windows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("exe", type=Path)
    parser.add_argument("--backend", choices=("default", "software"), default="default")
    parser.add_argument("--fontengine", choices=("default", "gdi", "freetype"), default="default")
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    exe = args.exe.resolve(strict=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    root = rf"Software\EnvHostsQmlTests\{uuid.uuid4()}"
    keys = [root + r"\User", root + r"\System"]
    process = None
    errors = []
    samples = []
    timings = []
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    try:
        for key in keys:
            with winreg.CreateKey(winreg.HKEY_CURRENT_USER, key) as handle:
                winreg.SetValueEx(handle, "PATH_TEST", 0, winreg.REG_EXPAND_SZ, r"%TEMP%;C:\Fixture")
        before = [registry(key) for key in keys]
        with tempfile.TemporaryDirectory(prefix="lifecycle-", dir=args.output.parent) as tmp:
            scratch = Path(tmp)
            hosts = scratch / "hosts"
            source = b"# fixture\r\n127.0.0.1 alpha.test\r\n::1 beta.test\r\n"
            hosts.write_bytes(source)
            env = dict(os.environ, WS_ENVHOSTS_TEST_SUBKEY=keys[0],
                       WS_ENVHOSTS_TEST_SYSTEM_SUBKEY="HKCU\\" + keys[1],
                       WS_ENVHOSTS_TEST_HOSTS=str(hosts))
            env.pop("QT_QUICK_BACKEND", None)
            env.pop("QSG_RHI_BACKEND", None)
            if args.backend == "software":
                env["QT_QUICK_BACKEND"] = "software"
            if args.fontengine != "default":
                env["QT_QPA_PLATFORM"] = "windows:fontengine=" + args.fontengine
            process = subprocess.Popen([str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True, encoding="utf-8", env=env,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            replies = queue.Queue()
            def read_replies():
                for line in process.stdout:
                    try:
                        replies.put(json.loads(line))
                    except ValueError:
                        replies.put({"invalid": line})
            threading.Thread(target=read_replies, daemon=True).start()
            reader = threading.Thread(target=lambda: errors.extend(process.stderr.readlines()), daemon=True)
            reader.start()
            sequence = 0
            def call(method, params):
                nonlocal sequence
                sequence += 1
                process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": sequence, "method": method, "params": params}) + "\n")
                process.stdin.flush()
                end = time.monotonic() + 15
                while True:
                    reply = replies.get(timeout=max(.1, end - time.monotonic()))
                    assert "invalid" not in reply, reply
                    if reply.get("id") == sequence:
                        assert "error" not in reply, reply
                        return reply["result"]
                    assert time.monotonic() < end
            def sample(phase):
                samples.append({"phase": phase, "memory_mib": memory_mib(process)})
            call("initialize", {"pluginDir": str(exe.parent), "dataDir": str(scratch / "data"),
                                "locale": "zh", "theme": "light", "settings": {"showSystemVars": True}})
            sample("startup")
            for cycle in range(args.cycles):
                handles = []
                for kind in ("env", "hosts"):
                    entry = {"data": {"kind": "openEditor", "target": kind}}
                    started = time.perf_counter()
                    result = call("action", {"command": kind, "action": "open", "item": entry})
                    timings.append({"cycle": cycle + 1, "kind": kind,
                                    "reply_ms": round((time.perf_counter() - started) * 1000, 2)})
                    assert any(effect["type"] == "hide" for effect in result["effects"]), result
                    eventually(lambda: len(windows(process.pid)) == (1 if kind == "env" else 2), timeout=15)
                    time.sleep(.6)
                    current = windows(process.pid)
                    handles.append(next(hwnd for hwnd in current if hwnd not in handles))
                    sample(f"cycle-{cycle + 1}-{kind}-opened")
                assert user32.PostMessageW(handles[0], 0x0010, 0, 0)
                eventually(lambda: len(windows(process.pid)) == 1)
                assert windows(process.pid) == [handles[1]], "Closing env also closed hosts"
                assert user32.PostMessageW(handles[1], 0x0010, 0, 0)
                eventually(lambda: not windows(process.pid))
                time.sleep(2)
                sample(f"cycle-{cycle + 1}-all-closed")
            assert [registry(key) for key in keys] == before
            assert hosts.read_bytes() == source
            call("shutdown", {})
            process.wait(timeout=5)
            reader.join(timeout=2)
            assert process.returncode == 0, process.returncode
            assert not any("E_QML_OPEN" in e or ("qrc:" in e and any(word in e for word in ("Error", "is not a type"))) for e in errors), errors
            report = {"status": "PASS", "exe": str(exe), "backend_override": args.backend,
                      "fontengine_override": args.fontengine,
                      "action_timings": timings, "samples": samples, "stderr": errors,
                      "checks": ["both actions return hide", "closing env preserves hosts", "reopen after full close",
                                 "no target writes", "normal protocol shutdown"]}
            args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
            print(json.dumps({k: v for k, v in report.items() if k != "stderr"}, ensure_ascii=False, indent=2))
    finally:
        if process and process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        for key in [*keys, root]:
            try:
                winreg.DeleteKey(winreg.HKEY_CURRENT_USER, key)
            except FileNotFoundError:
                pass


if __name__ == "__main__":
    main()
