"""Validate preview-pack payloads before publishing a release.

Upstream Pulse releases stage the four preview packs in their own release.
Fork releases keep the pinned upstream catalog and verify its published assets
without copying or republishing those binaries.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile


_CATALOG_URL_RE = re.compile(
    r'https://github\.com/([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)/releases/download/([A-Za-z0-9_.-]+)/'
)


def _release_rows(text):
    releases = re.findall(
        r'inline constexpr PackRelease \w+\{L"([^"]+)", L"([^"]+)", L"([^"]+)", (\w+), std::size\(\4\)\};',
        text,
    )
    expected = {'ffmpeg', 'images', 'raw', 'archive'}
    if len(releases) != 4 or {row[0] for row in releases} != expected:
        raise ValueError('Expected exactly four catalog releases')
    return releases


def catalog_source(path):
    """Return the single owner/repository and tag pinned by the generated catalog."""
    text = path.read_text(encoding='utf-8-sig')
    releases = _release_rows(text)
    urls = {row[2] for row in releases}
    if len(urls) != 1:
        raise ValueError('Preview-pack catalog releases must share one base URL')
    match = _CATALOG_URL_RE.fullmatch(next(iter(urls)))
    if not match:
        raise ValueError('Preview-pack catalog has an invalid HTTPS release URL')
    return match.group(1), match.group(2)


def catalog(path, repository, tag):
    text = path.read_text(encoding='utf-8-sig')
    arrays = dict(re.findall(r'inline constexpr PackFile (\w+)\[\] = \{(.*?)\n\};', text, re.S))
    releases = _release_rows(text)
    source_repository, source_tag = catalog_source(path)
    if source_repository != repository or source_tag != tag:
        raise ValueError(
            f'Catalog release URL targets {source_repository}@{source_tag}, '
            f'not the expected {repository}@{tag}'
        )
    packs = []
    names = set()
    for key, version, _url, array in releases:
        body = arrays.get(array)
        if body is None:
            raise ValueError(f'Missing catalog file array: {array}')
        rows = re.findall(
            r'\{L"([^"]+)", (\d+), L"([a-f0-9]{64})", (\d+), L"([a-f0-9]{64})", L"([^"]+)"\},',
            body,
        )
        if not rows or len(rows) != body.count('{'):
            raise ValueError(f'Unparsed catalog file rows: {key}')
        files = []
        for name, size, sha, packed_size, packed_sha, download in rows:
            if not re.fullmatch(r'[A-Za-z0-9_.-]+', download) or download in names:
                raise ValueError('Invalid/duplicate catalog asset')
            names.add(download)
            files.append(dict(name=name, size=int(size), sha256=sha, packed_size=int(packed_size),
                              packed_sha256=packed_sha, download_name=download))
        packs.append(dict(id=key, version=version, release_tag=tag, files=files))
    return packs


def validate(path, entry):
    if path.stat().st_size != entry['packed_size']:
        raise ValueError(f'Packed size mismatch: {path.name}')
    with path.open('rb') as source:
        digest = hashlib.file_digest(source, 'sha256').hexdigest()
    if digest != entry['packed_sha256']:
        raise ValueError(f'Packed hash mismatch: {path.name}')


def ensure(packs, repository, tag, source_tag=None, asset_directory=None, gh=None,
           source_repository=None, verify_only=False):
    gh = gh or (lambda *args: subprocess.run(['gh', *args], check=True, capture_output=True, text=True).stdout)
    source_repository = source_repository or repository
    source_tag = source_tag or tag

    target_state = json.loads(gh('release', 'view', tag, '--repo', repository, '--json', 'isDraft'))
    if not target_state['isDraft']:
        raise ValueError('Pack staging requires an unpublished target release')

    if verify_only or source_tag != tag or source_repository != repository:
        source_state = json.loads(
            gh('release', 'view', source_tag, '--repo', source_repository, '--json', 'isDraft')
        )
        if source_state['isDraft']:
            raise ValueError('Source pack release must be published')

    with tempfile.TemporaryDirectory(prefix='pulse-release-packs-') as temporary:
        root = Path(temporary)

        if verify_only:
            for pack in packs:
                for entry in pack['files']:
                    name = entry['download_name']
                    gh('release', 'download', source_tag, '--repo', source_repository,
                       '--pattern', name, '--dir', str(root))
                    validate(root / name, entry)
            return

        uploads = []
        for pack in packs:
            for entry in pack['files']:
                name = entry['download_name']
                local = Path(asset_directory) / name if asset_directory else None
                if local and local.is_file():
                    validate(local, entry)
                    uploads.append(str(local.resolve()))
                else:
                    gh('release', 'download', source_tag, '--repo', source_repository,
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
                gh('release', 'download', tag, '--repo', repository,
                   '--pattern', entry['download_name'], '--dir', str(verified))
                validate(verified / entry['download_name'], entry)
            name = f"pulse-pack-{pack['id']}-{pack['version']}.json"
            gh('release', 'download', tag, '--repo', repository, '--pattern', name, '--dir', str(verified))
            if json.loads((verified / name).read_text(encoding='utf-8')) != pack:
                raise ValueError(f'Remote manifest differs from catalog: {name}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', default='jimmgreen/pulse',
                        help='repository receiving the release assets')
    parser.add_argument('--tag', required=True, help='target release tag')
    parser.add_argument('--source-tag',
                        help='published source tag when mirroring packs into an upstream release')
    parser.add_argument('--asset-directory', type=Path)
    parser.add_argument('--verify-only', action='store_true',
                        help='verify the catalog-pinned external assets without uploading them')
    parser.add_argument('--catalog', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'src/app/pack_catalog_generated.h')
    args = parser.parse_args()

    if args.verify_only:
        if args.source_tag or args.asset_directory:
            parser.error('--verify-only reads the source pinned in the catalog; do not pass source options')
        source_repository, source_tag = catalog_source(args.catalog)
        packs = catalog(args.catalog, source_repository, source_tag)
        ensure(packs, args.repository, args.tag, source_tag, gh=None,
               source_repository=source_repository, verify_only=True)
        print(
            f"Verified {sum(len(p['files']) for p in packs)} external preview-pack assets "
            f"from {source_repository}@{source_tag}; no assets were uploaded"
        )
        return

    packs = catalog(args.catalog, args.repository, args.tag)
    source_tag = args.source_tag or args.tag
    ensure(packs, args.repository, args.tag, source_tag, args.asset_directory)
    print(f"Verified {sum(len(p['files']) for p in packs)} remote pack files and four manifests for {args.tag}")


if __name__ == '__main__':
    main()
