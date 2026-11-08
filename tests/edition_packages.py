"""Inspect release archives and exercise GDI -> QML -> GDI in a temporary install.

This checks package replacement/data isolation, not WinShun's installation UI.
All queries use scratch registry keys and a temporary hosts file. No live host.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import tempfile
import threading
import uuid
import winreg
import zipfile


def inspect(path, backend):
    data = path.read_bytes()
    assert hashlib.sha256(data).hexdigest() == Path(str(path)+'.sha256').read_text().split()[0]
    with zipfile.ZipFile(path) as archive:
        assert archive.testzip() is None
        manifest = json.loads(archive.read('plugin.json'))
        edition = json.loads(archive.read('edition.json'))
        assert manifest['id'] == 'ra9fael.envhosts' and edition['backend'] == backend
        assert ('Qt6Core.dll' in archive.namelist()) == (backend == 'qml')
        for name in archive.namelist():
            assert not Path(name).is_absolute() and '..' not in Path(name).parts and ':' not in name
        assert sum(entry.file_size for entry in archive.infolist()) <= 256 << 20
        assert len(data) <= 64 << 20
        return manifest


def smoke(exe, data, env, backend, manifest):
    actual = json.loads(subprocess.check_output([str(exe),'--build-info'], cwd=exe.parent, env=env, text=True, timeout=10))
    assert actual == {'id':manifest['id'],'backend':backend,'version':manifest['version']}
    p = subprocess.Popen([str(exe)], cwd=exe.parent, env=env, stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8',
                         creationflags=subprocess.CREATE_NO_WINDOW)
    replies = queue.Queue()
    def pump():
        for line in p.stdout:
            replies.put(json.loads(line))
    threading.Thread(target=pump,daemon=True).start()
    threading.Thread(target=lambda:list(p.stderr),daemon=True).start()
    sequence = 0
    def call(method, params):
        nonlocal sequence
        sequence += 1
        p.stdin.write(json.dumps({'jsonrpc':'2.0','id':sequence,'method':method,'params':params})+'\n')
        p.stdin.flush()
        while True:
            r = replies.get(timeout=8)
            if r.get('id') == sequence:
                assert 'error' not in r, r
                return r.get('result')
    try:
        call('initialize',{'pluginDir':str(exe.parent),'dataDir':str(data),'locale':'en-US','theme':'light','settings':{'showSystemVars':True}})
        for kind,needle in [('env','EDITION_TEST'),('hosts','edition.test')]:
            result=call('query',{'queryId':sequence+1,'command':kind,'trigger':'keyword','keyword':kind,'text':'','raw':kind,'limit':200})
            assert needle in json.dumps(result), result
        call('shutdown',{})
        assert p.wait(timeout=8) == 0
    finally:
        if p.poll() is None:
            p.kill();p.wait(timeout=5)


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('gdi',type=Path);parser.add_argument('qml',type=Path)
    args=parser.parse_args()
    packages={'gdi':args.gdi.resolve(),'qml':args.qml.resolve()}
    manifests={backend:inspect(path,backend) for backend,path in packages.items()}
    key=rf'Software\EnvHostsEditionTests\{uuid.uuid4()}'
    keys=[key+r'\User',key+r'\System']
    for sub in keys:
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER,sub) as h:
            winreg.SetValueEx(h,'EDITION_TEST',0,winreg.REG_SZ,'unchanged')
    try:
        with tempfile.TemporaryDirectory(prefix='envhosts-package-switch-') as folder:
            root=Path(folder);installed=root/'plugins/ra9fael.envhosts';data=root/'ra9fael.envhosts.data'
            for name,content in [('drafts/gdi/env.json',b'{"rows":[]}'),('drafts/qml/env.json',b'{"schemaVersion":2}'),('backups/hosts/sentinel.hosts',b'# keep'),('settings.json',b'{"language":"en-US"}')]:
                path=data/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(content)
            original={str(p.relative_to(data)):p.read_bytes() for p in data.rglob('*') if p.is_file()}
            hosts=root/'hosts';hosts.write_bytes(b'127.0.0.1 edition.test\n')
            env=os.environ.copy();env.update(WS_DATA_DIR=str(data),WS_ENVHOSTS_TEST_SUBKEY=keys[0],
                WS_ENVHOSTS_TEST_SYSTEM_SUBKEY='HKCU\\'+keys[1],WS_ENVHOSTS_TEST_HOSTS=str(hosts))
            # Verify that bundled DLLs suffice without a development Qt on PATH.
            env['PATH']=os.environ['SystemRoot']+r'\System32;'+os.environ['SystemRoot']
            for i,backend in enumerate(('gdi','qml','gdi')):
                candidate=root/f'candidate-{i}';candidate.mkdir()
                with zipfile.ZipFile(packages[backend]) as archive:archive.extractall(candidate)
                previous=root/f'previous-{i}';installed.parent.mkdir(exist_ok=True)
                if installed.exists():installed.rename(previous)
                candidate.rename(installed)
                smoke(installed/'envhosts.exe',data,env,backend,manifests[backend])
                assert len(list(installed.parent.iterdir())) == 1
                assert (installed/'Qt6Core.dll').exists() == (backend=='qml')
                assert original == {str(p.relative_to(data)):p.read_bytes() for p in data.rglob('*') if p.is_file()}
                if previous.exists():shutil.rmtree(previous)
            assert hosts.read_bytes()==b'127.0.0.1 edition.test\n'
            for sub in keys:
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER,sub) as h:
                    assert winreg.QueryValueEx(h,'EDITION_TEST')[0]=='unchanged'
        print('PASS: archive hashes, shared ID, binary identity, standalone runtime, GDI/QML/GDI replacement, isolated query/shutdown, preserved data and scratch targets')
    finally:
        for sub in (*keys,key):winreg.DeleteKey(winreg.HKEY_CURRENT_USER,sub)


if __name__=='__main__':main()
