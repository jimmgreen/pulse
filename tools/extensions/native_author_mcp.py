"""Stdio MCP author server: create, build, package and stage public C++ plugins."""
import argparse
import concurrent.futures
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
MAX_MESSAGE = 1024 * 1024
MAX_LOG = 32768


class AuthorError(Exception):
    pass


def schema(properties=None, required=()):
    return dict(type='object', properties=properties or {}, required=list(required), additionalProperties=False)


STRING = dict(type='string', minLength=1, maxLength=32768)
TOOLS = [
    dict(name='native_sdk_describe', description='Read SDK interface summaries, examples and build requirements. Supply interface to retrieve one complete header contract.', inputSchema=schema(dict(interface=STRING))),
    dict(name='native_project_create', description='Copy one public SDK example into a new author workspace project. Existing files are never replaced.',
         inputSchema=schema(dict(name=dict(type='string', pattern='^[a-z][a-z0-9-]{0,63}$'), template=STRING), ('name', 'template'))),
    dict(name='native_project_build', description='Configure and build a workspace C++ project with CMake/Ninja and public SDK headers. Returns compiler diagnostics; no Pulse private libraries.',
         inputSchema=schema(dict(project=STRING), ('project',))),
    dict(name='native_package_stage', description='Bind the compiled DLL hash, pack and verify a .pulse-ext package using the same Pulse author CLI.',
         inputSchema=schema(dict(project=STRING, library=STRING, files=dict(type='array', items=STRING, maxItems=63)), ('project', 'library'))),
    dict(name='native_package_install', description='Install a verified native package in this author workspace test store. Native consent must be explicit. Enable applies on next host startup.',
         inputSchema=schema(dict(package=STRING, allowNative=dict(type='boolean'), enable=dict(type='boolean')), ('package', 'allowNative'))),
    dict(name='native_host_diagnose', description='Compare this Pulse host native SDK catalog with the author SDK; return version mismatch diagnostics without loading a plugin.', inputSchema=schema()),
    dict(name='native_host_run', description='Launch the configured host with an isolated author profile and test store. Returns the process ID and log path; launch acceptance does not certify plugin loading or UI behavior.',
         inputSchema=schema(dict(allowNative=dict(type='boolean')), ('allowNative',))),
]


def validate(arguments, spec):
    if not isinstance(arguments, dict):
        raise AuthorError('arguments must be an object')
    fields = spec['inputSchema']['properties']
    if set(arguments) - set(fields) or set(spec['inputSchema']['required']) - set(arguments):
        raise AuthorError('unexpected or missing arguments')
    for key, value in arguments.items():
        expected = fields[key]['type']
        if expected == 'array' and (not isinstance(value, list) or len(value) > fields[key].get('maxItems', 63) or
                                   any(not isinstance(item, str) or not item or len(item) > 32768 or '\0' in item for item in value)):
            raise AuthorError(key+' must be an array of at most 63 nonempty path strings')
        if expected == 'boolean' and type(value) is not bool:
            raise AuthorError(key+' must be boolean')
        if expected == 'string' and (not isinstance(value, str) or not value or len(value) > 32768 or '\0' in value):
            raise AuthorError(key+' must be a nonempty path/identifier string')


