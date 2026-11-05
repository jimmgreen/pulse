import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from ensure_release_packs import catalog, catalog_source, ensure


class ReleaseGate(unittest.TestCase):
    def setUp(self):
        self.packs = []
        self.assets = {}
        for key in ['ffmpeg', 'images', 'raw', 'archive']:
            blob = (key + '-fixture').encode()
            name = key + '.lzms'
            self.assets[name] = blob
            self.packs.append(dict(id=key, version='1', release_tag='v1', files=[dict(
                name=key, size=100, sha256='a' * 64, packed_size=len(blob),
                packed_sha256=hashlib.sha256(blob).hexdigest(), download_name=name)]))
        self.remote = {'v1': {}, 'source': dict(self.assets)}
        self.uploads = 0
        self.corrupt_uploaded = False

    def gh(self, *args):
        command, tag = args[1:3]
        if command == 'view':
            return json.dumps({'isDraft': tag == 'v1'})
        if command == 'download':
            name = args[args.index('--pattern') + 1]
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

    def test_catalog_is_complete_and_tag_bound(self):
        root = Path(__file__).resolve().parents[1]
        header = root / 'src/app/pack_catalog_generated.h'
        # The fork deliberately pins the upstream preview-pack release.
        source_repository, source_tag = catalog_source(header)
        self.assertEqual(source_repository, 'jimmgreen/pulse')
        self.assertEqual(source_tag, 'v1.0.58')
        packs = catalog(header, source_repository, source_tag)
        self.assertEqual(sum(len(p['files']) for p in packs), 17)
        with self.assertRaises(ValueError):
            catalog(header, source_repository, 'wrong-tag')

    def test_fork_release_verifies_upstream_assets_without_uploading(self):
        ensure(self.packs, 'test/repo', 'v1', 'source', gh=self.gh,
               source_repository='jimmgreen/pulse', verify_only=True)
        self.assertEqual(self.uploads, 0)
        self.assertEqual(self.remote['v1'], {})

    def test_fork_verification_rejects_missing_or_corrupt_upstream_assets(self):
        for corrupted in [None, b'x' * len(self.assets['ffmpeg.lzms'])]:
            with self.subTest(corrupted=corrupted):
                self.remote['source'] = dict(self.assets)
                if corrupted is None:
                    del self.remote['source']['ffmpeg.lzms']
                else:
                    self.remote['source']['ffmpeg.lzms'] = corrupted
                with self.assertRaises((ValueError, KeyError)):
                    ensure(self.packs, 'test/repo', 'v1', 'source', gh=self.gh,
                           source_repository='jimmgreen/pulse', verify_only=True)
                self.assertEqual(self.uploads, 0)

    def test_new_draft_can_copy_verified_published_bundle(self):
        ensure(self.packs, 'test/repo', 'v1', 'source', gh=self.gh)
        self.assertEqual(self.uploads, 1)
        self.assertEqual(len(self.remote['v1']), 8)

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
