import hashlib
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from ensure_release_packs import catalog, ensure, main, verify_independent


class ReleaseGate(unittest.TestCase):
    def setUp(self):
        self.packs = []
        self.assets = {}
        for key in ['ffmpeg', 'images', 'raw']:
            blob = (key + '-fixture').encode()
            name = key + '.lzms'
            self.assets[name] = blob
            self.packs.append(dict(id=key, version='1', release_tag='v1', files=[dict(
                name=key, size=100, sha256='a' * 64, packed_size=len(blob),
                packed_sha256=hashlib.sha256(blob).hexdigest(), download_name=name)]))
        self.remote = {'v1': {}, 'source': dict(self.assets)}
        self.uploads = 0
        self.corrupt_uploaded = False
        self.draft_sources = set()
        self.downloads = []
        self.calls = []

    def gh(self, *args):
        self.calls.append(args)
        command, tag = args[1:3]
        if command == 'view':
            return json.dumps({'isDraft': tag == 'v1' or tag in self.draft_sources})
        if command == 'download':
            name = args[args.index('--pattern') + 1]
            self.downloads.append((tag, name))
            directory = Path(args[args.index('--dir') + 1])
            (directory / name).write_bytes(self.remote[tag][name])
        elif command == 'upload':
            self.uploads += 1
            for path in args[5:-1]:
                p = Path(path)
                self.remote[tag][p.name] = p.read_bytes()
            if self.corrupt_uploaded:
                self.remote[tag]['ffmpeg.lzms'] = b'bad'
        else:
            raise AssertionError(args)
        return ''

    def published_raw(self):
        raw = next(pack for pack in self.packs if pack['id'] == 'raw')
        raw['release_tag'] = 'pack-raw-' + raw['version']
        assets = {raw['files'][0]['download_name']: self.assets['raw.lzms']}
        for name in ['raw_r.dll', 'THIRD-PARTY-NOTICES.txt']:
            blob = (name + '-fixture').encode()
            download = name + '.lzms'
            assets[download] = blob
            raw['files'].append(dict(name=name, size=100, sha256='a' * 64,
                packed_size=len(blob), packed_sha256=hashlib.sha256(blob).hexdigest(),
                download_name=download))
        self.remote[raw['release_tag']] = assets
        return raw

    def assert_read_only(self):
        self.assertEqual(self.uploads, 0)
        self.assertTrue(all(call[0] == 'release' and call[1] in ['view', 'download']
                            for call in self.calls))

    def test_verify_independent_published_raw_is_read_only(self):
        raw = self.published_raw()
        self.assertEqual(verify_independent(self.packs, 'test/repo', 'raw', '1', gh=self.gh), raw)
        self.assertEqual(self.downloads,
                         [(raw['release_tag'], entry['download_name']) for entry in raw['files']])
        self.assertEqual(len(self.calls), 4)
        self.assert_read_only()

    def test_verify_independent_rejects_wrong_version_and_tag_before_remote(self):
        raw = self.published_raw()
        with self.assertRaisesRegex(ValueError, 'expected version'):
            verify_independent(self.packs, 'test/repo', 'raw', '2', gh=self.gh)
        raw['release_tag'] = 'v1'
        with self.assertRaisesRegex(ValueError, 'catalog pack and version'):
            verify_independent(self.packs, 'test/repo', 'raw', '1', gh=self.gh)
        self.assertEqual(self.calls, [])
        self.assert_read_only()

    def test_verify_independent_rejects_draft_before_download(self):
        raw = self.published_raw()
        self.draft_sources.add(raw['release_tag'])
        with self.assertRaisesRegex(ValueError, 'must be published'):
            verify_independent(self.packs, 'test/repo', 'raw', '1', gh=self.gh)
        self.assertEqual(self.downloads, [])
        self.assert_read_only()

    def test_verify_independent_rejects_bad_hash_size_and_missing_asset(self):
        raw = self.published_raw()
        name = raw['files'][-1]['download_name']
        original = self.remote[raw['release_tag']][name]
        for blob in [b'x' * len(original), b'x', None]:
            with self.subTest(blob=blob):
                if blob is None:
                    del self.remote[raw['release_tag']][name]
                else:
                    self.remote[raw['release_tag']][name] = blob
                with self.assertRaises((ValueError, KeyError)):
                    verify_independent(self.packs, 'test/repo', 'raw', '1', gh=self.gh)
                self.assert_read_only()

    def test_verify_independent_cli_never_stages(self):
        self.published_raw()
        def gh_run(command, **kwargs):
            self.assertEqual(command[0], 'gh')
            return SimpleNamespace(stdout=self.gh(*command[1:]))
        with patch('sys.argv', ['ensure_release_packs.py', '--verify-independent', 'raw',
                               '--expected-pack-version', '1']), \
                patch('ensure_release_packs.catalog', return_value=self.packs), \
                patch('ensure_release_packs.subprocess.run', side_effect=gh_run), \
                patch('ensure_release_packs.ensure') as stage, patch('builtins.print') as output:
            main()
        stage.assert_not_called()
        self.assertEqual(len(self.calls), 4)
        self.assert_read_only()
        self.assertIn('engine was not rebuilt; no assets uploaded or published', output.call_args.args[0])

    def test_staging_cli_preserves_ensure_path(self):
        with patch('sys.argv', ['ensure_release_packs.py', '--tag', 'v1', '--source-tag', 'source']), \
                patch('ensure_release_packs.catalog', return_value=self.packs) as read_catalog, \
                patch('ensure_release_packs.verify_independent') as verify, \
                patch('ensure_release_packs.ensure') as stage, patch('builtins.print'):
            main()
        self.assertEqual(read_catalog.call_args.args[1:], ('jimmgreen/pulse', 'v1'))
        stage.assert_called_once_with(self.packs, 'jimmgreen/pulse', 'v1', 'source', None)
        verify.assert_not_called()

    def test_verify_independent_cli_rejects_staging_arguments(self):
        for arguments in [[], ['--tag', 'v1'], ['--source-tag', 'source'],
                          ['--asset-directory', '.']]:
            args = ['ensure_release_packs.py', '--verify-independent', 'raw']
            if arguments:
                args += ['--expected-pack-version', '1', *arguments]
            with self.subTest(arguments=arguments), patch('sys.argv', args), \
                    patch('sys.stderr'), patch('ensure_release_packs.catalog') as read_catalog, \
                    patch('ensure_release_packs.ensure') as stage:
                with self.assertRaises(SystemExit) as error:
                    main()
                self.assertEqual(error.exception.code, 2)
                read_catalog.assert_not_called()
                stage.assert_not_called()

    def test_catalog_is_complete_and_independent(self):
        root = Path(__file__).resolve().parents[1]
        header = root / 'src/app/pack_catalog_generated.h'
        # Independent pack versions survive a main application version bump.
        tag = 'v' + (root / 'version.txt').read_text(encoding='utf-8').strip()
        packs = catalog(header, 'jimmgreen/pulse', tag)
        self.assertEqual({p['id'] for p in packs}, {'ffmpeg', 'images', 'raw'})
        self.assertEqual(sum(len(p['files']) for p in packs), 16)
        for pack in packs:
            self.assertEqual(pack['release_tag'], 'pack-' + pack['id'] + '-' + pack['version'])
        self.assertEqual(catalog(header, 'jimmgreen/pulse', 'v99.0.0'), packs)
        media = next(p for p in packs if p['id'] == 'ffmpeg')
        self.assertEqual(media['release_tag'], 'pack-ffmpeg-' + media['version'])
        self.assertEqual(media['files'][0]['download_name'], media['files'][0]['name'] + '.lzms')
        with self.assertRaises(ValueError):
            catalog(header, 'other/repo', tag)
        original = header.read_text(encoding='utf-8')
        raw = next(p for p in packs if p['id'] == 'raw')
        replacements = [(media['release_tag'], replacement) for replacement in
                        ['arbitrary-release', 'pack-images-1.0.5', 'pack-ffmpeg-9.9.9']]
        replacements.append((raw['release_tag'], 'pack-raw-9.9.9'))
        for release_tag, replacement in replacements:
            with self.subTest(replacement=replacement), tempfile.TemporaryDirectory() as directory:
                changed = Path(directory) / 'catalog.h'
                changed.write_text(original.replace(release_tag, replacement), encoding='utf-8')
                with self.assertRaises(ValueError):
                    catalog(changed, 'jimmgreen/pulse', tag)

    def test_target_bound_catalog_rejects_wrong_target(self):
        root = Path(__file__).resolve().parents[1]
        original = (root / 'src/app/pack_catalog_generated.h').read_text(encoding='utf-8')
        repository = 'jimmgreen/pulse'
        tag = 'v-target-bound-fixture'
        prefix = 'https://github.com/' + repository + '/releases/download/'
        raw = next(pack for pack in catalog(root / 'src/app/pack_catalog_generated.h', repository, tag)
                   if pack['id'] == 'raw')
        independent_url = prefix + raw['release_tag'] + '/'
        self.assertEqual(original.count(independent_url), 1)
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / 'target-bound-catalog.h'
            header.write_text(original.replace(independent_url, prefix + tag + '/'), encoding='utf-8')
            packs = catalog(header, repository, tag)
            self.assertEqual(next(pack for pack in packs if pack['id'] == 'raw')['release_tag'], tag)
            with self.assertRaisesRegex(ValueError, 'Catalog release URL does not match target: raw'):
                catalog(header, repository, 'wrong-tag')

    def test_new_draft_can_copy_verified_published_bundle(self):
        ensure(self.packs, 'test/repo', 'v1', 'source', gh=self.gh)
        self.assertEqual(self.uploads, 1)
        self.assertEqual(len(self.remote['v1']), 6)

    def test_independent_pack_uses_published_catalog_source(self):
        self.packs[0]['release_tag'] = 'pack-ffmpeg-1'
        self.remote['pack-ffmpeg-1'] = {'ffmpeg.lzms': self.assets['ffmpeg.lzms']}
        del self.remote['source']['ffmpeg.lzms']
        ensure(self.packs, 'test/repo', 'v1', 'source', gh=self.gh)
        self.assertIn(('pack-ffmpeg-1', 'ffmpeg.lzms'), self.downloads)
        self.assertNotIn(('source', 'ffmpeg.lzms'), self.downloads)
        self.assertEqual(self.uploads, 1)

    def test_all_independent_packs_work_across_main_versions(self):
        for pack in self.packs:
            pack['release_tag'] = 'pack-' + pack['id'] + '-' + pack['version']
            name = pack['files'][0]['download_name']
            self.remote[pack['release_tag']] = {name: self.assets[name]}
        self.remote['v2'] = {}
        self.draft_sources.add('v2')
        for tag in ['v1', 'v2']:
            with self.subTest(tag=tag):
                ensure(self.packs, 'test/repo', tag, gh=self.gh)
                self.assertEqual(len(self.remote[tag]), 6)
        self.assertEqual(self.uploads, 2)
        for pack in self.packs:
            self.assertEqual(self.downloads.count((pack['release_tag'], pack['files'][0]['download_name'])), 2)

    def test_independent_draft_and_bad_hash_fail_before_upload(self):
        self.packs[0]['release_tag'] = 'pack-ffmpeg-1'
        self.remote['pack-ffmpeg-1'] = {'ffmpeg.lzms': b'x' * len(self.assets['ffmpeg.lzms'])}
        for draft in [True, False]:
            with self.subTest(draft=draft):
                self.draft_sources = {'pack-ffmpeg-1'} if draft else set()
                with self.assertRaises(ValueError):
                    ensure(self.packs, 'test/repo', 'v1', 'source', gh=self.gh)
                self.assertEqual(self.uploads, 0)

    def test_preuploaded_draft_is_reverified(self):
        self.remote['v1'] = dict(self.assets)
        ensure(self.packs, 'test/repo', 'v1', gh=self.gh)
        self.assertEqual(self.uploads, 1)

    def test_missing_and_wrong_hash_fail_before_upload(self):
        for corrupted in [None, b'x' * len(self.assets['ffmpeg.lzms'])]:
            with self.subTest(corrupted=corrupted):
                self.remote['v1'] = dict(self.assets)
                if corrupted is None:
                    del self.remote['v1']['ffmpeg.lzms']
                else:
                    self.remote['v1']['ffmpeg.lzms'] = corrupted
                with self.assertRaises((ValueError, KeyError)):
                    ensure(self.packs, 'test/repo', 'v1', gh=self.gh)
                self.assertEqual(self.uploads, 0)

    def test_local_bundle_and_remote_recheck(self):
        with tempfile.TemporaryDirectory() as directory:
            for name, blob in self.assets.items():
                (Path(directory) / name).write_bytes(blob)
            self.corrupt_uploaded = True
            with self.assertRaises(ValueError):
                ensure(self.packs, 'test/repo', 'v1', asset_directory=directory, gh=self.gh)
            self.assertEqual(self.uploads, 1)


if __name__ == '__main__':
    unittest.main()
