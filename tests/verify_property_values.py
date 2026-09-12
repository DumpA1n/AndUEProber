#!/usr/bin/env python3
"""Build and execute owned property metadata providers and their shared-context regressions."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--serial', required=True, help='Explicit owned-fixture Android device serial')
parser.add_argument('--ndk', type=Path, required=True)
parser.add_argument('--jobs', type=int, default=3)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
stamp = time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())
archive = root / 'build/property-values-validation' / stamp
archive.mkdir(parents=True, exist_ok=True)
programs = ['andueprober_properties', 'andueprober_property_tails', 'andueprober_property_values']
source_paths = [Path(__file__), root / 'CMakeLists.txt', root / 'tests/CMakeLists.txt']
source_paths += list((root / 'include/andueprober').glob('*'))
for base in ['source/Core', 'source/Probe', 'cmake']:
    source_paths += [p for p in (root / base).rglob('*') if p.is_file()]
source_paths += list((root / 'tests').glob('Owned*.hpp'))
source_paths += [root / 'tests' / (name + '.cpp') for name in ['properties', 'property_tails', 'property_values']]
source_paths = sorted(set(p for p in source_paths if p.is_file()))

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def source_hashes():
    return {str(p.relative_to(root)): digest(p) for p in source_paths}

inputs = source_hashes()
record = {'format_version': 1, 'date': time.strftime('%Y-%m-%d'), 'traceability': ['R09', 'R13', 'R18', 'C04', 'E05'],
    'scope': 'Owned declared Bool/inline-FName FieldPath providers and shared-context FProperty/opaque-pointer regressions; no UE engine or complete reflected SDK',
    'source_state': 'Modified working tree; selected source hashes and complete configured build records',
    'source_sha256': inputs, 'source_consistent': False, 'commands': [], 'artifacts': {},
    'host': {'platform': platform.platform(), 'architecture': platform.machine()},
    'android': {'ndk': str(args.ndk.resolve()), 'abi': 'arm64-v8a', 'api_target': 27, 'stl': 'c++_static'},
    'unverified': ['Actual UE metadata layouts and engine integration', 'Other page sizes, API levels and hardening configurations',
        'LSan on this macOS host; ASan/UBSan do not establish leak-freedom']}
report = archive / 'verification.json'

def save():
    report.write_text(json.dumps(record, indent=2) + '\n')

def artifact(path):
    path = Path(path)
    record['artifacts'][str(path.relative_to(root))] = {'sha256': digest(path), 'bytes': path.stat().st_size}

def run(command, kind, config=''):
    command = [str(x) for x in command]
    env = dict(os.environ)
    if platform.system() == 'Darwin':
        env['ASAN_OPTIONS'] = 'detect_leaks=0'
    result = subprocess.run(command, cwd=root, capture_output=True, text=True, env=env)
    log = archive / f'{len(record["commands"]):03d}-{kind}-{config}.log'
    log.write_text(result.stdout + result.stderr)
    artifact(log)
    record['commands'].append({'kind': kind, 'config': config, 'command': command, 'exit_code': result.returncode,
        'output': result.stdout + result.stderr, 'log': str(log.relative_to(root))})
    save()
    print(f'{kind} {config}: exit={result.returncode}', flush=True)
    if result.returncode:
        print(result.stdout + result.stderr, flush=True)
        raise SystemExit(result.returncode)
    return result.stdout

adb = ['adb', '-s', args.serial]
record['android']['serial'] = args.serial
for name, command in [('model', ['getprop', 'ro.product.model']), ('api', ['getprop', 'ro.build.version.sdk']),
    ('fingerprint', ['getprop', 'ro.build.fingerprint']), ('page_size', ['getconf', 'PAGESIZE']), ('architecture', ['uname', '-m'])]:
    record['android'][name] = run(adb + ['shell', *command], 'device-read').strip()
run(['cmake', '--version'], 'toolchain')
run(['clang++', '--version'], 'toolchain')
prebuilt = args.ndk.resolve() / 'toolchains/llvm/prebuilt' / ('darwin-x86_64' if platform.system() == 'Darwin' else 'linux-x86_64') / 'bin'
run([prebuilt / 'clang++', '--version'], 'toolchain')
remote = '/data/local/tmp/andueprober-property-values-' + stamp
record['android']['fixture_directory'] = remote
for config in ['host-debug', 'host-release', 'host-sanitizers', 'android-debug', 'android-release']:
    build = archive / config
    options = ['-DANDUEPROBER_BUILD_AGENT=OFF', '-DANDUEPROBER_BUILD_TESTS=ON', '-DANDUEPROBER_BUILD_INSPECTOR=OFF',
        '-DANDUEPROBER_BUILD_DUMPER_EMITTER=OFF', '-DANDUEPROBER_BUILD_DUMPER_ADAPTER=OFF',
        '-DCMAKE_BUILD_TYPE=' + ('Release' if config.endswith('release') else 'Debug'),
        '-DANDUEPROBER_SANITIZERS=' + ('ON' if config == 'host-sanitizers' else 'OFF')]
    android = config.startswith('android')
    if android:
        options += ['-DCMAKE_TOOLCHAIN_FILE=' + str(args.ndk.resolve() / 'build/cmake/android.toolchain.cmake'),
            '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-27', '-DANDROID_STL=c++_static']
    run(['cmake', '-S', root, '-B', build, '-G', 'Ninja', *options], 'configure', config)
    run(['cmake', '--build', build, '--target', *programs, 'header_PropertyValues_hpp', 'header_Agent_h', '-j', args.jobs], 'build', config)
    artifact(build / 'CMakeCache.txt')
    if android:
        run(adb + ['shell', 'mkdir', '-p', remote + '/' + config], 'staging', config)
    for program in programs:
        binary = build / 'tests' / program
        artifact(binary)
        if android:
            run([prebuilt / 'llvm-readelf', '-n', binary], 'symbols', config)
            target = remote + '/' + config + '/' + program
            run(adb + ['push', binary, target], 'push', config)
            actual = run(adb + ['shell', 'sha256sum', target], 'artifact-check', config).split()[0]
            if actual != digest(binary):
                raise SystemExit('Device artifact hash mismatch')
            run(adb + ['shell', 'chmod', '700', target], 'staging', config)
            run(adb + ['shell', target], 'android-execution', config)
        else:
            if platform.system() == 'Darwin':
                run(['dwarfdump', '--uuid', binary], 'symbols', config)
            run([binary], 'host-execution', config)
record['source_consistent'] = inputs == source_hashes()
record['counts'] = {kind: sum(row['kind'] == kind for row in record['commands']) for kind in sorted({row['kind'] for row in record['commands']})}
save()
if not record['source_consistent']:
    raise SystemExit('Source inputs changed; this archive is not evidence for the final source state')
for name in ['properties', 'property_tails', 'property_values']:
    destination = root / 'tests' / name / 'verification.json'
    destination.parent.mkdir(parents=True, exist_ok=True)
    selected = dict(record)
    selected['fixture'] = 'andueprober_' + name
    selected['scope'] = {'properties': 'Four FProperty scalar offsets and exact FField evidence; shared-context regression',
        'property_tails': 'Eleven opaque pointer offsets across nine kinds through shared PropertyContext; excludes Bool, FieldPath and complete containers',
        'property_values': 'Four independently scanned Bool uint8 offsets and explicitly declared inline-FName FieldPath; no engine or complete UE layout claim'}[name]
    selected['evidence_archive'] = str(report.relative_to(root))
    destination.write_text(json.dumps(selected, indent=2) + '\n')
print('PASS: ' + str(report), flush=True)
