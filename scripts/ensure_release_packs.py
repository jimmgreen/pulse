"""Validate the compiled catalog's complete pack payload before publishing a draft."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile


def catalog(path, repository, tag):
    text = path.read_text(encoding='utf-8-sig')
    arrays = dict(re.findall(r'inline constexpr PackFile (\w+)\[\] = \{(.*?)\n\};', text, re.S))
    releases = re.findall(r'inline constexpr PackRelease \w+\{\s*L"([^"]+)",\s*L"([^"]+)",\s*L"([^"]+)",\s*(\w+),\s*(?:std::size\(\4\)|sizeof\(\4\)\s*/\s*sizeof\(\4\[0\]\))\s*\};', text)
    if len(releases) != 3 or {r[0] for r in releases} != {'ffmpeg', 'images', 'raw'}:
        raise ValueError('Expected exactly three catalog releases')
    packs = []
    names = set()
    for key, version, url, array in releases:
        independent_tag = f'pack-{key}-{version}'
        if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*', version):
            raise ValueError(f'Invalid catalog version: {key}')
        prefix = f'https://github.com/{repository}/releases/download/'
        if url not in {f'{prefix}{tag}/', f'{prefix}{independent_tag}/'}:
            raise ValueError(f'Catalog release URL does not match target: {key}')
        release_tag = independent_tag if url == f'{prefix}{independent_tag}/' else tag
        body = arrays[array]
        rows = re.findall(r'\{L"([^"]+)", (\d+), L"([a-f0-9]{64})", (\d+), L"([a-f0-9]{64})"(?:, L"([^"]+)")?\},', body)
        if not rows or len(rows) != body.count('{'):
            raise ValueError(f'Unparsed catalog file rows: {key}')
        files = []
        for name, size, sha, packed_size, packed_sha, download in rows:
            download = download or name + '.lzms'
            if not re.fullmatch(r'[A-Za-z0-9_.-]+', download) or download in names:
                raise ValueError('Invalid/duplicate catalog asset')
            names.add(download)
            files.append(dict(name=name, size=int(size), sha256=sha, packed_size=int(packed_size),
                              packed_sha256=packed_sha, download_name=download))
        packs.append(dict(id=key, version=version, release_tag=release_tag, files=files))
    return packs


def validate(path, entry):
    if path.stat().st_size != entry['packed_size']:
        raise ValueError(f'Packed size mismatch: {path.name}')
    with path.open('rb') as source:
        digest = hashlib.file_digest(source, 'sha256').hexdigest()
    if digest != entry['packed_sha256']:
        raise ValueError(f'Packed hash mismatch: {path.name}')


def ensure(packs, repository, tag, source_tag=None, asset_directory=None, gh=None):
    gh = gh or (lambda *args: subprocess.run(['gh', *args], check=True, capture_output=True, text=True).stdout)
    state = json.loads(gh('release', 'view', tag, '--repo', repository, '--json', 'isDraft'))
    if not state['isDraft']:
        raise ValueError('Pack staging requires an unpublished target release')
    for pack in packs:
        if pack['release_tag'] != tag:
            if pack['release_tag'] != f"pack-{pack['id']}-{pack['version']}":
                raise ValueError('Independent release must match the catalog pack and version')
            state = json.loads(gh('release', 'view', pack['release_tag'], '--repo', repository, '--json', 'isDraft'))
            if state['isDraft']:
                raise ValueError('Independent pack release must be published')
    if source_tag and source_tag != tag:
        state = json.loads(gh('release', 'view', source_tag, '--repo', repository, '--json', 'isDraft'))
        if state['isDraft']:
            raise ValueError('Source pack release must be published')
    with tempfile.TemporaryDirectory(prefix='pulse-release-packs-') as temporary:
        root = Path(temporary)
        uploads = []
        for pack in packs:
            independent = pack['release_tag'] != tag
            for entry in pack['files']:
                name = entry['download_name']
                local = Path(asset_directory) / name if asset_directory else None
                if not independent and local and local.is_file():
                    validate(local, entry)
                    uploads.append(str(local.resolve()))
                else:
                    gh('release', 'download', pack['release_tag'] if independent else source_tag or tag, '--repo', repository,
                       '--pattern', name, '--dir', str(root))
                    validate(root / name, entry)
                    uploads.append(str(root / name))
            manifest = root / f"pulse-pack-{pack['id']}-{pack['version']}.json"
            manifest.write_text(json.dumps(pack, indent=2) + '\n', encoding='utf-8')
            uploads.append(str(manifest))
        # No upload happens until all payload files match the compiled catalog.
        gh('release', 'upload', tag, '--repo', repository, *uploads, '--clobber')
        verified = root / 'remote'
        verified.mkdir()
        for pack in packs:
            for entry in pack['files']:
                gh('release', 'download', tag, '--repo', repository, '--pattern', entry['download_name'], '--dir', str(verified))
                validate(verified / entry['download_name'], entry)
            name = f"pulse-pack-{pack['id']}-{pack['version']}.json"
            gh('release', 'download', tag, '--repo', repository, '--pattern', name, '--dir', str(verified))
            if json.loads((verified / name).read_text(encoding='utf-8')) != pack:
                raise ValueError(f'Remote manifest differs from catalog: {name}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', default='jimmgreen/pulse')
    parser.add_argument('--tag', required=True)
    parser.add_argument('--source-tag')
    parser.add_argument('--asset-directory', type=Path)
    parser.add_argument('--catalog', type=Path, default=Path(__file__).resolve().parents[1] / 'src/app/pack_catalog_generated.h')
    args = parser.parse_args()
    packs = catalog(args.catalog, args.repository, args.tag)
    ensure(packs, args.repository, args.tag, args.source_tag, args.asset_directory)
    print(f"Verified {sum(len(p['files']) for p in packs)} remote pack files and {len(packs)} manifests for {args.tag}")


if __name__ == '__main__':
    main()
