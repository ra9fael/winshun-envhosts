#!/usr/bin/env python3
# integration.py - drives the built envhosts.exe over its JSON-RPC line protocol
# and verifies real Windows behaviour (registry writes to a scratch subkey,
# hosts read-only degradation, side-effect audit, settings changes).
#
# It uses the test-only escapes in main.cpp:
#   WS_ENVHOSTS_TEST_SUBKEY  -> HKCU subkey the plugin writes instead of Environment
#   WS_ENVHOSTS_TEST_HOSTS   -> hosts path the plugin reads/writes instead of the real one
# So the real user Environment and the real hosts file are never touched.

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import winreg

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, "..", "envhosts.exe")
FIXTURE = os.path.join(HERE, "fixtures", "hosts.sample")
SUBKEY = r"Software\WinShunTest\EnvHosts"

passed = 0
failed = 0


def check(cond, label, extra=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  PASS  {label}")
    else:
        failed += 1
        print(f"  FAIL  {label}  {extra}")


def reg_list(subkey):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, subkey) as h:
            out = {}
            i = 0
            while True:
                try:
                    name, value, _type = winreg.EnumValue(h, i)
                    out[name] = (value, _type)
                    i += 1
                except OSError:
                    break
            return out
    except FileNotFoundError:
        return {}


def reg_set(name, value):
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, SUBKEY) as h:
        winreg.SetValueEx(h, name, 0, winreg.REG_SZ, value)


def reg_delete(name):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, SUBKEY, 0, winreg.KEY_SET_VALUE) as h:
            winreg.DeleteValue(h, name)
    except FileNotFoundError:
        pass


def reg_delete_key():
    try:
        winreg.DeleteKey(winreg.HKEY_CURRENT_USER, SUBKEY)
    except FileNotFoundError:
        pass


def reg_make_key():
    # RegOpenKeyEx(KEY_SET_VALUE) does not create a missing key, so the scratch
    # subkey must exist before the plugin writes (HKCU\Environment always exists
    # in production, so this is test-only setup).
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, SUBKEY) as h:
        pass


