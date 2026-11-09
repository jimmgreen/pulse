"""Generate the extension capability inventory without granting public access."""
import argparse
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / 'schemas/extensions/capability_inventory.json'
SOURCE = ROOT / 'src/app/actions/action_inventory.h'


def inventory(source):
    match = re.search(r'kActions\[\]\s*=\s*\{(.*?)\n\};', source, re.S)
    if not match:
        raise ValueError('Cannot locate kActions; update the inventory parser')
    entries = re.findall(r'\{"([^"\n]+)",\s*ActionRisk::(\w+)(?:,\s*(true|false))?\}', match[1])
    if not entries or len(entries) != match[1].count('ActionRisk::'):
        raise ValueError('Unsupported action declaration; refusing partial inventory')
    if len({entry[0] for entry in entries}) != len(entries):
        raise ValueError('Duplicate internal action ID')
    return {
        'formatVersion': 1,
        'publicApiVersion': None,
        'source': 'src/app/actions/action_inventory.h',
        'capabilities': [
            {'internalActionId': name, 'internalRisk': risk,
             'riskDependsOnTarget': dynamic == 'true',
             'status': 'internal', 'publicId': None}
            for name, risk, dynamic in entries
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    expected = json.dumps(inventory(SOURCE.read_text(encoding='utf-8-sig')),
                          ensure_ascii=False, indent=2) + '\n'
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text(encoding='utf-8') != expected:
            parser.exit(1, 'Capability inventory is stale; run tools/extensions/catalog.py\n')
        print('Capability inventory is current')
    else:
        OUTPUT.write_text(expected, encoding='utf-8')
        print('Generated ' + str(OUTPUT.relative_to(ROOT)))


if __name__ == '__main__':
    main()
