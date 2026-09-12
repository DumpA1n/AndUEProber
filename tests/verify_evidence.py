#!/usr/bin/env python3
"""Verify bounded evidence closures, phase regressions and public component consumers."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TARGETS = ['andueprober_' + name for name in
           ('evidence', 'evidence_phases', 'structs', 'classes', 'functions', 'object_flags')]
SELECTOR = r'^andueprober\.(evidence|evidence_phases|structs|classes|functions|object_flags)$'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sources():
    paths = {ROOT / 'CMakeLists.txt', ROOT / 'tests/CMakeLists.txt', Path(__file__).resolve()}
    for directory in ('cmake', 'source/Core', 'tests/component_consumer'):
        paths.update(path for path in (ROOT / directory).rglob('*') if path.is_file() and
                     path.suffix in {'.cpp', '.hpp', '.h', '.cmake', '.txt', '.in'})
    for name in ('Probe', 'Names', 'Relations', 'Structs', 'Classes', 'Functions', 'ObjectFlags'):
        paths.add(ROOT / 'source/Probe' / (name + '.cpp'))
    for name in ('evidence', 'evidence_phases', 'structs', 'classes', 'functions', 'object_flags'):
        paths.add(ROOT / 'tests' / (name + '.cpp'))
    pending = list(paths)
    while pending:
        path = pending.pop()
        if path.suffix not in {'.cpp', '.hpp', '.h'}:
            continue
        for name in re.findall(r'#include\s*[<"](?:andueprober/)?([^>"\n]+)[>"]', path.read_text()):
            for directory in (ROOT / 'include/andueprober', path.parent):
                header = directory / name
                if header.is_file() and header not in paths:
                    paths.add(header)
                    pending.append(header)
                    break
    return {str(path.relative_to(ROOT)): sha(path) for path in sorted(paths)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', type=Path)
    args = parser.parse_args()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    build = ROOT / 'build/evidence-validation' / stamp
    logs = ROOT / 'artifacts/evidence-validation' / stamp
    build.mkdir(parents=True)
    logs.mkdir(parents=True)
    report = {'schema_version': 1, 'timestamp_utc': stamp,
              'scope': 'Bounded evidence closure, selected real phase regressions and static components in a caller DSO',
              'source_scope': 'Core and selected Probe implementation, recursively included public/fixture headers, CMake graph and component consumer. Other Probe archive members and installed optional headers are supporting build artifacts, not runtime-validated by this gate.',
              'source_sha256': sources(), 'checks': [], 'artifact_sha256': {}, 'prefixes': {},
              'android_runtime': 'not executed by this runner'}

    def run(name, command):
        command = [str(value) for value in command]
        log = logs / (name + '.log')
        print(name, flush=True)
        with log.open('w') as output:
            output.write(json.dumps(command) + '\n')
            output.flush()
            result = subprocess.run(command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT)
        report['checks'].append({'name': name, 'command': command, 'exit_code': result.returncode,
                                'passed': result.returncode == 0, 'log': str(log), 'log_sha256': sha(log)})
        if result.returncode:
            raise RuntimeError(f'{name} failed; inspect {log}')

    def configure(name, source, flags):
        run(name + '-configure', ['cmake', '-S', source, '-B', build / name, '-G', 'Ninja',
                                 '-DCMAKE_BUILD_TYPE=Release', *flags])

    def compile(name, targets=()):
        run(name + '-build', ['cmake', '--build', build / name, '--parallel', '4', '--verbose',
                             *(['--target', *targets] if targets else [])])

    def execute(name, selector=None):
        run(name + '-ctest', ['ctest', '--test-dir', build / name, '--no-tests=error', '--output-on-failure', '-V',
                             *(['-R', selector] if selector else [])])

    def install(name):
        original = build / (name + '-install')
        relocated = build / (name + '-relocated')
        run(name + '-install', ['cmake', '--install', build / name, '--prefix', original])
        original.rename(relocated)
        report['prefixes'][name] = str(relocated)
        return relocated

    producer = ['-DANDUEPROBER_BUILD_AGENT=OFF', '-DANDUEPROBER_BUILD_INSPECTOR=OFF',
                '-DANDUEPROBER_BUILD_DUMPER_ADAPTER=OFF', '-DANDUEPROBER_WITH_PROCESS_MEMORY=OFF',
                '-DANDUEPROBER_BUILD_TESTS=ON', '-DCMAKE_INSTALL_INCLUDEDIR=include/custom']
    consumer = ROOT / 'tests/component_consumer'
    isolated = ['-DCMAKE_DISABLE_FIND_PACKAGE_AndSwapChainHook=ON', '-DCMAKE_DISABLE_FIND_PACKAGE_AndUEProberImGui=ON']
    try:
        for name, extra in (('host-debug', ['-DCMAKE_BUILD_TYPE=Debug']), ('host-release', []),
                            ('host-sanitized', ['-DCMAKE_BUILD_TYPE=Debug', '-DANDUEPROBER_SANITIZERS=ON'])):
            configure(name, ROOT, [*producer, *extra])
            compile(name, [*TARGETS, 'header_Evidence_hpp'])
            execute(name, SELECTOR)
        host = install('host-release')
        for name, flags in (('host-source', [f'-DANDUEPROBER_SOURCE={ROOT}']),
                            ('host-installed', [f'-DCMAKE_PREFIX_PATH={host}'])):
            configure(name, consumer, [*flags, *isolated])
            compile(name)
            execute(name)
        if args.ndk:
            ndk = args.ndk.resolve()
            report['ndk_source_properties_sha256'] = sha(ndk / 'source.properties')
            cross = [f'-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake', '-DANDROID_ABI=arm64-v8a',
                     '-DANDROID_PLATFORM=android-27', '-DANDROID_STL=c++_static']
            for name, mode in (('android-debug', 'Debug'), ('android-release', 'Release')):
                configure(name, ROOT, [*producer, *cross, f'-DCMAKE_BUILD_TYPE={mode}'])
                compile(name, [*TARGETS, 'header_Evidence_hpp'])
            android = install('android-release')
            for name, flags in (('android-source', [f'-DANDUEPROBER_SOURCE={ROOT}']),
                                ('android-installed', [f'-DCMAKE_PREFIX_PATH={android}', f'-DCMAKE_FIND_ROOT_PATH={android}'])):
                configure(name, consumer, [*flags, *cross, *isolated])
                compile(name)
        report['status'] = 'passed'
    except Exception as error:
        report['status'] = 'failed'
        report['error'] = str(error)
    finally:
        report['source_consistent'] = report['source_sha256'] == sources()
        if not report['source_consistent']:
            report['source_after_sha256'] = sources()
            report['status'] = 'failed'
        for path in sorted(build.rglob('*')):
            if path.is_file() and (path.suffix in {'.a', '.so', '.dylib', '.cmake', '.hpp', '.json'} or
                                  path.name in {*TARGETS, 'component_consumer', 'CMakeCache.txt', 'LICENSE', 'THIRD_PARTY_NOTICES.md'}):
                report['artifact_sha256'][str(path)] = sha(path)
        (logs / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
        destination = ROOT / 'tests/evidence/verification.json'
        destination.parent.mkdir(exist_ok=True)
        destination.write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({'status': report['status'], 'source_consistent': report['source_consistent'],
                          'checks': len(report['checks']), 'evidence': str(logs / 'verification.json')}))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
