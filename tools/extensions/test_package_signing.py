import ctypes
import hashlib
import json
import os
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest
import threading
from unittest.mock import patch
import package_signing as signing
import package_store as store


@unittest.skipUnless(os.name == 'nt', 'Windows CNG required')
class SignatureTests(unittest.TestCase):
    def test_atomic_policy_write_with_real_target_reader(self):
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                      ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / 'signature-policy.json'
            target.write_bytes(b'old-policy')
            handle = kernel.CreateFileW(str(target), 0x80000000, 5, None, 3, 0x200000, None)
            self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
            released = threading.Timer(.12, lambda: kernel.CloseHandle(handle))
            released.start()
            try:
                signing.atomic_write(target, b'new-policy')
            finally:
                released.join()
            self.assertEqual(target.read_bytes(), b'new-policy')
            self.assertEqual(list(Path(temporary).glob('.signature-*')), [])
            handle = kernel.CreateFileW(str(target), 0x80000000, 5, None, 3, 0x200000, None)
            self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
            try:
                with self.assertRaises(OSError):
                    signing.atomic_write(target, b'must-not-commit')
            finally:
                kernel.CloseHandle(handle)
            self.assertEqual(target.read_bytes(), b'new-policy')
            self.assertEqual(list(Path(temporary).glob('.signature-*')), [])

    def test_atomic_policy_retry_budget_and_unrelated_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / 'signature-policy.json'
            target.write_bytes(b'original')
            blocked = OSError('injected sharing failure')
            blocked.winerror = 32
            with patch.object(signing.os, 'replace', side_effect=blocked) as replace, patch.object(signing.time, 'sleep') as sleep:
                with self.assertRaises(OSError):
                    signing.atomic_write(target, b'new')
                self.assertEqual(replace.call_count, 50)
                self.assertEqual(sleep.call_count, 49)
                self.assertTrue(all(call.args == (.01,) for call in sleep.call_args_list))
            unrelated = OSError('injected invalid parameter')
            unrelated.winerror = 87
            with patch.object(signing.os, 'replace', side_effect=unrelated) as replace, patch.object(signing.time, 'sleep') as sleep:
                with self.assertRaises(OSError):
                    signing.atomic_write(target, b'new')
                self.assertEqual(replace.call_count, 1)
                sleep.assert_not_called()
            self.assertEqual(target.read_bytes(), b'original')
            self.assertEqual(list(Path(temporary).glob('.signature-*')), [])

    @classmethod
    def setUpClass(cls):
        cng = signing.Cng()
        try:
            cng.check(cng.dll.BCryptGenerateKeyPair(cng.algorithm, ctypes.byref(cng.key), 2048, 0))
            cng.check(cng.dll.BCryptFinalizeKeyPair(cng.key, 0))
            cls.private, cls.public = cng.export(True), cng.export()
        finally:
            cng.close()
        cls.pin = hashlib.sha256(cls.public).hexdigest()
        cls.policy = {'formatVersion': 1, 'requireSigned': True, 'publishers': [{'id': cls.pin, 'publicKey': cls.public.hex()}]}
        cls.data = json.dumps({'apiVersion': 1, 'id': 'signed', 'name': 'Signed', 'commands': [{'id': 'refresh', 'title': 'Refresh', 'capability': 'view.refresh'}]}).encode()

    def test_real_cng_signature_and_tamper(self):
        signature = signing.sign(self.data, self.private)
        self.assertTrue(signing.verify(self.data, signature, self.policy)['trusted'])
        with self.assertRaises(ValueError):
            signing.verify(self.data + b' ', signature, self.policy)
        bad = dict(signature, signature='00' * 256)
        with self.assertRaises(ValueError):
            signing.verify(self.data, bad, self.policy)
        with self.assertRaises(ValueError):
            signing.verify(self.data, dict(signature, formatVersion=2), self.policy)

    def test_unknown_unsigned_and_key_pin(self):
        signature = signing.sign(self.data, self.private)
        self.assertFalse(signing.verify(self.data)['signed'])
        for sig, policy in ((None, self.policy), (signature, None), (signature, dict(self.policy, publishers=[]))):
            with self.assertRaises(ValueError):
                signing.verify(self.data, sig, policy)
        with self.assertRaises(ValueError):
            signing.verify(self.data, signature, dict(self.policy, publishers=[{'id': '0' * 64, 'publicKey': self.public.hex()}]))

    def test_signed_store_enable_and_independent_permission_upgrade(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'signature-policy.json').write_text(json.dumps(self.policy), encoding='utf-8')
            with self.assertRaises(store.StoreError):
                store.install(root, self.data)
            sig = json.dumps(signing.sign(self.data, self.private)).encode()
            first = store.install(root, self.data, signature=sig)
            store.update(root, 'signed', 'enable')
            changed = json.loads(self.data)
            changed['resources'] = ['settings.own']
            data = json.dumps(changed).encode()
            signed = json.dumps(signing.sign(data, self.private)).encode()
            with self.assertRaises(store.StoreError) as rejected:
                store.install(root, data, signature=signed)
            self.assertEqual(rejected.exception.code, 'capability_confirmation_required')
            store.install(root, data, True, signed)
            revision = first['package']['activeRevision']
            (root / 'versions' / (revision + '.sig.json')).write_text('{}', encoding='utf-8')
            with self.assertRaises(store.StoreError):
                store.update(root, 'signed', 'rollback')

    def test_interrupted_signed_upgrade_keeps_verified_active_revision(self):
        for boundary in ('signature', 'registry'):
            with self.subTest(boundary=boundary), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                (root / 'signature-policy.json').write_text(json.dumps(self.policy), encoding='utf-8')
                initial = store.install(root, self.data, signature=json.dumps(signing.sign(self.data, self.private)).encode())
                store.update(root, 'signed', 'enable')
                before = (root / 'registry.json').read_bytes()
                changed = json.loads(self.data)
                changed['name'] = 'Upgraded signed fixture'
                data = json.dumps(changed).encode()
                signature = json.dumps(signing.sign(data, self.private)).encode()
                target = 'package_signing.atomic_write' if boundary == 'signature' else 'package_store.save'
                with patch(target, side_effect=OSError('injected interrupted commit')):
                    with self.assertRaises(OSError):
                        store.install(root, data, signature=signature)
                self.assertEqual((root / 'registry.json').read_bytes(), before)
                active = store.read_registry(root)['packages'][0]['activeRevision']
                self.assertEqual(active, initial['package']['activeRevision'])
                original, _ = store.revision(root, active)
                store.check_signature(root, original, active)
                recovered = store.install(root, data, signature=signature)
                self.assertEqual(recovered['package']['previousRevision'], active)
                store.update(root, 'signed', 'rollback')
                self.assertEqual(store.read_registry(root)['packages'][0]['activeRevision'], active)

    def test_sign_trust_verify_cli_with_temporary_keys(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            private, public, manifest = root / "private.cng", root / "public.cng", root / "manifest.json"
            private.write_bytes(self.private)
            public.write_bytes(self.public)
            manifest.write_bytes(self.data)
            script = Path(signing.__file__)
            signature, store_root = root / "manifest.sig.json", root / "store"
            def run(*args):
                return subprocess.run([sys.executable, str(script), *map(str, args)], capture_output=True, timeout=30)
            result = run("sign", manifest, "--private-key", private, "--output", signature)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
            rejected = run("trust", "--store", store_root, "--public-key", public, "--fingerprint", "0" * 64)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertFalse((store_root / "signature-policy.json").exists())
            result = run("trust", "--store", store_root, "--public-key", public, "--fingerprint", self.pin, "--require-signed")
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
            result = run("verify", manifest, "--signature", signature, "--policy", store_root / "signature-policy.json")
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
            self.assertTrue(json.loads(result.stdout)["trusted"])

    def test_empty_policy_and_signature_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'signature-policy.json').write_bytes(b'')
            with self.assertRaises(store.StoreError):
                store.install(root, self.data)
        with self.assertRaises(ValueError):
            signing.parse(b'{"formatVersion":1,"formatVersion":1}')

    def test_rotation_untrust_and_policy_strength_preserved(self):
        cng = signing.Cng()
        try:
            cng.check(cng.dll.BCryptGenerateKeyPair(cng.algorithm, ctypes.byref(cng.key), 2048, 0))
            cng.check(cng.dll.BCryptFinalizeKeyPair(cng.key, 0))
            new_private, new_public = cng.export(True), cng.export()
        finally:
            cng.close()
        new_pin = hashlib.sha256(new_public).hexdigest()
        old_signature = signing.sign(self.data, self.private)
        new_signature = signing.sign(self.data, new_private)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            policy_path = root / 'signature-policy.json'
            policy_path.write_text(json.dumps(self.policy), encoding='utf-8')
            public_path = root / 'new-public.cng'
            public_path.write_bytes(new_public)
            def run(*args):
                return subprocess.run([sys.executable, str(Path(signing.__file__)), *map(str, args)],
                                      capture_output=True, timeout=30)
            result = run('trust', '--store', root, '--public-key', public_path, '--fingerprint', new_pin)
            self.assertEqual(result.returncode, 0, result.stderr)
            policy = signing.parse(policy_path.read_bytes())
            self.assertTrue(policy['requireSigned'])
            self.assertEqual(set(signing.policy_keys(policy)), {self.pin, new_pin})
            self.assertTrue(signing.verify(self.data, old_signature, policy)['trusted'])
            self.assertTrue(signing.verify(self.data, new_signature, policy)['trusted'])
            before = policy_path.read_bytes()
            for pin in ('0' * 64, 'a' * 63, 'g' * 64, self.pin.upper(), self.pin + '\n'):
                result = run('untrust', '--store', root, '--fingerprint', pin)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(policy_path.read_bytes(), before)
            with store.locked(root):
                result = run('untrust', '--store', root, '--fingerprint', self.pin)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b'store_busy', result.stderr)
                self.assertEqual(policy_path.read_bytes(), before)
            result = run('untrust', '--store', root, '--fingerprint', self.pin)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(json.loads(result.stdout)['reloadRequired'])
            policy = signing.parse(policy_path.read_bytes())
            self.assertTrue(policy['requireSigned'])
            self.assertEqual(set(signing.policy_keys(policy)), {new_pin})
            with self.assertRaisesRegex(ValueError, 'unknown_publisher'):
                signing.verify(self.data, old_signature, policy)
            self.assertTrue(signing.verify(self.data, new_signature, policy)['trusted'])
            policy_path.write_bytes(b'{"formatVersion":1}')
            result = run('untrust', '--store', root, '--fingerprint', new_pin)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(policy_path.read_bytes(), b'{"formatVersion":1}')


if __name__ == '__main__':
    unittest.main()
