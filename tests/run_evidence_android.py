#!/usr/bin/env python3
"""Execute previously verified owned evidence fixtures on an explicit Android device."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--manifest', type=Path, default=ROOT / 'tests/evidence/verification.json')
    parser.add_argument('--ndk', type=Path, help='NDK used by the producer; defaults to its Android CMake cache')
    args = parser.parse_args()
    report = json.loads(args.manifest.read_text())
    if report['status'] != 'passed' or not report['source_consistent']:
        raise RuntimeError('A successful source-consistent build gate is required')
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    logs = ROOT / 'artifacts/evidence-runtime' / stamp
    logs.mkdir(parents=True)
    build = Path(report['prefixes']['host-release']).parent
    cache = (build / 'android-debug/CMakeCache.txt').read_text()
    toolchain = re.search(r'^CMAKE_TOOLCHAIN_FILE:[^=]+=(.+)$', cache, re.MULTILINE)
    if args.ndk is None and not toolchain:
        raise RuntimeError('The Android producer cache must identify an NDK toolchain, or --ndk is required')
    ndk = args.ndk.resolve() if args.ndk else Path(toolchain.group(1)).resolve().parents[2]
    if sha(ndk / 'source.properties') != report['ndk_source_properties_sha256']:
        raise RuntimeError('NDK revision differs from the completed build gate')
    readelf_candidates = list((ndk / 'toolchains/llvm/prebuilt').glob('*/bin/llvm-readelf'))
    if len(readelf_candidates) != 1:
        raise RuntimeError('Exactly one NDK host llvm-readelf is required')
    readelf = readelf_candidates[0]
    ndk_revision = re.search(r'^Pkg.Revision\s*=\s*(.+)$', (ndk / 'source.properties').read_text(), re.MULTILINE).group(1)
    remote = '/data/local/tmp/andueprober-evidence-' + stamp
    expected = {'evidence': 50, 'evidence_phases': 139, 'structs': 319, 'classes': 382, 'functions': 865, 'object_flags': 191}
    runtime = {'status': 'running', 'serial': args.serial, 'owned_directory': remote,
               'script': str(Path(__file__).resolve()), 'script_sha256': sha(Path(__file__).resolve()),
               'checks': [], 'fixture_checks': expected, 'source_consistent': False,
               'artifacts': {}, 'source_before_sha256': {}, 'source_after_sha256': {},
               'ndk_source_properties_sha256': sha(ndk / 'source.properties'),
               'readelf': str(readelf), 'readelf_sha256': sha(readelf)}
    report['android_runtime'] = runtime
    adb = ['adb', '-s', args.serial]

    def run(name, command):
        command = [str(value) for value in command]
        log = logs / (name + '.log')
        result = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log.write_text(json.dumps(command) + '\n' + result.stdout)
        runtime['checks'].append({'name': name, 'command': command, 'exit_code': result.returncode,
                                  'passed': result.returncode == 0, 'log': str(log), 'log_sha256': sha(log)})
        print(name + ': ' + str(result.returncode), flush=True)
        if result.returncode:
            raise RuntimeError(f'{name} failed; inspect {log}')
        return result.stdout

    def unchanged():
        return all((ROOT / name).is_file() and sha(ROOT / name) == value for name, value in report['source_sha256'].items())

    def current_sources():
        return {name: sha(ROOT / name) for name in report['source_sha256']}

    def verified(path):
        path = Path(path)
        digest = report['artifact_sha256'].get(str(path))
        if not digest or sha(path) != digest:
            raise RuntimeError(f'Artifact differs from the completed build gate: {path}')
        return digest

    def stage(name, path, destination):
        digest = verified(path)
        run(name + '-push', [*adb, 'push', path, destination])
        content = run(name + '-device-sha256', [*adb, 'shell', 'sha256sum', destination])
        if content.split()[0] != digest:
            raise RuntimeError('Device artifact digest differs from the producer artifact')
        runtime['artifacts'][str(path)].update(device_path=destination, device_sha256=content.split()[0])

    def identify(name, path, android):
        record = {'path': str(path), 'sha256': verified(path), 'bytes': path.stat().st_size,
                  'symbols': 'matching unstripped artifact'}
        if android:
            content = run(name + '-symbols', [readelf, '-n', '-S', path])
            build_id = re.search(r'Build ID:\s*(\w+)', content)
            if not build_id or '.symtab' not in content:
                raise RuntimeError('Android artifact must retain build ID and matching symbols')
            record['build_id'] = build_id.group(1)
        else:
            content = run(name + '-uuid', ['dwarfdump', '--uuid', path])
            uuid = re.search(r'UUID:\s*([0-9A-F-]+)', content)
            if not uuid:
                raise RuntimeError('Host artifact UUID is absent')
            record['uuid'] = uuid.group(1)
            content = run(name + '-symbols', ['nm', path])
            if ' T ' not in content and ' t ' not in content:
                raise RuntimeError('Host artifact does not retain function symbols')
        runtime['artifacts'][str(path)] = record

    try:
        if not unchanged():
            raise RuntimeError('Current selected source differs from the completed build gate')
        runtime['source_before_sha256'] = current_sources()
        runtime['model'] = run('device-model', [*adb, 'shell', 'getprop', 'ro.product.model']).strip()
        runtime['api_level'] = run('device-api', [*adb, 'shell', 'getprop', 'ro.build.version.sdk']).strip()
        runtime['android_version'] = run('device-version', [*adb, 'shell', 'getprop', 'ro.build.version.release']).strip()
        runtime['abi'] = run('device-abi', [*adb, 'shell', 'getprop', 'ro.product.cpu.abi']).strip()
        runtime['page_size'] = run('device-page-size', [*adb, 'shell', 'getconf', 'PAGE_SIZE']).strip()
        runtime['fingerprint'] = run('device-fingerprint', [*adb, 'shell', 'getprop', 'ro.build.fingerprint']).strip()
        for config in ('host-debug', 'host-release', 'host-sanitized', 'android-debug', 'android-release'):
            for fixture in expected:
                identify(config + '-' + fixture, build / config / 'tests' / ('andueprober_' + fixture),
                         config.startswith('android'))
        run('owned-directory', [*adb, 'shell', 'mkdir', '-p', remote])
        for config in ('android-debug', 'android-release'):
            for fixture, count in expected.items():
                name = config + '-' + fixture
                source = build / config / 'tests' / ('andueprober_' + fixture)
                destination = remote + '/' + name
                stage(name, source, destination)
                run(name + '-chmod', [*adb, 'shell', 'chmod', '700', destination])
                content = run(name, [*adb, 'shell', destination])
                if not re.search(r'PASS: ' + str(count) + r'\b', content):
                    raise RuntimeError(f'{name} did not report its required {count} checks')
        for config in ('android-source', 'android-installed'):
            for filename in ('component_consumer', 'libowned_component.so'):
                name = config + '-' + filename
                identify(name, build / config / filename, True)
                stage(name, build / config / filename, remote + '/' + name)
            run(config + '-chmod', [*adb, 'shell', 'chmod', '700', remote + '/' + config + '-component_consumer'])
            run(config + '-consumer', [*adb, 'shell', remote + '/' + config + '-component_consumer',
                                      remote + '/' + config + '-libowned_component.so', remote + '/' + config + '-output'])
        runtime['source_after_sha256'] = current_sources()
        runtime['source_consistent'] = unchanged() and runtime['source_before_sha256'] == runtime['source_after_sha256']
        if not runtime['source_consistent']:
            raise RuntimeError('Selected source changed during Android execution')
        runtime['status'] = 'passed'
        for phase in ('structs', 'classes', 'functions', 'object_flags'):
            manifest = ROOT / 'tests' / phase / 'verification.json'
            prior = json.loads(manifest.read_text())
            archive = logs / ('prior-' + phase)
            shutil.copytree(manifest.parent, archive)
            prior['source_sha256'] = report['source_sha256']
            prior['source_after_sha256'] = runtime['source_after_sha256']
            prior['source_consistent'] = True
            prior['source_scope'] = report['source_scope']
            prior['host'] = {'platform': platform.platform(), 'architecture': platform.machine()}
            prior['android'] = {key: runtime[key] for key in ('serial', 'owned_directory', 'model', 'api_level', 'android_version', 'abi', 'page_size', 'fingerprint')}
            prior['android'].update({'ndk': ndk_revision, 'native_api_target': 27, 'stl': 'c++_static',
                                    'hardening': 'NDK defaults; no separate BTI/PAC/CFI/MTE options or validation; owned memory only'})
            prior['configurations'] = []
            evidence = manifest.parent / 'evidence'
            if evidence.exists():
                shutil.rmtree(evidence)
            evidence.mkdir()
            for config in ('host-debug', 'host-release', 'host-sanitized', 'android-debug', 'android-release'):
                executable = build / config / 'tests' / ('andueprober_' + phase)
                verified(executable)
                compile_log = next(check['log'] for check in report['checks'] if check['name'] == config + '-build')
                if config.startswith('host'):
                    execution_log = next(check['log'] for check in report['checks'] if check['name'] == config + '-ctest')
                else:
                    execution_log = next(check['log'] for check in runtime['checks'] if check['name'] == config + '-' + phase)
                output = Path(execution_log).read_text()
                if not re.search(r'PASS: ' + str(expected[phase]) + r'\b', output):
                    raise RuntimeError(f'{config} {phase} check count is absent')
                copied_log = evidence / (config + '.log')
                copied_build = evidence / (config + '-build.log')
                shutil.copyfile(execution_log, copied_log)
                shutil.copyfile(compile_log, copied_build)
                prior['configurations'].append({'name': config, 'build_type': 'Release' if config.endswith('release') else 'Debug',
                    'compiled': True, 'runtime': 'passed', 'checks': expected[phase], 'production_boundary_replacement': False,
                    'sanitizers': 'ASan and UBSan; no macOS LeakSanitizer claim' if config == 'host-sanitized' else 'none',
                    'executable': runtime['artifacts'][str(executable)],
                    'runtime_log': str(copied_log.relative_to(ROOT)), 'runtime_log_sha256': sha(copied_log),
                    'build_log': str(copied_build.relative_to(ROOT)), 'build_log_sha256': sha(copied_build)})
            prior['shared_closure_evidence'] = {'path': 'tests/evidence/verification.json', 'helper_checks': 50,
                'phase_closure_checks': 139, 'allocation_failure_positions': 12, 'build_gate_timestamp': report['timestamp_utc'],
                'build_archive': str(ROOT / 'artifacts/evidence-validation' / report['timestamp_utc'] / 'verification.json'),
                'runtime_archive': str(logs / 'verification.json')}
            prior.pop('limitations', None)
            prior['limitations'] = ['Owned native fixtures; no external UE process or engine function execution.',
                'The runtime checks only the selected phase and its prerequisites; other archive members are supporting build artifacts.']
            manifest.write_text(json.dumps(prior, indent=2) + '\n')
    except Exception as error:
        runtime['status'] = 'failed'
        runtime['error'] = str(error)
    finally:
        (logs / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
        args.manifest.write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({'status': runtime['status'], 'source_consistent': runtime['source_consistent'], 'evidence': str(logs / 'verification.json')}))
    return 0 if runtime['status'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
