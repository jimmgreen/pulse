"""Metadata-only regressions: no network, PE execution, or real user files."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import import_arm64_pack_catalog as catalog


class NativeCatalog(unittest.TestCase):
    def fixture(self, key='ffmpeg', version='1.0.3'):
        name = catalog.TOOLS[key]
        packed = dict(name=name + '.lzms', size=20, sha256='b' * 64)
        manifest = dict(id=key, version=version, architecture='arm64', files=[
            dict(name=name, size=40, sha256='a' * 64, packed=packed)])
        assets = {packed['name']: dict(name=packed['name'], size=20, digest='sha256:' + 'b' * 64)}
        return manifest, assets

    def test_all_four_native_symbols_and_urls(self):
        for key, symbol in catalog.SYMBOLS.items():
            with self.subTest(key=key):
                manifest, assets = self.fixture(key)
                files = catalog.validate_manifest(key, '1.0.3', manifest, assets)
                text = '\n'.join(catalog.render_pack(key, '1.0.3', 'owner/pulse', files))
                self.assertIn('k' + symbol + 'PackArm64Files', text)
                self.assertIn('k' + symbol + 'PackArm64Release', text)
                self.assertIn('/owner/pulse/releases/download/pack-' + key + '-1.0.3-arm64/', text)

    def test_mismatched_identity_version_or_architecture(self):
        for field, value in [('id', 'images'), ('version', 'wrong'), ('architecture', 'x64')]:
            manifest, assets = self.fixture()
            manifest[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                catalog.validate_manifest('ffmpeg', '1.0.3', manifest, assets)

    def test_payload_metadata_is_required(self):
        for mutation in ['missing', 'size', 'hash', 'unsafe', 'duplicate', 'no-tool', 'boolean-size', 'oversize']:
            manifest, assets = self.fixture()
            item = manifest['files'][0]
            if mutation == 'missing':
                assets.clear()
            elif mutation == 'size':
                assets[item['packed']['name']]['size'] += 1
            elif mutation == 'hash':
                assets[item['packed']['name']]['digest'] = 'sha256:' + 'c' * 64
            elif mutation == 'unsafe':
                item['name'] = '../ffmpeg.exe'
            elif mutation == 'duplicate':
                manifest['files'].append(copy.deepcopy(item))
            elif mutation == 'no-tool':
                item['name'] = 'notes.txt'
                item['packed']['name'] = 'notes.txt.lzms'
                assets = {'notes.txt.lzms': dict(size=20, digest='sha256:' + 'b' * 64)}
            elif mutation == 'boolean-size':
                item['size'] = True
            else:
                item['size'] = catalog.MAX_FILE + 1
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                catalog.validate_manifest('ffmpeg', '1.0.3', manifest, assets)

    def test_complete_import_writes_only_after_four_verified_manifests(self):
        repository = 'owner/pulse'
        responses = {}
        manifest_urls = []
        for key in catalog.SYMBOLS:
            manifest, assets = self.fixture(key)
            tag = 'pack-' + key + '-1.0.3-arm64'
            name = 'pulse-pack-' + key + '-1.0.3-arm64.json'
            url = 'https://github.com/' + repository + '/releases/download/' + tag + '/' + name
            payload = json.dumps(manifest).encode()
            manifest_urls.append(url)
            responses[url] = payload
            assets[name] = dict(name=name, size=len(payload), digest='sha256:' + hashlib.sha256(payload).hexdigest(),
                                browser_download_url=url)
            responses['https://api.github.com/repos/' + repository + '/releases/tags/' + tag] = json.dumps(
                dict(draft=False, tag_name=tag, assets=[dict(value, name=asset_name) for asset_name, value in assets.items()])).encode()
        with tempfile.TemporaryDirectory(prefix='pulse-catalog-metadata-') as temporary:
            output = Path(temporary) / 'native.h'
            argv = ['import_arm64_pack_catalog.py', '--repository', repository, '--output', str(output)]
            for key in catalog.SYMBOLS:
                argv += ['--' + key + '-version', '1.0.3']
            output.write_text('unchanged-on-failure', encoding='utf-8')
            good_last = responses[manifest_urls[-1]]
            responses[manifest_urls[-1]] = b'corrupted manifest'
            with patch.object(catalog, 'fetch', side_effect=responses.__getitem__), patch('sys.argv', argv):
                with self.assertRaises(ValueError):
                    catalog.main()
            self.assertEqual(output.read_text(encoding='utf-8'), 'unchanged-on-failure')
            responses[manifest_urls[-1]] = good_last
            with patch.object(catalog, 'fetch', side_effect=responses.__getitem__), patch('sys.argv', argv):
                catalog.main()
            text = output.read_text(encoding='utf-8')
            self.assertEqual(text.count('inline constexpr PackRelease '), 4)
            self.assertNotIn('nullptr', text)


if __name__ == '__main__':
    unittest.main()
