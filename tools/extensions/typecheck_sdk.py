"""Compile the current native manifest SDK with the pinned TypeScript compiler."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
# Every assertion below uses an exported current type. Removed UI/command/resource
# APIs are checked separately, so missing types cannot mask an invalid fixture.
NEGATIVE = [
    'const wrongApi: ExtensionManifest = {apiVersion:2,id:"x",name:"X"};',
    'const missingId: ExtensionManifest = {apiVersion:1,name:"X"};',
    'const missingName: ExtensionManifest = {apiVersion:1,id:"x"};',
    'const wrongId: ExtensionManifest = {apiVersion:1,id:42,name:"X"};',
    'const wrongName: ExtensionManifest = {apiVersion:1,id:"x",name:false};',
    'const unknownResource: ResourceDeclaration = "files.anyPath";',
    'const numericResource: ExtensionManifest = {apiVersion:1,id:"x",name:"X",resources:[42]};',
    'const badResourceContainer: ExtensionManifest = {apiVersion:1,id:"x",name:"X",resources:"context.read"};',
    'const absentOptional: ExtensionManifest = {apiVersion:1,id:"x",name:"X",native:undefined};',
    'const wrongNativeVersion: NativePackage = {version:2,architecture:"windows-x64",library:"a.dll",sha256:"hash"};',
    'const wrongArchitecture: NativePackage = {version:1,architecture:"windows-arm64",library:"a.dll",sha256:"hash"};',
    'const missingLibrary: NativePackage = {version:1,architecture:"windows-x64",sha256:"hash"};',
    'const missingHash: NativePackage = {version:1,architecture:"windows-x64",library:"a.dll"};',
    'const numericLibrary: NativePackage = {version:1,architecture:"windows-x64",library:1,sha256:"hash"};',
    'const numericHash: NativePackage = {version:1,architecture:"windows-x64",library:"a.dll",sha256:42};',
    'const wrongFilePath: NonNullable<NativePackage["files"]>[number] = {path:1,sha256:"hash"};',
    'const missingFileHash: NonNullable<NativePackage["files"]>[number] = {path:"license.txt"};',
    'const numericFileHash: NonNullable<NativePackage["files"]>[number] = {path:"license.txt",sha256:42};',
    'const wrongFiles: NativePackage = {version:1,architecture:"windows-x64",library:"a.dll",sha256:"hash",files:[null]};',
    'const absentFiles: NativePackage = {version:1,architecture:"windows-x64",library:"a.dll",sha256:"hash",files:undefined};',
    'const extraFile: NonNullable<NativePackage["files"]>[number] = {path:"a.dll",sha256:"hash",execute:true};',
    'const extraNative: NativePackage = {version:1,architecture:"windows-x64",library:"a.dll",sha256:"hash",trusted:true};',
    'const wrongTemplate: GetExtensionTemplateArgs = {template:42};',
    'const missingTemplate: GetExtensionTemplateArgs = {};',
    'const invalidValidation: ExtensionManifestValidationResult = {valid:true,error:"failure"};',
    'const wrongValidationArgs: ValidateExtensionManifestArgs = {manifest:{apiVersion:1,id:"x",name:"X"}};',
    'const wrongDiscovery: ExtensionApiDescription = {...hostApi,capabilities:["workflow.run"]};',
    'const wrongBindings: ExtensionApiDescription = {...hostApi,bindings:["context.directory"]};',
    'const wrongAbi: ExtensionApiDescription = {...hostApi,nativeSdk:{...hostApi.nativeSdk,abiVersion:2}};',
]
REMOVED_FIELDS = {
    'commands': '[{id:"x",title:"X",capability:"runtime.invoke",args:{entrypoint:"run"}}]',
    'runtime': '{kind:"javascript",version:1,source:"function run() {}",entrypoints:["run"]}',
    'workflows': '[{id:"w",steps:["x"]}]',
    'panel': '{title:"Panel",ui:{version:1,root:{type:"text",id:"r"}}}',
    'toolbar': '["x"]', 'palette': '["x"]',
}
REMOVED_TYPES = ('ExtensionCommand', 'ExtensionPanelField', 'ExtensionTemplateResult',
                 'UiNode', 'ExtensionPanel', 'ResourceRequest', 'ResourceResponse',
                 'ResourceResponseFor', 'SortColumn', 'SortDirection')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk-root', type=Path, default=ROOT)
    parser.add_argument('--tsc', type=Path, required=True)
    parser.add_argument('--node', default='node')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    sdk, output = args.sdk_root.absolute(), args.output.absolute()
    output.mkdir(exist_ok=False)
    expected = json.loads((ROOT / 'tools/extensions/typecheck/package.json').read_text(encoding='utf-8'))['devDependencies']['typescript']
    version = subprocess.run([args.node, str(args.tsc.absolute()), '--version'], capture_output=True, timeout=30)
    if version.returncode or version.stdout.decode('utf-8').strip() != 'Version ' + expected:
        raise RuntimeError('TypeScript compiler does not match the pinned version ' + expected)
    names = json.loads((sdk / 'examples/native/catalog.json').read_text(encoding='utf-8'))['templates']
    examples = [json.loads((sdk / 'examples/native' / name / 'manifest.json').read_text(encoding='utf-8')) for name in names]
    module = os.path.relpath(sdk / 'sdk/typescript/index', output).replace('\\', '/')
    if not module.startswith('.'):
        module = './' + module
    code = 'import type {ExtensionManifest, NativePackage, ResourceDeclaration, ExtensionApiDescription, GetExtensionTemplateArgs, ValidateExtensionManifestArgs, ExtensionManifestValidationResult} from ' + json.dumps(module) + ';\n'
    code += 'const examples: ExtensionManifest[] = ' + json.dumps(examples, ensure_ascii=True) + ';\n'
    schema = json.loads((sdk / 'schemas/extensions/manifest.schema.json').read_text(encoding='utf-8'))
    code += 'const resources: ResourceDeclaration[] = ' + json.dumps(schema['properties']['resources']['items']['enum']) + ';\n'
    code += 'const minimal: ExtensionManifest = {apiVersion:1,id:"x",name:"X"};\n'
    code += 'const native: NativePackage = {version:1,architecture:"windows-x64",library:"a.dll",sha256:"0".repeat(64),files:[{path:"License.txt",sha256:"1".repeat(64)}]};\n'
    code += 'const packaged: ExtensionManifest = {...minimal,native,resources};\n'
    code += 'const validateArgs: ValidateExtensionManifestArgs = {manifest:JSON.stringify(packaged)};\n'
    code += 'const valid: ExtensionManifestValidationResult = {valid:true,error:""};\n'
    code += 'const invalid: ExtensionManifestValidationResult = {valid:false,error:"invalid_native"};\n'
    code += 'const templateArgs: GetExtensionTemplateArgs[] = ' + json.dumps([{'template': name} for name in names]) + ';\n'
    api_path = sdk / 'schemas/extensions/host-api.json'
    if api_path.is_file():
        api = json.loads(api_path.read_text(encoding='utf-8'))
    else:
        import generate_sdk
        api = generate_sdk.describe(schema)
    code += 'const hostApi: ExtensionApiDescription = ' + json.dumps(api, ensure_ascii=True) + ';\n'
    assertions = list(NEGATIVE)
    for field, value in REMOVED_FIELDS.items():
        assertions.append('const removed_' + field + ': ExtensionManifest = {apiVersion:1,id:"x",name:"X",' + field + ':' + value + '};')
    for name in REMOVED_TYPES:
        assertions.append('import type {' + name + '} from ' + json.dumps(module) + ';')
    for case in assertions:
        code += '// @ts-expect-error This violates the current native extension contract.\n' + case + '\n'
    fixture = output / 'sdk-contract.ts'
    fixture.write_text(code, encoding='utf-8')
    arguments = [args.node, str(args.tsc.absolute()), '--noEmit', '--strict', '--exactOptionalPropertyTypes',
                 '--target', 'ES2020', '--module', 'commonjs', '--moduleResolution', 'node', '--pretty', 'false', str(fixture)]
    result = subprocess.run(arguments, cwd=output, capture_output=True, timeout=60)
    (output / 'compiler.log').write_bytes(result.stdout + result.stderr)
    report = {'compilerVersion': expected, 'templates': names, 'positiveTemplateCount': len(examples),
              'negativeAssertionCount': len(assertions), 'hostApiCompiled': True,
              'strict': True, 'exactOptionalPropertyTypes': True, 'exitCode': result.returncode}
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report), flush=True)
    if result.returncode:
        print((result.stdout + result.stderr).decode('utf-8', errors='replace'), file=sys.stderr)
    return result.returncode


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')
    raise SystemExit(main())
