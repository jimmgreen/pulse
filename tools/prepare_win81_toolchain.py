"""Extract the minimal Microsoft v143 toolset locally; no system installation.

Requires the official VS 17 channel/manifest JSON under win81-toolchain.
Payload hashes are verified against Microsoft's manifest before extraction.
"""
from pathlib import Path
import concurrent.futures
import hashlib
import json
import urllib.parse
import urllib.request
import zipfile

base = Path(__file__).resolve().parent / 'win81-toolchain'
manifest = json.loads((base / 'manifest.json').read_text(encoding='utf-8'))
prefix = 'Microsoft.VC.14.44.17.14.'
wanted = {prefix + suffix for suffix in [
    'Tools.HostX64.TargetX64.base', 'Tools.HostX64.TargetX64.Res.base',
    'CRT.Headers.base', 'CRT.x64.Desktop.base', 'CRT.x64.Store.base',
]}
payloads = [p for entry in manifest['packages'] if entry['id'] in wanted
            and entry.get('language', 'en-US') == 'en-US' for p in entry['payloads']]

def fetch(payload):
    url = payload['url']
    assert urllib.parse.urlsplit(url).hostname == 'download.visualstudio.microsoft.com'
    dest = base / payload['fileName']
    if not dest.exists():
        urllib.request.urlretrieve(url, dest)
    assert hashlib.sha256(dest.read_bytes()).hexdigest() == payload['sha256'], dest.name
    print('Verified ' + dest.name, flush=True)
    return dest

with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    archives = list(pool.map(fetch, payloads))
for archive in archives:
    with zipfile.ZipFile(archive) as z:
        for item in z.infolist():
            name = urllib.parse.unquote(item.filename).replace('\\', '/')
            if not name.startswith('Contents/') or item.is_dir():
                continue
            dest = (base / name[len('Contents/'):]).resolve()
            assert dest.is_relative_to(base.resolve())
            dest.parent.mkdir(parents=True, exist_ok=True)
            data = z.read(item)
            if not dest.exists() or dest.read_bytes() != data:
                dest.write_bytes(data)
print('Local toolset ready', flush=True)
