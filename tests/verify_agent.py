#!/usr/bin/env python3
"""Execute configured owned native Agent fixtures and archive their exact inputs."""
import argparse
import hashlib
import json
import platform
from pathlib import Path
import re
import shlex
import subprocess
import tarfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--serial', required=True, help='Explicit adb serial for the owned fixture device')
parser.add_argument('--config', action='append', choices=['android-debug', 'android-release', 'android-memory-debug',
    'android-memory-release', 'android-no-adapter-debug', 'android-no-adapter-release'])
parser.add_argument('--host-config', action='append', choices=['host-debug', 'host-release', 'host-sanitizers'])
parser.add_argument('--ndk', required=True, type=Path)
parser.add_argument('--jobs', type=int, default=6)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
configs = args.config or ['android-debug', 'android-release', 'android-memory-debug', 'android-memory-release',
    'android-no-adapter-debug', 'android-no-adapter-release']
host_configs = args.host_config or ['host-debug', 'host-release', 'host-sanitizers']
adb = ['adb', '-s', args.serial]

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def sources():
    paths = [root / 'CMakeLists.txt', root / 'CMakePresets.json', root / 'tests/CMakeLists.txt', Path(__file__),
        root / 'tools/extract_dumper_emitter.py']
    for base in ['include', 'source', 'tests']:
        paths.extend(p for p in (root / base).rglob('*') if p.is_file() and p.suffix in ['.cpp', '.hpp', '.h', '.c'])
    paths.extend(p for p in (root / 'cmake').rglob('*') if p.is_file())
    return {str(p.relative_to(root)): digest(p) for p in sorted(set(paths))}

inputs = sources()
source_id = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
archive = root / 'build/slices' / source_id
archive.mkdir(parents=True, exist_ok=True)
report_path = archive / 'configured-agent-verification.json'
record = {'scope': 'Owned native configured phases, provider admission, immutable Inspector views and generated declared layout headers; no UE engine, full reflected SDK or Android platform renderer',
    'sourceDigest': source_id, 'sourceInputs': inputs, 'checks': [], 'artifacts': {}, 'sourceConsistent': False}
with tarfile.open(archive / 'first-party-source.tar.gz', 'w:gz') as tar:
    for name in inputs:
        tar.add(root / name, arcname=name)
