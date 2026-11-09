import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from ensure_release_packs import catalog, ensure


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

    def gh(self, *args):
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

    def test_catalog_is_complete_and_tag_bound(self):
        root = Path(__file__).resolve().parents[1]
        header = root / 'src/app/pack_catalog_generated.h'
        # Bound to the release being built, so a version bump needs no edit here.
        tag = 'v' + (root / 'version.txt').read_text(encoding='utf-8').strip()
        packs = catalog(header, 'jimmgreen/pulse', tag)
        self.assertEqual({p['id'] for p in packs}, {'ffmpeg', 'images', 'raw'})
        self.assertEqual(sum(len(p['files']) for p in packs), 16)
        media = next(p for p in packs if p['id'] == 'ffmpeg')
        self.assertEqual(media['release_tag'], 'pack-ffmpeg-' + media['version'])
        self.assertEqual(media['files'][0]['download_name'], media['files'][0]['name'] + '.lzms')
        with self.assertRaises(ValueError):
            catalog(header, 'jimmgreen/pulse', 'wrong-tag')
        with self.assertRaises(ValueError):
            catalog(header, 'other/repo', tag)
        original = header.read_text(encoding='utf-8')
        for replacement in ['arbitrary-release', 'pack-images-1.0.5', 'pack-ffmpeg-9.9.9']:
            with self.subTest(replacement=replacement), tempfile.TemporaryDirectory() as directory:
                changed = Path(directory) / 'catalog.h'
                changed.write_text(original.replace(media['release_tag'], replacement), encoding='utf-8')
                with self.assertRaises(ValueError):
                    catalog(changed, 'jimmgreen/pulse', tag)

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