class Author:
    def __init__(self, workspace, sdk, pulse):
        self.workspace = Path(workspace).resolve()
        self.sdk = Path(sdk).resolve()
        self.pulse = Path(pulse).resolve()
        self.workspace.mkdir(parents=True, exist_ok=True)
        self.catalog = self.sdk/'sdk/native/api-catalog.json'
        if not self.catalog.is_file() or not self.pulse.is_file():
            raise AuthorError('SDK catalog and Pulse host must exist')

    def inside(self, value, exists=True):
        path = (self.workspace/value).resolve()
        try:
            path.relative_to(self.workspace)
        except ValueError as error:
            raise AuthorError('path must remain inside the configured author workspace') from error
        if path == self.workspace:
            raise AuthorError('path must remain inside the configured author workspace')
        if exists and not path.exists():
            raise AuthorError('workspace path does not exist: '+str(path))
        return path

    def cli(self, *arguments):
        return [sys.executable, str(self.sdk/'tools/extensions/pulse_ext.py'), '--pulse', str(self.pulse), '--json', *map(str, arguments)]

    def run(self, command, cancelled, commands, timeout=300, json_reply=False):
        if cancelled.is_set():
            raise AuthorError('cancelled')
        started = time.monotonic()
        with tempfile.TemporaryFile() as log:
            process = subprocess.Popen(command, cwd=self.workspace, stdin=subprocess.DEVNULL,
                                       stdout=log, stderr=subprocess.STDOUT, shell=False)
            reason = None
            while process.poll() is None:
                if cancelled.wait(0.05) or time.monotonic()-started > timeout:
                    reason = 'cancelled' if cancelled.is_set() else 'timeout'
                    if sys.platform == 'win32':
                        subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], capture_output=True, timeout=15)
                    else:
                        process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=10)
                    break
            log.seek(0, 2)
            length = log.tell()
            log.seek(0 if json_reply and length <= 2*1024*1024 else max(0, length-MAX_LOG))
            output = log.read().decode('utf-8', errors='replace')
        entry = dict(command=command, exit=process.returncode, output=output[-MAX_LOG:], outputTruncated=length > MAX_LOG,
                     elapsedMs=round((time.monotonic()-started)*1000))
        commands.append(entry)
        if reason or process.returncode or (json_reply and length > 2*1024*1024):
            raise AuthorError(reason or 'command failed; inspect compiler/CLI diagnostics')
        return output

    def execute(self, name, arguments, cancelled):
        commands = []
        try:
            spec = next((item for item in TOOLS if item['name'] == name), None)
            if not spec:
                raise AuthorError('unknown tool')
            validate(arguments, spec)
            if cancelled.is_set():
                raise AuthorError('cancelled')
            if name == 'native_sdk_describe':
                examples = sorted(path.name for path in (self.sdk/'examples/native').iterdir() if (path/'CMakeLists.txt').is_file())
                catalog = json.loads(self.catalog.read_text(encoding='utf-8'))
                if 'interface' in arguments:
                    contract = next((item for item in catalog['interfaces'] if item['name'] == arguments['interface']), None)
                    if contract is None:
                        raise AuthorError('unknown SDK interface')
                else:
                    contract = [dict(name=item['name'], version=item['version'], header=item.get('header'),
                                     documentation=item.get('documentation')) for item in catalog['interfaces']]
                value = dict(interfaces=contract, templates=examples,
                             workspace=str(self.workspace), requirements='Windows x64 Visual Studio developer environment, CMake, Ninja, Python 3',
                             installStore=str(self.workspace/'.pulse-author/extensions'))
            elif name == 'native_project_create':
                project_name, template = arguments['name'], arguments['template']
                if (len(project_name) > 64 or not project_name[0].isascii() or not project_name[0].islower() or
                        any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789-' for c in project_name)):
                    raise AuthorError('name must match [a-z][a-z0-9-]{0,63}')
                source = (self.sdk/'examples/native'/template).resolve()
                if source.parent != self.sdk/'examples/native' or not (source/'CMakeLists.txt').is_file():
                    raise AuthorError('template must be one of native_sdk_describe templates')
                destination = self.inside('projects/'+project_name, exists=False)
                if destination.exists():
                    raise AuthorError('project already exists; files were preserved')
                if any(path.is_symlink() for path in source.rglob('*')):
                    raise AuthorError('template contains a symbolic link')
                shutil.copytree(source, destination)
                value = dict(project=str(destination), template=template)
            elif name == 'native_project_build':
                project = self.inside(arguments['project'])
                if not (project/'CMakeLists.txt').is_file():
                    raise AuthorError('project requires CMakeLists.txt')
                build = self.inside(str(project.relative_to(self.workspace))+'/build', exists=False)
                self.run(['cmake', '-S', str(project), '-B', str(build), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                          '-DPULSE_SDK_INCLUDE='+str(self.sdk/'sdk/native/include')], cancelled, commands)
                self.run(['cmake', '--build', str(build), '--parallel', '2'], cancelled, commands)
                value = dict(project=str(project), build=str(build), libraries=[str(p) for p in sorted(build.glob('*.dll'))])
            elif name == 'native_package_stage':
                project, library = self.inside(arguments['project']), self.inside(arguments['library'])
                if not library.is_file() or library.suffix.lower() != '.dll':
                    raise AuthorError('library must be a compiled workspace DLL')
                manifest_template = project/'manifest.json'
                if not manifest_template.is_file():
                    raise AuthorError('project requires manifest.json')
                output = self.inside('packages', exists=False)
                output.mkdir(exist_ok=True)
                # Each stage is distinct so callers cannot accidentally replace a reviewed package.
                stage = Path(tempfile.mkdtemp(prefix=project.name+'-', dir=output))
                staged_library = stage/library.name
                shutil.copyfile(library, staged_library)
                manifest, package = stage/'manifest.json', stage/(project.name+'.pulse-ext')
                extra = []
                names = {library.name.lower(), 'manifest.json', 'package.json'}
                for item in arguments.get('files', []):
                    source = self.inside(item)
                    if not source.is_file() or source.name.lower() in names:
                        raise AuthorError('additional files must be distinct regular workspace files')
                    names.add(source.name.lower())
                    target = stage/source.name
                    shutil.copyfile(source, target)
                    extra.extend(['--file', str(target)])
                self.run(self.cli('native-manifest', manifest_template, '--library', staged_library, '--output', manifest, *extra), cancelled, commands, 40)
                self.run(self.cli('pack', manifest, '--output', package), cancelled, commands, 40)
                verified = json.loads(self.run(self.cli('verify', package), cancelled, commands, 40))
                value = dict(package=str(package), manifest=str(manifest), verified=verified)
            elif name == 'native_package_install':
                if arguments['allowNative'] is not True:
                    raise AuthorError('allowNative=true is required for native test installation')
                package = self.inside(arguments['package'])
                if not package.is_file() or package.suffix != '.pulse-ext':
                    raise AuthorError('package must be a workspace .pulse-ext file')
                store = self.inside('.pulse-author/extensions', exists=False)
                installed = json.loads(self.run(self.cli('install', package, '--store', store, '--allow-native', '--accept-capabilities'), cancelled, commands, 40))
                value = dict(store=str(store), installed=installed, enabled=False, activation='next host startup')
                if arguments.get('enable', False):
                    identity = installed.get('installation', {}).get('package', {}).get('id')
                    if not identity:
                        raise AuthorError('CLI install did not expose package identity; installed disabled')
                    value['enabledResult'] = json.loads(self.run(self.cli('enable', identity, '--store', store, '--allow-native'), cancelled, commands, 40))
                    value['enabled'] = True
            elif name == 'native_host_run':
                if arguments['allowNative'] is not True:
                    raise AuthorError('allowNative=true is required to run installed native code')
                store = self.inside('.pulse-author/extensions')
                profiles = self.inside('profiles', exists=False)
                profiles.mkdir(exist_ok=True)
                profile = profiles/('run-'+str(time.time_ns()))
                value = json.loads(self.run(self.cli('run', '--store', store, '--profile', profile), cancelled, commands))
            else:
                result = json.loads(self.run([str(self.pulse), '--extension-api', '--extension-check'], cancelled, commands, 40, json_reply=True))
                expected = json.loads(self.catalog.read_text(encoding='utf-8'))
                actual = result.get('nativeSdk', {}).get('catalog')
                value = dict(host=str(self.pulse), sdk=str(self.sdk), catalogMatches=actual == expected,
                             diagnosis='matched' if actual == expected else 'host_sdk_mismatch',
                             hostInterfaces=[dict(name=item['name'], version=item['version']) for item in (actual or {}).get('interfaces', [])])
            return dict(ok=True, result=value, commands=commands)
        except (AuthorError, OSError, ValueError, subprocess.SubprocessError) as error:
            return dict(ok=False, error=str(error), commands=commands)


def serve(author, input_stream=sys.stdin.buffer, output_stream=sys.stdout):
    lock = threading.Lock()
    pending = {}
    def send(value):
        with lock:
            output_stream.write(json.dumps(value, ensure_ascii=False)+'\n')
            output_stream.flush()
    def respond(identity, result):
        send(dict(jsonrpc='2.0', id=identity, result=result))
    def complete(identity, name, arguments, event):
        result = author.execute(name, arguments, event)
        respond(identity, dict(content=[dict(type='text', text=json.dumps(result, ensure_ascii=False))],
                               structuredContent=result, isError=not result['ok']))
        with lock:
            pending.pop(identity, None)
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
        while True:
            line = input_stream.readline(MAX_MESSAGE+1)
            if not line:
                break
            try:
                if len(line) > MAX_MESSAGE:
                    raise ValueError('message exceeds 1 MiB')
                request = json.loads(line)
                if not isinstance(request, dict) or request.get('jsonrpc') != '2.0':
                    raise ValueError('invalid JSON-RPC request')
                method, identity = request.get('method'), request.get('id')
                parameters = request.get('params', {})
                if not isinstance(parameters, dict):
                    raise ValueError('params must be an object')
                if method == 'notifications/cancelled':
                    with lock:
                        event = pending.get(parameters.get('requestId'))
                    if event:
                        event.set()
                    continue
                if 'id' not in request:
                    continue
                if type(identity) not in (int, str):
                    raise ValueError('id must be a string or integer')
                if method == 'initialize':
                    requested = parameters.get('protocolVersion')
                    version = requested if requested in ('2025-11-25', '2025-06-18', '2025-03-26', '2024-11-05') else '2025-11-25'
                    respond(identity, dict(protocolVersion=version, capabilities=dict(tools=dict(listChanged=False)),
                        serverInfo=dict(name='pulse-native-author', version='1'),
                        instructions='Author C++ plugins against the public Pulse SDK. Build in an x64 VS environment. Install tools use only the configured author test store.'))
                elif method == 'ping':
                    respond(identity, {})
                elif method == 'tools/list':
                    respond(identity, dict(tools=TOOLS))
                elif method == 'tools/call':
                    with lock:
                        duplicate = identity in pending
                        if not duplicate:
                            event = threading.Event()
                            pending[identity] = event
                    if duplicate:
                        raise ValueError('request id is already pending')
                    pool.submit(complete, identity, parameters.get('name'), parameters.get('arguments', {}), event)
                else:
                    send(dict(jsonrpc='2.0', id=identity, error=dict(code=-32601, message='Method not found')))
            except (ValueError, TypeError, KeyError) as error:
                send(dict(jsonrpc='2.0', id=None, error=dict(code=-32600, message=str(error))))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--sdk-root', type=Path, default=ROOT)
    parser.add_argument('--pulse', type=Path, required=True)
    args = parser.parse_args()
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    serve(Author(args.workspace, args.sdk_root, args.pulse))


if __name__ == '__main__':
    main()
