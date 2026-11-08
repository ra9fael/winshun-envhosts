"""Build the standalone review page from the inline mockup; no product changes."""
from pathlib import Path
import base64
import hashlib
import re
import sys

root = Path(__file__).resolve().parents[1]
folder = root / "docs/qml/repair-prototype"
fragment = (folder / "fragment.html").read_text(encoding="utf-8")
scripts = re.findall(r"<script>([\s\S]*?)</script>", fragment)
hashes = " ".join("'sha256-" + base64.b64encode(hashlib.sha256(s.encode()).digest()).decode() + "'" for s in scripts)
page = f'''<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src {hashes}; style-src 'unsafe-inline'; connect-src 'none'; base-uri 'none'; form-action 'none'">
<title>EnvHosts · 紧凑列表修复演示</title>
<style>body {{ margin:16px auto; padding:0 12px; max-width:1120px; color-scheme:light dark; background:light-dark(#f7f9fc,#101720); }}</style>
</head><body>
{fragment}
</body></html>
'''
destination = folder / "index.html"
if "--check" in sys.argv:
    if not destination.exists() or destination.read_text(encoding="utf-8") != page:
        raise SystemExit("Repair demo is stale; run tools/build_qml_repair_demo.py")
    print("Repair demo source / standalone page: PASS")
else:
    destination.write_text(page, encoding="utf-8", newline="\n")
    print(destination)
