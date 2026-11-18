"""Bounded, read-only discovery comparison. It never certifies runtime compatibility."""
import hashlib
import json

MAX_BYTES = 4 * 1024 * 1024
MAX_CHANGES = 256


def strict_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError('Duplicate contract member: ' + key)
        value[key] = item
    return value


def fingerprint(contract):
    body = {key: value for key, value in contract.items() if key != 'contractIdentity'}
    data = json.dumps(body, sort_keys=True, ensure_ascii=True, separators=(',', ':'), allow_nan=False).encode('ascii')
    return hashlib.sha256(data).hexdigest()


def identity(contract):
    return {'formatVersion': 1, 'algorithm': 'sha256', 'canonicalization': 'sorted-ascii-json-v1',
            'scope': 'discovery-excluding-contractIdentity', 'sha256': fingerprint(contract)}


def validate(contract):
    if (not isinstance(contract, dict) or type(contract.get('apiVersion')) is not int
            or contract['apiVersion'] < 1 or not isinstance(contract.get('manifestSchema'), dict)
            or contract['manifestSchema'].get('type') != 'object' or not isinstance(contract.get('limits'), dict)):
        raise ValueError('Expected a host discovery object, not a manifest or CLI envelope')
    for key in ('capabilities', 'bindings'):
        values = contract.get(key)
        if (not isinstance(values, list) or not all(isinstance(item, str) and item for item in values)
                or len(set(values)) != len(values)):
            raise ValueError('Invalid contract ' + key)
    count = 0
    pending = [(contract, 0)]
    while pending:
        value, depth = pending.pop()
        count += 1
        if count > 200000 or depth > 64:
            raise ValueError('Contract exceeds structural budget')
        if isinstance(value, dict):
            pending.extend((item, depth + 1) for item in value.values())
        elif isinstance(value, list):
            pending.extend((item, depth + 1) for item in value)
    if 'contractIdentity' in contract and contract['contractIdentity'] != identity(contract):
        raise ValueError('Contract identity does not match its discovery content')
    return contract


def load(data):
    if len(data) > MAX_BYTES:
        raise ValueError('Contract exceeds 4 MiB')
    try:
        result = json.loads(data.decode('utf-8-sig'), object_pairs_hook=strict_object,
                            parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
        return validate(result)
    except (RecursionError, UnicodeError) as error:
        raise ValueError('Invalid or excessively nested UTF-8 contract') from error


def compare(baseline, current):
    validate(baseline)
    validate(current)
    changes = []
    truncated = False
    missing = object()

    def walk(before, after, path):
        nonlocal truncated
        if before is not missing and after is not missing and type(before) is type(after) and before == after:
            return
        if len(changes) >= MAX_CHANGES:
            truncated = True
            return
        if isinstance(before, dict) and isinstance(after, dict):
            for key in sorted(before.keys() | after.keys()):
                if not path and key == 'contractIdentity':
                    continue
                escaped = key.replace('~', '~0').replace('/', '~1')
                walk(before.get(key, missing), after.get(key, missing), path + '/' + escaped)
            return
        changes.append({'path': path or '/',
                        'kind': 'added' if before is missing else 'removed' if after is missing else 'changed'})

    walk(baseline, current, '')
    before_hash, after_hash = fingerprint(baseline), fingerprint(current)
    result = {'status': 'same-discovery' if before_hash == after_hash else 'review-required',
              'identical': before_hash == after_hash, 'baselineSha256': before_hash, 'currentSha256': after_hash,
              'baselineApiVersion': baseline['apiVersion'], 'currentApiVersion': current['apiVersion'],
              'changes': changes, 'changesTruncated': truncated, 'grantsAuthority': False,
              'nativeManifestValidationRequired': True, 'runtimeCompatibilityCertified': False,
              'commandsExecuted': 0}
    for field in ('capabilities', 'bindings'):
        result[field] = {'added': sorted(set(current[field]) - set(baseline[field])),
                         'removed': sorted(set(baseline[field]) - set(current[field]))}
    return result