class Driver:
    def __init__(self, data_dir, hosts_path, hosts_override=True):
        env = dict(os.environ)
        env["WS_ENVHOSTS_TEST_SUBKEY"] = SUBKEY
        if hosts_override:
            env["WS_ENVHOSTS_TEST_HOSTS"] = hosts_path
        else:
            # No override: the plugin targets the real
            # %SystemRoot%\System32\drivers\etc\hosts (read-only for a
            # normal user), which is what the denial tests need.
            env.pop("WS_ENVHOSTS_TEST_HOSTS", None)
        self.proc = subprocess.Popen(
            [EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, env=env, text=True, bufsize=1,
            encoding="utf-8", errors="replace")
        self._id = 0
        self._reader = threading.Thread(target=self._pump, daemon=True)
        self._buf = []
        self._lock = threading.Lock()
        self._reader.start()

    def _pump(self):
        for line in self.proc.stdout:
            with self._lock:
                self._buf.append(line)

    def _next_id(self):
        self._id += 1
        return self._id

    def _send(self, msg):
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()

    def recv(self, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                if self._buf:
                    return self._buf.pop(0)
            time.sleep(0.01)
        return None

    def initialize(self, data_dir, settings):
        mid = self._next_id()
        self._send({"id": mid, "method": "initialize", "params": {
            "settings": settings, "locale": "zh", "theme": "light",
            "pluginDir": HERE, "dataDir": data_dir}})
        line = self.recv()
        return line

    def query(self, command, text, raw=None, query_id=None):
        mid = self._next_id()
        qid = query_id if query_id is not None else mid
        raw = raw if raw is not None else f"{command} {text}".strip()
        self._send({"id": mid, "method": "query", "params": {
            "queryId": qid, "command": command, "trigger": "keyword",
            "keyword": command, "text": text, "raw": raw, "limit": 200}})
        line = self.recv()
        return line

    def action(self, command, action_id, item, text=""):
        mid = self._next_id()
        self._send({"id": mid, "method": "action", "params": {
            "command": command, "action": action_id, "text": text, "item": item}})
        line = self.recv()
        return line

    def notify_settings(self, settings):
        # settingsChanged has no reply; just send.
        self._send({"id": self._next_id(), "method": "settingsChanged",
                    "params": {"settings": settings}})

    def cancel(self, query_id):
        self._send({"id": self._next_id(), "method": "cancel",
                    "params": {"queryId": query_id}})

    def shutdown(self):
        mid = self._next_id()
        self._send({"id": mid, "method": "shutdown", "params": {}})
        self.recv(timeout=2.0)
        # A resident plugin ignores idle exit by design, so make sure the process
        # really goes: a leaked instance holds envhosts.exe open and breaks the
        # next link step.
        try:
            self.proc.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()

    def find_item(self, items, kind=None, name=None, contains=None):
        for it in items:
            if kind and it.get("data", {}).get("kind") != kind:
                continue
            if name and it.get("data", {}).get("name") != name:
                continue
            if contains and contains not in json.dumps(it):
                continue
            return it
        return None


def parse(line):
    if not line:
        return None
    return json.loads(line)


def main():
    if not os.path.exists(EXE):
        print("envhosts.exe not built; run: g++ -std=c++17 -I. main.cpp -o envhosts.exe -ladvapi32")
        return 2
    tmp = tempfile.mkdtemp(prefix="envhosts_test_")
    data_dir = os.path.join(tmp, "data")
    os.makedirs(data_dir, exist_ok=True)
    hosts_path = os.path.join(tmp, "hosts.test")
    shutil.copy(FIXTURE, hosts_path)
    hosts_hash = hashlib.sha256(open(hosts_path, "rb").read()).hexdigest()

    reg_delete_key()
    reg_make_key()
    settings = {"hostsPath": "%SystemRoot%\\System32\\drivers\\etc\\hosts",
                "backupCount": 5, "maxResults": 200, "showSystemVars": True}

    print("== T2.1 protocol: initialize / query / action / settings / cancel ==")
    d = Driver(data_dir, hosts_path)
    init = parse(d.initialize(data_dir, settings))
    check(init and init.get("result", {}).get("name") == "ra9fael.envhosts", "initialize returns plugin name")
    q = parse(d.query("env", "", "env"))
    check(q and "items" in q.get("result", {}), "env query returns items")
    check(q and q["result"].get("cache") == 0, "queryResult carries cache:0")

    print("== T2.6 side-effect audit: 20 query-only inputs must not change hosts or registry ==")
    before_reg = reg_list(SUBKEY)
    before_hash = hashlib.sha256(open(hosts_path, "rb").read()).hexdigest()
    for i in range(20):
        r = parse(d.query("env", f"filter{i}", f"env filter{i}"))
        r2 = parse(d.query("hosts", f"q{i}", f"hosts q{i}"))
        assert r and r2
    after_reg = reg_list(SUBKEY)
    after_hash = hashlib.sha256(open(hosts_path, "rb").read()).hexdigest()
    check(before_hash == after_hash, "hosts bytes unchanged after 20 queries", f"{before_hash[:8]} vs {after_hash[:8]}")
    check(before_reg == after_reg, "test registry subkey unchanged after 20 queries")

    print("== T2.8 edit round trip on the scratch store ==")
    # env:set WS_TEST_VAR=hello -> edit row -> save -> verify in registry
    e1 = parse(d.query("env", ":set WS_TEST_VAR=hello", "env:set WS_TEST_VAR=hello"))
    edit = d.find_item(e1["result"]["items"], kind="envEdit")
    check(edit is not None, "env:set produces an edit row")
    save = parse(d.action("env", "save", edit))
    effects = save["result"]["effects"]
    check(any(e.get("type") == "notify" for e in effects), "save returns a notify effect")
    time.sleep(0.2)
    check(reg_list(SUBKEY).get("WS_TEST_VAR", (None, None))[0] == "hello", "WS_TEST_VAR written to registry",
          str(reg_list(SUBKEY)))
    # change value
    e2 = parse(d.query("env", ":set WS_TEST_VAR=world", "env:set WS_TEST_VAR=world"))
    edit2 = d.find_item(e2["result"]["items"], kind="envEdit")
    d.action("env", "save", edit2)
    time.sleep(0.2)
    check(reg_list(SUBKEY).get("WS_TEST_VAR", (None, None))[0] == "world", "WS_TEST_VAR updated")
    # delete via confirm
    lst = parse(d.query("env", "WS_TEST_VAR", "env WS_TEST_VAR"))
    row = d.find_item(lst["result"]["items"], kind="env", name="WS_TEST_VAR")
    if row is None:
        print("    debug list items:", json.dumps(lst["result"]["items"])[:600])
    check(row is not None, "WS_TEST_VAR listed")
    if row is not None:
        delc = parse(d.action("env", "delete", row))
        confirm = d.find_item(delc["result"]["effects"][0]["items"], kind="envDeleteConfirm")
        check(confirm is not None, "delete returns a confirm row via replace")
        d.action("env", "confirm", confirm)
        time.sleep(0.2)
        check("WS_TEST_VAR" not in reg_list(SUBKEY), "WS_TEST_VAR deleted")

    print("== restore from backup (undo) ==")
    rest = parse(d.query("env", "restore", "env restore"))
    bk = d.find_item(rest["result"]["items"], kind="envRestore")
    if bk:
        d.action("env", "restore", bk)
        time.sleep(0.2)
        check(reg_list(SUBKEY).get("WS_TEST_VAR", (None, None))[0] == "world",
              "restore brought WS_TEST_VAR back", str(reg_list(SUBKEY)))
    else:
        check(False, "env:restore listed a backup")

    print("== P1/P2 interactive editor pinned row is always first ==")
    pe = parse(d.query("env", "", "env"))
    pe_items = pe["result"]["items"]
    check(pe_items and pe_items[0]["id"] == "env:editor", "env query first row is env:editor")
    check(pe_items and pe_items[0]["data"]["kind"] == "openEditor", "env:editor row kind is openEditor")
    ph = parse(d.query("hosts", "", "hosts"))
    ph_items = ph["result"]["items"]
    check(ph_items and ph_items[0]["id"] == "hosts:editor", "hosts query first row is hosts:editor")
    check(ph_items and ph_items[0]["data"]["kind"] == "openEditor", "hosts:editor row kind is openEditor")

    print("== P3 openEditor action is wired and degrades gracefully ==")
    oe = parse(d.action("env", "open", pe_items[0]))
    oe_eff = oe["result"]["effects"]
    has_notify = any(e.get("type") == "notify" for e in oe_eff)
    check(has_notify, "openEditor returns a notify effect")
    # On a headless/CI box CreateWindowExW fails; the action must NOT crash and the
    # process must stay responsive (a follow-up query still answers).
    pe2 = parse(d.query("env", "", "env"))
    check(pe2 and pe2["result"]["items"][0]["id"] == "env:editor", "plugin still answers after openEditor")
    p3_hide = any(e.get("type") == "hide" for e in oe_eff)
    p3_keep = any(e.get("type") == "keep" for e in oe_eff)
    check(p3_hide or p3_keep, "openEditor returns hide (opened) or keep (failed) effect",
          str([e.get("type") for e in oe_eff]))
    # P6: external-change path must not crash when a launcher write happens while an
    # editor may or may not be open (onExternalChange posts to a possibly-null HWND).
    e_save = parse(d.query("env", ":set WS_EDITOR_PING=ok", "env:set WS_EDITOR_PING=ok"))
    edit_row = d.find_item(e_save["result"]["items"], kind="envEdit")
    if edit_row:
        d.action("env", "save", edit_row)
        time.sleep(0.2)
        ping = reg_list(SUBKEY).get("WS_EDITOR_PING", (None, None))[0]
        check(ping == "ok", "env save still works after openEditor (external-change path)",
              f"WS_EDITOR_PING={ping}")
        d.action("env", "delete", d.find_item(parse(d.query("env", "WS_EDITOR_PING", "env WS_EDITOR_PING"))["result"]["items"],
                                              kind="env", name="WS_EDITOR_PING"))
        time.sleep(0.2)

    print("== P4 manifest declares the current version + ui + clipboard:write + idleExitSec 0 ==")
    import json as _json
    plugin_path = os.path.join(HERE, "..", "plugin.json")
    with open(plugin_path, "r", encoding="utf-8") as _f:
        manifest = _json.load(_f)
    check(re.match(r"^\d+\.\d+\.\d+$", manifest.get("version", "")),
          "plugin.json carries a semantic version", manifest.get("version"))
    perms = manifest.get("permissions", [])
    check("ui" in perms, "permissions include ui")
    check("clipboard:write" in perms, "permissions include clipboard:write")
    check(manifest.get("runtime", {}).get("idleExitSec") == 0, "idleExitSec is 0 (window must stay alive)")

    print("== P5 editor draft path uses per-kind files under WS_DATA_DIR ==")
    editor_src = open(os.path.join(HERE, "..", "editor_win.h"), "r", encoding="utf-8").read()
    # Must check the actual path expression, not a comment: drafts are written
    # to <dataDir>/drafts/<kind>.json (env.json / hosts.json).
    check('m_args.dataDir + "/drafts/" + m_kind + ".json"' in editor_src,
          "editor_win.h writes drafts to <dataDir>/drafts/<kind>.json")
    check("fromDraft" in editor_src and "readDraft" in editor_src,
          "editor_win.h auto-loads a leftover draft on open")

    print("== T2.3 hosts write denied as normal user (real System32 hosts) ==")
    # Second instance without the temp-hosts override: the plugin reads the
    # real hosts file, which a normal user cannot write.
    d2 = Driver(data_dir, hosts_path, hosts_override=False)
    d2.initialize(data_dir, settings)
    hq = parse(d2.query("hosts", "", "hosts"))
    hitems = hq["result"]["items"]
    live = next((it for it in hitems if it.get("data", {}).get("kind") == "hosts"
                and not it.get("data", {}).get("commented")
                and it.get("data", {}).get("section") is not True), None)
    check(live is not None, "a live hosts entry is listed from the real file")
    real_hosts = os.path.expandvars(r"%SystemRoot%\System32\drivers\etc\hosts")
    hash_before = hashlib.sha256(open(real_hosts, "rb").read()).hexdigest()
    if live:
        tg = parse(d2.action("hosts", "toggle", live))
        effects = tg["result"]["effects"]
        rep = next((e for e in effects if e.get("type") == "replace"), None)
        denied = False
        if rep:
            for it in rep["items"]:
                if it.get("data", {}).get("code") == "E_DENIED" or "只读" in it.get("title", ""):
                    denied = True
        check(denied, "hosts toggle as normal user returns an E_DENIED error row",
              json.dumps(effects, ensure_ascii=False)[:200])
        hash_after = hashlib.sha256(open(real_hosts, "rb").read()).hexdigest()
        check(hash_before == hash_after, "real hosts file bytes unchanged after denied toggle")
    d2.shutdown()

    print("== T2.7 settingsChanged toggles showSystemVars ==")
    sq1 = parse(d.query("env", "", "env"))
    has_sys1 = any(it.get("data", {}).get("scope") == "system" for it in sq1["result"]["items"])
    d.notify_settings({"hostsPath": "%SystemRoot%\\System32\\drivers\\etc\\hosts",
                      "backupCount": 5, "maxResults": 200, "showSystemVars": False})
    time.sleep(0.1)
    sq2 = parse(d.query("env", "", "env"))
    has_sys2 = any(it.get("data", {}).get("scope") == "system" for it in sq2["result"]["items"])
    check(has_sys1 and not has_sys2, "settingsChanged showSystemVars=false hides system vars",
          f"{has_sys1} -> {has_sys2}")
    d.notify_settings({"hostsPath": "%SystemRoot%\\System32\\drivers\\etc\\hosts",
                      "backupCount": 5, "maxResults": 200, "showSystemVars": True})

    print("== T2.2 system-scope delete is refused (write to HKLM denied) ==")
    sys_q = parse(d.query("env", "", "env"))
    sys_row = next((it for it in sys_q["result"]["items"] if it.get("data", {}).get("kind") == "env"
                    and it.get("data", {}).get("scope") == "system"), None)
    if sys_row:
        delc = parse(d.action("env", "delete", sys_row))
        effects = delc["result"]["effects"]
        rep = next((e for e in effects if e.get("type") == "replace"), None)
        if rep is None:
            # Correct behaviour: refuse immediately, no confirm row offered.
            note = next((e for e in effects if e.get("type") == "notify"), {})
            check("管理员" in note.get("text", "") or "denied" in note.get("text", "").lower()
                  or "失败" in note.get("text", ""),
                  "system var delete is refused (needs elevated)", note.get("text", ""))
        else:
            conf = d.find_item(rep["items"], kind="envDeleteConfirm")
            res = parse(d.action("env", "confirm", conf))
            note = next((e for e in res["result"]["effects"] if e.get("type") == "notify"), {})
            check("失败" in note.get("text", "") or "denied" in note.get("text", "").lower()
                  or "管理员" in note.get("text", "") or "admin" in note.get("text", "").lower(),
                  "system var delete is refused (needs elevated)", note.get("text", ""))
    else:
        check(True, "no system var row to test (acceptable)")

    d.shutdown()
    time.sleep(0.3)
    reg_delete_key()
    shutil.rmtree(tmp, ignore_errors=True)
    print(f"\n== RESULT: {passed} passed, {failed} failed ==")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
