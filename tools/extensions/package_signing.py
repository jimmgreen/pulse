"""Detached manifest signatures using Windows CNG; no private-key generation CLI."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile
import time

PREFIX = b'Pulse.Extension.Manifest.Signature\0v1\0'
ALGORITHM = 'RSA-PKCS1-SHA256'


def parse(data):
    def unique(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('Duplicate signature JSON field')
            result[key] = value
        return result
    return json.loads(data.decode('utf-8'), object_pairs_hook=unique,
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError('Invalid JSON constant')))


def exact(value, fields):
    if not isinstance(value, dict) or set(value) != set(fields):
        raise ValueError('Invalid signature fields')


def hex_bytes(value):
    if not isinstance(value, str) or not value or len(value) > 8192 or len(value) % 2 or any(c not in '0123456789abcdef' for c in value):
        raise ValueError('Invalid canonical signature hex')
    return bytes.fromhex(value)


def fingerprint(value):
    if not isinstance(value, str) or len(value) != 64 or any(c not in '0123456789abcdef' for c in value):
        raise ValueError('Fingerprint must be 64 lowercase hexadecimal characters')
    return value


def public_blob(blob):
    if len(blob) < 24:
        raise ValueError('Invalid public key')
    magic, bits, exponent, modulus, prime1, prime2 = struct.unpack('<6I', blob[:24])
    if magic != 0x31415352 or not 2048 <= bits <= 4096 or bits % 8 or modulus != bits // 8 or not 1 <= exponent <= 8 or prime1 or prime2 or len(blob) != 24 + exponent + modulus:
        raise ValueError('Only RSA2048–4096 CNG public blobs are accepted')
    return blob


class Cng:
    def __init__(self):
        if os.name != 'nt':
            raise ValueError('CNG signature operations require Windows')
        self.dll = ctypes.WinDLL('bcrypt.dll')
        void, ulong = ctypes.c_void_p, ctypes.c_ulong
        signatures = {
            'BCryptOpenAlgorithmProvider': ([ctypes.POINTER(void), ctypes.c_wchar_p, ctypes.c_wchar_p, ulong], ctypes.c_long),
            'BCryptCloseAlgorithmProvider': ([void, ulong], ctypes.c_long),
            'BCryptImportKeyPair': ([void, void, ctypes.c_wchar_p, ctypes.POINTER(void), void, ulong, ulong], ctypes.c_long),
            'BCryptExportKey': ([void, void, ctypes.c_wchar_p, void, ulong, ctypes.POINTER(ulong), ulong], ctypes.c_long),
            'BCryptDestroyKey': ([void], ctypes.c_long),
            'BCryptSignHash': ([void, void, void, ulong, void, ulong, ctypes.POINTER(ulong), ulong], ctypes.c_long),
            'BCryptVerifySignature': ([void, void, void, ulong, void, ulong, ulong], ctypes.c_long),
            'BCryptGenerateKeyPair': ([void, ctypes.POINTER(void), ulong, ulong], ctypes.c_long),
            'BCryptFinalizeKeyPair': ([void, ulong], ctypes.c_long),
        }
        for name, (args, result) in signatures.items():
            fn = getattr(self.dll, name)
            fn.argtypes, fn.restype = args, result
        self.algorithm, self.key = void(), void()
        self.check(self.dll.BCryptOpenAlgorithmProvider(ctypes.byref(self.algorithm), 'RSA', None, 0))

    @staticmethod
    def check(status):
        if status < 0:
            raise ValueError('CNG signature operation failed: %08x' % (status & 0xffffffff))

    def close(self):
        if self.key:
            self.dll.BCryptDestroyKey(self.key)
            self.key = ctypes.c_void_p()
        if self.algorithm:
            self.dll.BCryptCloseAlgorithmProvider(self.algorithm, 0)
            self.algorithm = ctypes.c_void_p()

    def import_key(self, blob, private=False):
        if len(blob) > 8192:
            raise ValueError('Key exceeds size limit')
        buffer = ctypes.create_string_buffer(blob)
        self.check(self.dll.BCryptImportKeyPair(self.algorithm, None, 'RSAFULLPRIVATEBLOB' if private else 'RSAPUBLICBLOB', ctypes.byref(self.key), buffer, len(blob), 0))

    def export(self, private=False):
        size = ctypes.c_ulong()
        kind = 'RSAFULLPRIVATEBLOB' if private else 'RSAPUBLICBLOB'
        self.check(self.dll.BCryptExportKey(self.key, None, kind, None, 0, ctypes.byref(size), 0))
        result = ctypes.create_string_buffer(size.value)
        self.check(self.dll.BCryptExportKey(self.key, None, kind, result, size.value, ctypes.byref(size), 0))
        return result.raw[:size.value]

    def signature(self, manifest, signature=None):
        class Padding(ctypes.Structure):
            _fields_ = [('algorithm', ctypes.c_wchar_p)]
        padding = Padding('SHA256')
        digest = hashlib.sha256(PREFIX + manifest).digest()
        digest_buffer = ctypes.create_string_buffer(digest)
        if signature is not None:
            signature_buffer = ctypes.create_string_buffer(signature)
            self.check(self.dll.BCryptVerifySignature(self.key, ctypes.byref(padding), digest_buffer, len(digest), signature_buffer, len(signature), 2))
            return None
        size = ctypes.c_ulong()
        self.check(self.dll.BCryptSignHash(self.key, ctypes.byref(padding), digest_buffer, len(digest), None, 0, ctypes.byref(size), 2))
        result = ctypes.create_string_buffer(size.value)
        self.check(self.dll.BCryptSignHash(self.key, ctypes.byref(padding), digest_buffer, len(digest), result, size.value, ctypes.byref(size), 2))
        return result.raw[:size.value]


def sign(manifest, private):
    if len(manifest) > 65536:
        raise ValueError('Manifest exceeds size limit')
    cng = Cng()
    try:
        cng.import_key(private, True)
        public = public_blob(cng.export())
        signature = cng.signature(manifest)
        return {'formatVersion': 1, 'algorithm': ALGORITHM, 'publisher': hashlib.sha256(public).hexdigest(), 'signature': signature.hex()}
    finally:
        cng.close()


def policy_keys(policy):
    exact(policy, ('formatVersion', 'requireSigned', 'publishers'))
    if type(policy['formatVersion']) is not int or policy['formatVersion'] != 1 or type(policy['requireSigned']) is not bool or not isinstance(policy['publishers'], list) or len(policy['publishers']) > 32:
        raise ValueError('Invalid signature policy')
    keys = {}
    for entry in policy['publishers']:
        exact(entry, ('id', 'publicKey'))
        blob = public_blob(hex_bytes(entry['publicKey']))
        pin = hashlib.sha256(blob).hexdigest()
        if entry['id'] != pin or pin in keys:
            raise ValueError('Invalid or duplicate publisher pin')
        keys[pin] = blob
    return keys


def verify(manifest, signature=None, policy=None):
    if len(manifest) > 65536:
        raise ValueError('Manifest exceeds size limit')
    keys = policy_keys(policy) if policy is not None else {}
    if signature is None:
        if policy and policy['requireSigned']:
            raise ValueError('signature_required')
        return {'signed': False, 'trusted': False}
    exact(signature, ('formatVersion', 'algorithm', 'publisher', 'signature'))
    if type(signature['formatVersion']) is not int or signature['formatVersion'] != 1 or signature['algorithm'] != ALGORITHM:
        raise ValueError('unsupported_signature_format')
    if not isinstance(signature['publisher'], str) or signature['publisher'] not in keys:
        raise ValueError('unknown_publisher')
    sig = hex_bytes(signature['signature'])
    cng = Cng()
    try:
        cng.import_key(keys[signature['publisher']])
        cng.signature(manifest, sig)
    finally:
        cng.close()
    return {'signed': True, 'trusted': True, 'publisher': signature['publisher']}


def atomic_write(path, data):
    import package_store as store
    store.ordinary(path)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".signature-", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        # Windows legacy replace can reject a target held by a short-lived reader,
        # even when that reader shares deletion. Keep the original policy until commit.
        for attempt in range(50):
            try:
                os.replace(temporary, path)
                break
            except OSError as error:
                if getattr(error, 'winerror', None) not in (5, 32, 33) or attempt == 49:
                    raise
                time.sleep(.01)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main():
    import package_store as store
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    sign_parser = commands.add_parser('sign')
    sign_parser.add_argument('manifest', type=Path)
    sign_parser.add_argument('--private-key', type=Path, required=True)
    sign_parser.add_argument('--output', type=Path, required=True)
    trust = commands.add_parser('trust')
    trust.add_argument('--store', type=Path, required=True)
    trust.add_argument('--public-key', type=Path, required=True)
    trust.add_argument('--fingerprint', required=True)
    trust.add_argument('--require-signed', action='store_true')
    untrust = commands.add_parser('untrust')
    untrust.add_argument('--store', type=Path, required=True)
    untrust.add_argument('--fingerprint', required=True)
    verification = commands.add_parser('verify')
    verification.add_argument('manifest', type=Path)
    verification.add_argument('--signature', type=Path)
    verification.add_argument('--policy', type=Path)
    args = parser.parse_args()
    if args.command == 'sign':
        result = sign(store.read_bytes(args.manifest, 65536), store.read_bytes(args.private_key, 8192))
        with args.output.open('x', encoding='utf-8') as stream:
            json.dump(result, stream, indent=2)
        print(json.dumps({'signed': True, 'publisher': result['publisher']}))
    elif args.command == 'trust':
        fingerprint(args.fingerprint)
        blob = public_blob(store.read_bytes(args.public_key, 8192))
        pin = hashlib.sha256(blob).hexdigest()
        if pin != args.fingerprint:
            raise ValueError('Public key fingerprint does not match explicit pin')
        with store.locked(args.store, create=True) as root:
            path = root / 'signature-policy.json'
            policy = parse(store.read_bytes(path, 65536)) if path.exists() else {'formatVersion': 1, 'requireSigned': False, 'publishers': []}
            keys = policy_keys(policy)
            if pin not in keys:
                policy['publishers'].append({'id': pin, 'publicKey': blob.hex()})
            policy['requireSigned'] |= args.require_signed
            policy_keys(policy)
            atomic_write(path, (json.dumps(policy, indent=2) + '\n').encode('utf-8'))
        print(json.dumps({'trustedPublisher': pin, 'requireSigned': policy['requireSigned']}))
    elif args.command == 'untrust':
        pin = fingerprint(args.fingerprint)
        with store.locked(args.store) as root:
            path = root / 'signature-policy.json'
            policy = parse(store.read_bytes(path, 65536))
            keys = policy_keys(policy)
            if pin not in keys:
                raise ValueError('Publisher fingerprint is not trusted')
            policy['publishers'] = [entry for entry in policy['publishers'] if entry['id'] != pin]
            policy_keys(policy)
            atomic_write(path, (json.dumps(policy, indent=2) + '\n').encode('utf-8'))
        print(json.dumps({'untrustedPublisher': pin, 'requireSigned': policy['requireSigned'],
                          'reloadRequired': True}))
    else:
        result = verify(store.read_bytes(args.manifest, 65536), parse(store.read_bytes(args.signature, 16384)) if args.signature else None, parse(store.read_bytes(args.policy, 65536)) if args.policy else None)
        print(json.dumps(result))


if __name__ == '__main__':
    main()