(archive / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')

def save():
    report_path.write_text(json.dumps(record, indent=2) + '\n')

def run(command, kind):
    result = subprocess.run([str(x) for x in command], cwd=root, text=True, capture_output=True)
    record['checks'].append({'kind': kind, 'command': [str(x) for x in command], 'exitCode': result.returncode,
        'output': result.stdout + result.stderr})
    save()
    print(f'{kind}: exit={result.returncode} {shlex.join([str(x) for x in command])}', flush=True)
    if result.returncode:
        print(result.stdout + result.stderr, flush=True)
        raise SystemExit(result.returncode)
    return result.stdout

def artifact(path):
    path = Path(path)
    record['artifacts'][str(path.relative_to(root))] = {'sha256': digest(path), 'bytes': path.stat().st_size}

record['runId'] = str(time.time_ns())
remote = '/data/local/tmp/andueprober-owned-' + source_id[:12] + '-' + record['runId']
record['deviceDirectory'] = remote
run(adb + ['shell', 'mkdir', '-p', remote], 'staging')
for label, command in [('model', ['getprop', 'ro.product.model']), ('api', ['getprop', 'ro.build.version.sdk']),
    ('buildFingerprint', ['getprop', 'ro.build.fingerprint']), ('pageSize', ['getconf', 'PAGESIZE']), ('architecture', ['uname', '-m'])]:
    record.setdefault('device', {})[label] = run(adb + ['shell', *command], 'device-read').strip()
record['device']['serial'] = args.serial
record['device']['scope'] = 'Owned command-line records and normal-linker DSOs; no external application or graphics driver execution'
record['device']['hardening'] = 'Build-cache settings only; separate PAC/BTI/CFI/MTE matrix unverified'
record['dependencies'] = run(['git', 'submodule', 'status'], 'dependency-read')
ndk = args.ndk.resolve()
host_tag = {'Darwin': 'darwin-x86_64', 'Linux': 'linux-x86_64'}.get(platform.system())
if host_tag is None:
    raise SystemExit('This native fixture runner supports macOS and Linux hosts')
compiler = ndk / 'toolchains/llvm/prebuilt' / host_tag / 'bin/aarch64-linux-android27-clang++'
record['androidCompiler'] = run([compiler, '--version'], 'toolchain-read')

cases = {
    'structs': ['ffield', 'uproperty', 'no-alignment', 'corrupt'],
    'classes': ['ffield', 'uproperty', 'corrupt-flags', 'corrupt-default', 'ambiguous'],
    'functions': ['ffield', 'uproperty', 'corrupt-flags', 'corrupt-native', 'ambiguous'],
    'fields': ['ffield', 'reverse', 'corrupt-owner', 'corrupt-name', 'ambiguous'],
    'properties': ['ffield', 'corrupt-size', 'corrupt-offset', 'corrupt-flags', 'ambiguous'],
    'object_flags': ['ffield', 'uproperty', 'zero', 'corrupt', 'ambiguous'],
    'enums': ['ffield', 'uproperty', 'reordered', 'corrupt-value', 'corrupt-name', 'corrupt-count', 'ambiguous'],
    'property_tails': ['enum', 'array', 'set', 'map', 'object', 'struct', 'byte', 'class', 'interface', 'corrupt-first', 'corrupt-second', 'ambiguous', 'prefix'],
    'property_values': ['bool', 'path', 'corrupt-bool', 'corrupt-path', 'ambiguous-bool', 'ambiguous-path', 'prefix-bool', 'prefix-path', 'native-only', 'bit-only'],
    'layout': ['ffield', 'uproperty', 'invalid-layout', 'corrupt-flags'],
}
for config in configs:
    build = root / 'build' / config
    if not (build / 'CMakeCache.txt').is_file():
        raise SystemExit(f'Configure the explicit build first: {build}')
    no_adapter = 'no-adapter' in config
    memory = 'memory' in config or no_adapter
    cache = {}
    for line in (build / 'CMakeCache.txt').read_text().splitlines():
        if line.startswith(('#', '//')) or '=' not in line or ':' not in line.split('=', 1)[0]:
            continue
        key, value = line.split('=', 1)
        cache[key.split(':', 1)[0]] = value
    actual_memory = bool(cache.get('ANDUEPROBER_MEMORY_SOURCE_DIR')) or cache.get('ANDUEPROBER_WITH_PROCESS_MEMORY') == 'ON'
    if actual_memory != memory or (cache.get('ANDUEPROBER_BUILD_DUMPER_ADAPTER') == 'OFF') != no_adapter:
        raise SystemExit('Configured provider options do not match the named validation scope: ' + config)
    names = ['andueprober_android_load']
    if no_adapter:
        names += ['andueprober_android_layout', 'andueprober_android_property_values']
    else:
        names += ['andueprober_android_' + name for name in cases]
        names += ['andueprober_android_commands', 'andueprober_android_index']
        if memory:
            names += ['andueprober_android_inspector']
    build_targets = ['AndUEProber', *names]
    if memory:
        build_targets += ['andueprober_owned_module']
    run(['cmake', '--build', build, '--target', *build_targets, '-j', args.jobs], 'android-build')
    target = remote + '/' + config
    run(adb + ['shell', 'mkdir', '-p', target], 'staging')
    def push(path):
        artifact(path)
        destination = target + '/' + path.name
        run(adb + ['push', path, destination], 'push')
        if run(adb + ['shell', 'sha256sum', destination], 'artifact-check').split()[0] != digest(path):
            raise SystemExit('Device artifact hash mismatch')
        run(adb + ['shell', 'chmod', '700', destination], 'staging')
        return destination
    agent = push(build / 'libAndUEProber.so')
    for name in names:
        push(build / 'tests' / name)
    # Missing-Memory admission must reject before inspecting the module path or address.
    module = push(build / 'tests/libandueprober_owned_module.so') if memory else None
    if module is None:
        module_build = root / 'build/android-memory-debug'
        run(['cmake', '--build', module_build, '--target', 'andueprober_owned_module', '-j', args.jobs], 'android-build')
        module = push(module_build / 'tests/libandueprober_owned_module.so')
    def native(program, arguments):
        command = 'cd ' + shlex.quote(target) + ' && LD_LIBRARY_PATH=. ' + shlex.join([target + '/' + program, *arguments])
        return run(adb + ['shell', command], 'android-execution')
    native('andueprober_android_load', [agent])
    if no_adapter:
        native('andueprober_android_layout', [agent, module, 'no-adapter', target + '/output'])
        for mode in cases['property_values']:
            native('andueprober_android_property_values', [agent, module, mode, target + '/property-values-output'])
    else:
        for name, modes in cases.items():
            for mode in modes if memory else ['missing']:
                output = native('andueprober_android_' + name, [agent, module, mode, target + '/' + name + '-output'])
                if name == 'layout' and memory and mode in ['ffield', 'uproperty']:
                    sdk = re.search(r'SDK_PATH=(.+)', output).group(1).strip()
                    generated = archive / 'generated' / config / mode
                    generated.mkdir(parents=True, exist_ok=True)
                    run(adb + ['pull', sdk, generated / 'FunctionLayout.hpp'], 'pull-generated')
                    includes = ['-I' + str(p) for p in [generated, root / 'tests', root / 'include']]
                    run(['clang++', '-std=c++20', *includes, root / 'tests/layout_smoke.cpp', '-o', generated / 'host-smoke'], 'host-sdk-compile')
                    run([generated / 'host-smoke'], 'host-execution')
                    run([compiler, '-std=c++20', '-static-libstdc++', *includes, root / 'tests/layout_smoke.cpp', '-o', generated / 'android-smoke'], 'android-sdk-compile')
                    executable = push(generated / 'android-smoke')
                    run(adb + ['shell', executable], 'android-execution')
                    for path in generated.iterdir():
                        artifact(path)
        native('andueprober_android_commands', [agent, 'enabled' if memory else 'missing'])
        for mode in ['flat', 'chunked', 'corrupt'] if memory else ['missing']:
            native('andueprober_android_index', [agent, mode, target + '/index-output'])
        if memory:
            for mode in ['interactive', 'configured']:
                native('andueprober_android_inspector', [module, mode, target + '/inspector-output'])
    for path in [build / 'CMakeCache.txt', build / 'generated/dumper-emitter/provenance.json']:
        if path.is_file():
            destination = archive / 'build-records' / config / path.relative_to(build)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(path.read_bytes())
            artifact(destination)
for config in host_configs:
    build = root / 'build' / config
    targets = ['andueprober_inspector', 'andueprober_names', 'andueprober_relations']
    run(['cmake', '--build', build, '--target', *targets, '-j', args.jobs], 'host-build')
    for target in targets:
        executable = build / 'tests' / target
        artifact(executable)
        run([executable], 'host-execution')
record['sourceConsistent'] = sources() == inputs
record['counts'] = {kind: sum(c['kind'] == kind for c in record['checks']) for kind in sorted({c['kind'] for c in record['checks']})}
save()
if not record['sourceConsistent']:
    raise SystemExit('Source inputs changed; this record does not validate the final source state')
print('PASS: ' + str(report_path), flush=True)
