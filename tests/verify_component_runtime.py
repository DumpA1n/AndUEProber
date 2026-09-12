#!/usr/bin/env python3
"""Execute frozen Android component-package consumers on one owned device."""

import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, default=ROOT / 'tests/component-verification.json')
    parser.add_argument('--device', required=True)
    parser.add_argument('--ndk', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    producer_path = args.manifest.resolve()
    producer = json.loads(producer_path.read_text())
    if producer.get('status') != 'passed' or not producer.get('source_consistent'):
        parser.error('The package manifest must record a successful source-consistent build')
    prefix = Path(producer.get('prefixes', {}).get('android-producer', ''))
    if not prefix.is_absolute() or not prefix.is_dir():
        parser.error('The manifest must identify an available installed Android producer')
    readelf = list((args.ndk / 'toolchains/llvm/prebuilt').glob('*/bin/llvm-readelf'))
    if len(readelf) != 1:
        parser.error('Select an NDK with exactly one host llvm-readelf tool')

    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    archive = args.output.resolve() if args.output else ROOT / 'artifacts/component-runtime' / stamp
    archive.mkdir(parents=True, exist_ok=False)
    build = prefix.parent
    remote = '/data/local/tmp/andueprober-component-' + stamp
    adb = ['adb', '-s', args.device]
    report = {
        'schema_version': 1, 'timestamp_utc': stamp, 'traceability': ['R12', 'R13', 'R21', 'E05', 'E07'],
        'scope': 'Seven Android source/installed package consumer executions against frozen producer artifacts',
        'producer_manifest': {'path': str(producer_path), 'sha256': digest(producer_path)},
        'producer_source_sha256': producer['source_sha256'], 'runner_sha256': digest(Path(__file__)),
        'checks': [], 'artifacts': [], 'runtime': 'not_executed',
        'device': {'serial': args.device, 'owned_directory': remote},
        'ndk_source_properties_sha256': digest(args.ndk / 'source.properties'),
        'unavailable': ['Platform-rendered Inspector, actual UE engine calls and complete reflected SDK',
                        'Other device/API/page-size/hardening configurations',
                        'Source changes after the frozen package build are outside this execution scope'],
    }

    def run(label, command):
        started = time.monotonic()
        command = [str(value) for value in command]
        try:
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, timeout=90)
            code, output = result.returncode, result.stdout
        except subprocess.TimeoutExpired:
            code, output = 124, 'The command exceeded the 90-second observation deadline.\n'
        log = archive / (label + '.log')
        log.write_text(output)
        report['checks'].append({'name': label, 'command': command, 'exit_code': code,
                                 'seconds': round(time.monotonic() - started, 3),
                                 'log': str(log), 'log_sha256': digest(log)})
        if code:
            raise RuntimeError(f'{label} failed with exit {code}; inspect {log}')
        return output

    cases = []
    for name in ['android-source', 'android-installed-core', 'android-installed-full']:
        cases.append((name, [build / name / 'component_consumer', build / name / 'libowned_component.so'],
                      ['component_consumer', 'libowned_component.so', 'owned-output']))
    for name in ['android-inspector-source', 'android-inspector-installed']:
        cases.append((name, [build / name / 'inspector_consumer'], ['inspector_consumer']))
    for name in ['android-agent-source', 'android-agent-installed']:
        library = build / name / 'andueprober/libAndUEProber.so' if name.endswith('source') else prefix / 'lib/libAndUEProber.so'
        cases.append((name, [build / name / 'agent_consumer', library], ['agent_consumer']))

    try:
        # Check every required artifact before creating the owned device directory.
        for _, files, _ in cases:
            for path in files:
                if not path.is_file() or producer['artifact_sha256'].get(str(path)) != digest(path):
                    raise RuntimeError(f'Artifact differs from the frozen producer manifest: {path}')
        for key, prop in [('model', 'ro.product.model'), ('api_level', 'ro.build.version.sdk'),
                          ('fingerprint', 'ro.build.fingerprint')]:
            report['device'][key] = run('device-' + key, adb + ['shell', 'getprop', prop]).strip()
        report['device']['page_size'] = int(run('device-pages', adb + ['shell', 'getconf', 'PAGESIZE']).strip())
        run('owned-directory', adb + ['shell', 'mkdir', '-p', remote])
        for name, files, arguments in cases:
            dest = remote + '/' + name
            run(name + '-directory', adb + ['shell', 'mkdir', '-p', dest])
            for path in files:
                sha = digest(path)
                run(name + '-push-' + path.name, adb + ['push', path, dest + '/' + path.name])
                actual = run(name + '-hash-' + path.name, adb + ['shell', 'sha256sum', dest + '/' + path.name]).split()[0]
                if sha != actual:
                    raise RuntimeError(f'Device artifact digest mismatch: {path}')
                run(name + '-mode-' + path.name, adb + ['shell', 'chmod', '700', dest + '/' + path.name])
                notes = run(name + '-symbols-' + path.name, [readelf[0], '-n', '-S', path])
                build_id = re.search(r'Build ID: (\S+)', notes)
                if not build_id or '.symtab' not in notes:
                    raise RuntimeError(f'Artifact must retain its build ID and symbol table: {path}')
                report['artifacts'].append({'path': str(path), 'sha256': sha, 'device_sha256': actual,
                                             'build_id': build_id.group(1), 'symbols': 'Matching unstripped artifact'})
            output = run(name + '-runtime', adb + ['shell', 'env', 'LD_LIBRARY_PATH=' + dest] +
                         [dest + '/' + value for value in arguments])
            if 'PASS:' not in output:
                raise RuntimeError(f'{name} did not report completed fixture contracts')
            print(name + ': passed', flush=True)
        report['runtime'] = 'passed'
    except Exception as error:
        report['runtime'] = 'failed'
        report['error'] = str(error)
    finally:
        (archive / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'runtime': report['runtime'], 'evidence': str(archive / 'verification.json')}))
    return 0 if report['runtime'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
