"""Exercise the packaged QML executable with isolated registry/file targets.

Never starts WinShun or touches the real Environment/hosts. Python stdlib only.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import time
import uuid
import winreg


def registry(key):
    result = {}
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key) as handle:
        for i in range(winreg.QueryInfoKey(handle)[1]):
            name, value, kind = winreg.EnumValue(handle, i)
            result[name] = (value, kind)
    return result


def windows(pid):
    result = []
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    @callback_type
    def visit(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            result.append(hwnd)
        return True
    user32.EnumWindows(visit, 0)
    return result


def memory_mib(process):
    class Counters(ctypes.Structure):
        _fields_ = [("cb",wintypes.DWORD),("faults",wintypes.DWORD)] + [(name,ctypes.c_size_t) for name in
            ("peak_working","working","peak_paged","paged","peak_nonpaged","nonpaged","pagefile","peak_pagefile","private")]
    counters=Counters();counters.cb=ctypes.sizeof(counters)
    get=ctypes.WinDLL("psapi").GetProcessMemoryInfo
    get.argtypes=[wintypes.HANDLE,ctypes.POINTER(Counters),wintypes.DWORD]
    assert get(int(process._handle),ctypes.byref(counters),counters.cb)
    return {"working":round(counters.working/1048576,2),"peak_working":round(counters.peak_working/1048576,2),"private":round(counters.private/1048576,2)}


def eventually(predicate, timeout=5):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(.03)
    raise AssertionError("timed out")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("exe", type=Path)
    args = parser.parse_args()
    exe = args.exe.resolve(strict=True)
    run = str(uuid.uuid4())
    root_key = rf"Software\EnvHostsQmlTests\{run}"
    user_key, system_key = root_key + r"\User", root_key + r"\System"
    for key in (user_key, system_key):
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, key) as handle:
            winreg.SetValueEx(handle, "PATH_TEST", 0, winreg.REG_EXPAND_SZ, r"%TEMP%;C:\Fixture")
            winreg.SetValueEx(handle, "MULTI_TEST", 0, winreg.REG_MULTI_SZ, ["A;B", "中文"])
    checks = []
    process = None
    try:
        with tempfile.TemporaryDirectory(prefix="envhosts-qml-") as tmp:
            temp = Path(tmp)
            hosts = temp / "hosts"
            hosts.write_bytes(b"# fixture\r\n127.0.0.1 alpha.test alias.test\r\n::1 beta.test\n")
            before_hosts = hosts.read_bytes()
            before_user, before_system = registry(user_key), registry(system_key)
            env = dict(os.environ, WS_ENVHOSTS_TEST_SUBKEY=user_key,
                       WS_ENVHOSTS_TEST_SYSTEM_SUBKEY="HKCU\\" + system_key,
                       WS_ENVHOSTS_TEST_HOSTS=str(hosts), QT_QUICK_BACKEND="software")
            assert env["WS_ENVHOSTS_TEST_SYSTEM_SUBKEY"].startswith("HKCU\\Software\\EnvHostsQmlTests\\")
            process = subprocess.Popen([str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True, encoding="utf-8", env=env,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            replies = queue.Queue()
            errors = []
            def pump():
                for line in process.stdout:
                    try:
                        replies.put(json.loads(line))
                    except ValueError:
                        replies.put({"invalid_stdout": line})
            threading.Thread(target=pump, daemon=True).start()
            threading.Thread(target=lambda: errors.extend(process.stderr.readlines()), daemon=True).start()
            sequence = 0
            def call(method, params):
                nonlocal sequence
                sequence += 1
                process.stdin.write(json.dumps({"jsonrpc":"2.0", "id":sequence, "method":method, "params":params}, ensure_ascii=False)+"\n")
                process.stdin.flush()
                while True:
                    reply = replies.get(timeout=8)
                    assert "invalid_stdout" not in reply, reply
                    if reply.get("id") == sequence:
                        assert "error" not in reply, reply
                        return reply["result"]
            def query(kind, text=""):
                return call("query", {"queryId":sequence+1,"command":kind,"trigger":"keyword","keyword":kind,"text":text,"raw":kind+" "+text,"limit":200})["items"]
            def action(kind, act, entry):
                return call("action", {"command":kind,"action":act,"item":entry})
            call("initialize", {"pluginDir":str(exe.parent),"dataDir":str(temp/"data"),"locale":"zh","theme":"light","settings":{"showSystemVars":True}})
            for kind in ("env", "hosts"):
                entry = next(i for i in query(kind) if i.get("data",{}).get("kind")=="openEditor")
                assert not windows(process.pid) if kind=="env" else True
                for _ in range(3):
                    result = action(kind, "openEditor", entry)
                    assert "Cannot open" not in json.dumps(result) and "无法打开" not in json.dumps(result,ensure_ascii=False), result
                eventually(lambda: len(windows(process.pid))==(1 if kind=="env" else 2))
                checks.append(kind+" window reused (three opens)")
            assert registry(user_key)==before_user and registry(system_key)==before_system and hosts.read_bytes()==before_hosts
            memory = memory_mib(process)
            checks.append("query/open have zero target writes")
            edit = {"data":{"kind":"envEdit","name":"PATH_TEST","value":"new-value"}}
            action("env","save",edit)
            assert registry(user_key)["PATH_TEST"] == ("new-value",winreg.REG_EXPAND_SZ)
            assert registry(user_key)["MULTI_TEST"] == (["A;B","中文"],winreg.REG_MULTI_SZ)
            assert registry(system_key)==before_system
            backups=list((temp/"data/backups/env").glob("*.json"))
            assert len(backups)==1 and json.loads(backups[0].read_text("utf-8"))["schemaVersion"]==2
            checks.append("launcher writes through verified service; types and unrelated scopes preserved")
            host_entry=next(i for i in query("hosts") if i.get("data",{}).get("kind")=="hosts" and "alpha.test" in json.dumps(i))
            action("hosts","toggle",host_entry)
            assert b"# 127.0.0.1" in hosts.read_bytes()
            assert next((temp/"data/backups/hosts").glob("*.hosts")).read_bytes()==before_hosts
            checks.append("hosts toggle backed up exact source bytes")
            call("shutdown", {})
            process.wait(timeout=5)
            assert process.returncode==0
            assert not any("qrc:" in e and ("Error" in e or "ReferenceError" in e or "TypeError" in e) for e in errors), errors
            checks.append("packaged resources load; shutdown exits cleanly")
            print(json.dumps({"status":"PASS","checks":checks,"exe":str(exe),"isolated":True,"two_window_memory_mib":memory},ensure_ascii=False,indent=2))
    finally:
        if process and process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        for key in (user_key,system_key,root_key):
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER,key)


if __name__=="__main__":
    main()
