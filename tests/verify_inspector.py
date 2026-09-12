#!/usr/bin/env python3
"""Verify Inspector package boundaries using owned CPU consumers."""
import argparse
import datetime
import hashlib
import json
import re
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sources():
    paths = {ROOT / "CMakeLists.txt", ROOT / "tests/CMakeLists.txt", Path(__file__).resolve()}
    for pattern in ("cmake/*", "source/Core/*.cpp", "source/UI/*", "tests/inspector.cpp",
                    "tests/inspector_consumer/*", "tests/consumer/*"):
        paths.update(path for path in ROOT.glob(pattern) if path.is_file())
    paths.update(ROOT / "source/Probe" / (name + ".cpp") for name in ("Probe", "Names", "Relations"))
    pending = list(paths)
    while pending:
        path = pending.pop()
        if path.suffix not in {".cpp", ".hpp", ".h"}:
            continue
        for name in re.findall(r'#include\s*[<"](?:andueprober/)?([^>"\n]+)[>"]', path.read_text()):
            if name == "Agent.h":
                continue
            for directory in (ROOT / "include/andueprober", path.parent):
                header = directory / name
                if header.is_file() and header not in paths:
                    paths.add(header)
                    pending.append(header)
                    break
    return {str(path.relative_to(ROOT)): sha256(path) for path in sorted(paths)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ndk", type=Path)
    parser.add_argument("--jobs", type=int, default=6)
    args = parser.parse_args()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    build = ROOT / "build/inspector-validation" / stamp
    logs = ROOT / "artifacts/inspector-validation" / stamp
    logs.mkdir(parents=True)
    prior = ROOT / "tests/inspector-verification.json"
    if prior.is_file():
        (logs / "prior-verification.json").write_bytes(prior.read_bytes())
    report = {"status": "running", "timestamp_utc": stamp, "source_files": sources(),
              "checks": [], "prefixes": {}, "artifacts": {}, "android_runtime": "not_executed",
              "source_scope": "Core/UI implementation, actual Phase 1 owned fixture and independent consumers with recursively included API headers. Other Probe archive members and unused Agent C header declarations are supporting package artifacts; their installed bytes are recorded without a runtime claim."}

    def run(label, command, expected=0):
        log = logs / f"{len(report['checks']) + 1:02d}-{label}.log"
        completed = subprocess.run([str(value) for value in command], cwd=ROOT, text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log.write_text(completed.stdout)
        passed = completed.returncode == 0 if expected == 0 else completed.returncode != 0
        report["checks"].append({"label": label, "command": [str(value) for value in command],
                                  "exit_code": completed.returncode, "expected": expected,
                                  "passed": passed, "log": str(log.relative_to(ROOT)), "sha256": sha256(log)})
        print(f"{'PASS' if passed else 'FAIL'} {label}", flush=True)
        if not passed:
            raise RuntimeError(f"{label}: {log}")
        return log

    def configure(label, source, directory, options):
        run(label, ["cmake", "-S", source, "-B", directory, "-G", "Ninja", *options])

    def compile_and_run(label, directory, execute=True):
        run(label + "-build", ["cmake", "--build", directory, "-j", args.jobs])
        if execute:
            run(label + "-ctest", ["ctest", "--test-dir", directory, "--output-on-failure", "-V"])

    def install(label, directory):
        original, relocated = build / (label + "-install"), build / (label + "-relocated")
        run(label + "-install", ["cmake", "--install", directory, "--prefix", original])
        original.rename(relocated)
        report["prefixes"][label] = str(relocated)
        for package_file in relocated.rglob("*.cmake"):
            if str(ROOT) in package_file.read_text():
                raise RuntimeError(f"Source/install path leaked: {package_file}")
        return relocated

    def consumer(label, prefix=None, source=False, extra=(), execute=True):
        directory = build / label
        options = ["-DCMAKE_BUILD_TYPE=Release", *extra]
        options.append(f"-DANDUEPROBER_SOURCE={ROOT}" if source else f"-DCMAKE_PREFIX_PATH={prefix}")
        configure(label + "-configure", ROOT / "tests/inspector_consumer", directory, options)
        compile_and_run(label, directory, execute)
        return directory

    try:
        prefixes = {}
        for name, includedir in (("default", "include"), ("custom-include", "include/custom")):
            directory = build / (name + "-producer")
            configure(name + "-configure", ROOT, directory,
                      ["-DCMAKE_BUILD_TYPE=Release", "-DANDUEPROBER_BUILD_AGENT=OFF",
                       "-DANDUEPROBER_BUILD_TESTS=ON", "-DANDUEPROBER_BUILD_INSPECTOR=ON",
                       f"-DCMAKE_INSTALL_INCLUDEDIR={includedir}"])
            run(name + "-build", ["cmake", "--build", directory, "--target",
                                     "andueprober_inspector", "header_Inspector_hpp", "-j", args.jobs])
            run(name + "-ctest", ["ctest", "--test-dir", directory, "-R", "^andueprober.inspector$", "--output-on-failure", "-V"])
            prefixes[name] = install(name, directory)
            upstream = directory / "_deps/andueprober_imgui-src"
            license_path = prefixes[name] / "share/licenses/AndUEProberImGui/LICENSE.txt"
            if license_path.read_bytes() != (upstream / "LICENSE.txt").read_bytes():
                raise RuntimeError("Installed ImGui license differs from the fixed source")
            consumer(name + "-installed", prefixes[name])

        consumer("source", source=True)
        consumer("source-sanitizers", source=True,
                 extra=("-DCMAKE_BUILD_TYPE=Debug", "-DANDUEPROBER_SANITIZERS=ON"))
        imgui = build / "default-producer/_deps/andueprober_imgui-src"
        caller_option = f"-DINSPECTOR_IMGUI_SOURCE_DIR={imgui}"
        caller_directory = consumer("caller-source", source=True, extra=(caller_option,))
        caller_prefix = install("caller", caller_directory)
        if list(caller_prefix.rglob("*AndUEProberImGuiDependency*")) or list(caller_prefix.rglob("AndUEProberImGuiConfig.cmake")):
            raise RuntimeError("Caller package contains an unexpected default ImGui provider")
        consumer("caller-installed", caller_prefix,
                 extra=(caller_option, "-DCMAKE_DISABLE_FIND_PACKAGE_AndUEProberImGui=ON"))
        consumer("default-installed-caller", prefixes["default"],
                 extra=(caller_option, "-DCMAKE_DISABLE_FIND_PACKAGE_AndUEProberImGui=ON"))
        incompatible = build / "incompatible-caller"
        configure("incompatible-caller-configure", ROOT / "tests/inspector_consumer", incompatible,
                  [f"-DCMAKE_PREFIX_PATH={prefixes['default']}", caller_option, "-DINSPECTOR_TEST_WRONG_VERSION=ON"])
        mismatch_log = run("incompatible-caller-build", ["cmake", "--build", incompatible, "-j", args.jobs], expected=1)
        if "AndUEProber Inspector requires ImGui 1.92.2b" not in mismatch_log.read_text():
            raise RuntimeError("The incompatible provider did not fail the version contract")
        run("caller-required", ["cmake", "-S", ROOT / "tests/inspector_consumer", "-B", build / "missing-caller",
                                "-G", "Ninja", f"-DCMAKE_PREFIX_PATH={caller_prefix}"], expected=1)

        core_directory = build / "core-producer"
        configure("core-configure", ROOT, core_directory,
                  ["-DCMAKE_BUILD_TYPE=Release", "-DANDUEPROBER_BUILD_AGENT=OFF",
                   "-DANDUEPROBER_BUILD_TESTS=OFF", "-DANDUEPROBER_BUILD_INSPECTOR=OFF"])
        run("core-build", ["cmake", "--build", core_directory, "-j", args.jobs])
        core_prefix = install("core", core_directory)
        for name, prefix in (("core-only", core_prefix), ("full-package-core", prefixes["default"])):
            directory = build / name
            configure(name + "-configure", ROOT / "tests/consumer", directory,
                      [f"-DCMAKE_PREFIX_PATH={prefix}", "-DCMAKE_DISABLE_FIND_PACKAGE_AndUEProberImGui=ON"])
            run(name + "-build", ["cmake", "--build", directory, "-j", args.jobs])
            run(name + "-execute", [directory / "consumer"])

        if args.ndk:
            android = [f"-DCMAKE_TOOLCHAIN_FILE={args.ndk.resolve()}/build/cmake/android.toolchain.cmake",
                       "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-27", "-DANDROID_STL=c++_static"]
            directory = build / "android-producer"
            configure("android-configure", ROOT, directory,
                      [*android, "-DCMAKE_BUILD_TYPE=Release", "-DANDUEPROBER_BUILD_AGENT=OFF",
                       "-DANDUEPROBER_BUILD_TESTS=OFF", "-DANDUEPROBER_BUILD_INSPECTOR=ON"])
            run("android-build", ["cmake", "--build", directory, "-j", args.jobs])
            prefix = install("android", directory)
            consumer("android-installed", prefix, extra=(*android, f"-DCMAKE_FIND_ROOT_PATH={prefix}"), execute=False)
        report["status"] = "passed"
    except Exception as error:
        report["status"] = "failed"
        report["error"] = str(error)
        raise
    finally:
        report["final_source_files"] = sources()
        report["source_consistent"] = report["source_files"] == report["final_source_files"]
        if not report["source_consistent"]:
            report["status"] = "failed"
        for prefix in report["prefixes"].values():
            for path in sorted(Path(prefix).rglob("*")):
                if path.is_file():
                    report["artifacts"][str(path.relative_to(ROOT))] = sha256(path)
        for name in ("source", "source-sanitizers", "default-installed", "custom-include-installed",
                     "caller-source", "caller-installed", "default-installed-caller", "android-installed"):
            path = build / name / "inspector_consumer"
            if path.exists():
                report["artifacts"][str(path.relative_to(ROOT))] = sha256(path)
        target = ROOT / "tests/inspector-verification.json"
        (logs / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
        target.write_text(json.dumps(report, indent=2) + "\n")
        print(f"Evidence: {target}; source_consistent={report['source_consistent']}", flush=True)


if __name__ == "__main__":
    main()
