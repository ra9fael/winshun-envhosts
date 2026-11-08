"""Open both real editors and check window icons and graceful shutdown.

All data paths are test-owned; no registry or real hosts entries are changed.
The windows receive directed messages only (no global input).
"""
import ctypes as C
from ctypes import wintypes as W
import json, os, queue, subprocess, sys, tempfile, threading
from pathlib import Path

exe=Path(sys.argv[1]).resolve()
u=C.WinDLL('user32',use_last_error=True)
CALLBACK=C.WINFUNCTYPE(W.BOOL,W.HWND,W.LPARAM)
u.EnumWindows.argtypes=[CALLBACK,W.LPARAM]
u.GetWindowThreadProcessId.argtypes=[W.HWND,C.POINTER(W.DWORD)]
u.GetWindowTextW.argtypes=[W.HWND,W.LPWSTR,C.c_int]
u.SendMessageW.argtypes=[W.HWND,W.UINT,W.WPARAM,W.LPARAM];u.SendMessageW.restype=C.c_ssize_t
u.ShowWindow.argtypes=[W.HWND,C.c_int]

with tempfile.TemporaryDirectory(prefix='icon-lifecycle-',dir=exe.parent/'build') as temporary:
    data=Path(temporary); (data/'hosts').write_text('127.0.0.1 localhost\n')
    env=os.environ.copy()
    env.update(WS_ENVHOSTS_TEST_SUBKEY=r'Software\WinShunTest\IconReadOnly',
        WS_ENVHOSTS_TEST_SYSTEM_SUBKEY=r'HKCU\Software\WinShunTest\IconReadOnlySystem',
        WS_ENVHOSTS_TEST_HOSTS=str(data/'hosts'))
    process=subprocess.Popen([str(exe)],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,creationflags=subprocess.CREATE_NO_WINDOW)
    replies=queue.Queue()
    def reader():
        for line in process.stdout: replies.put(json.loads(line))
    threading.Thread(target=reader,daemon=True).start()
    serial=0
    def call(method,params=None):
        global serial
        serial+=1
        process.stdin.write((json.dumps({'jsonrpc':'2.0','id':serial,'method':method,'params':params or {}})+'\n').encode())
        process.stdin.flush()
        while True:
            reply=replies.get(timeout=10)
            if reply.get('id')==serial:
                assert 'result' in reply,reply
                return reply['result']
    try:
        call('initialize',{'locale':'en','pluginDir':str(exe.parent),'dataDir':str(data),'settings':{}})
        for target in ['env','hosts']:
            call('action',{'command':target,'action':'openEditor','item':{
                'id':target+':editor','data':{'kind':'openEditor','target':target}}})
        found=[]
        @CALLBACK
        def visit(hwnd,_):
            pid=W.DWORD();u.GetWindowThreadProcessId(hwnd,C.byref(pid))
            title=C.create_unicode_buffer(512);u.GetWindowTextW(hwnd,title,len(title))
            if pid.value==process.pid and 'WinShun' in title.value:
                found.append(hwnd);u.ShowWindow(hwnd,0)
            return True
        u.EnumWindows(visit,0)
        assert len(found)==2,found
        for hwnd in found:
            for kind in [0,1]:assert u.SendMessageW(hwnd,0x7F,kind,0),'Missing window icon'
        call('shutdown')
        assert process.wait(timeout=8)==0
        print('Both editors have icons and the process shuts down cleanly')
    finally:
        if process.poll() is None:process.kill();process.wait(timeout=5)
        process.stdin.close();process.stdout.close();process.stderr.close()
