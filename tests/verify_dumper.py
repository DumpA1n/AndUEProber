#!/usr/bin/env python3
"""Validate the bounded pinned emitter and public static DumperAdapter consumers."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sources():
    paths = {ROOT / "CMakeLists.txt", ROOT / "tests/CMakeLists.txt", Path(__file__).resolve(),
             ROOT / "tools/extract_dumper_emitter.py", ROOT / "LICENSE", ROOT / "THIRD_PARTY_NOTICES.md"}
    for directory in ("cmake", "source/Core", "source/DumperEmitter",
                      "tests/dumper_consumer", "tests/dumper_emitter"):
        paths.update(path for path in (ROOT / directory).rglob("*") if path.is_file() and
                     path.suffix in {".cpp", ".hpp", ".h", ".cmake", ".txt", ".in"})
    # Public header inputs follow the selected implementation and fixture include graph.
    pending = list(paths)
    while pending:
        path = pending.pop()
        if path.suffix not in {".cpp", ".hpp", ".h"}:
            continue
        for name in re.findall(r'#include\s*[<"](?:andueprober/)?([^>"\n]+)[>"]', path.read_text()):
            header = ROOT / "include/andueprober" / name
            if header.is_file() and header not in paths:
                paths.add(header)
                pending.append(header)
    spec = runpy.run_path(str(ROOT / "tools/extract_dumper_emitter.py"))
    paths.update(ROOT / "external/AndUEDumper" / name for name in spec["INPUTS"])
    return {str(path.relative_to(ROOT)): sha(path) for path in sorted(paths)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path)
    args = parser.parse_args()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    build = ROOT / "build/dumper-validation" / stamp
    logs = ROOT / "artifacts/dumper-validation" / stamp
    build.mkdir(parents=True)
    logs.mkdir(parents=True)
    report = {"schema_version": 1, "timestamp_utc": stamp,
              "scope": "Checked pinned emitter; source and relocated installed static-to-caller-DSO DumperAdapter consumption",
              "source_sha256": sources(), "checks": [], "artifact_sha256": {}, "prefixes": {},
              "source_scope": "Core implementation and recursively included public headers, DumperAdapter/emitter, checked upstream formatter inputs, their CMake graph and owned consumer fixtures. Probe archives and unrelated installed headers are supporting package build artifacts, not runtime-validated by this gate.",
              "android_runtime": "not executed by this runner"}

    def run(name, command, failure_text=None):
        command = [str(value) for value in command]
        log = logs / (name + ".log")
        print(name, flush=True)
        with log.open("w") as output:
            output.write(json.dumps(command) + "\n")
            output.flush()
            result = subprocess.run(command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT)
        content = log.read_text().split("\n", 1)[1]
        passed = result.returncode == 0 if failure_text is None else result.returncode != 0 and failure_text in content
        report["checks"].append({"name": name, "command": command, "exit_code": result.returncode,
            "expected": "success" if failure_text is None else "failure: " + failure_text,
            "passed": passed, "log": str(log), "log_sha256": sha(log)})
        if not passed:
            raise RuntimeError(f"{name} failed; inspect {log}")
        return content

    def configure(name, source, flags=()):
        run(name + "-configure", ["cmake", "-S", source, "-B", build / name, "-G", "Ninja",
                                   "-DCMAKE_BUILD_TYPE=Release", *flags])

    def compile(name, targets=()):
        run(name + "-build", ["cmake", "--build", build / name, "--parallel", "4", "--verbose",
                               *(["--target", *targets] if targets else [])])

    def execute(name):
        run(name + "-ctest", ["ctest", "--test-dir", build / name, "--no-tests=error", "--output-on-failure", "-V"])

    def install(name):
        original = build / (name + "-install")
        relocated = build / (name + "-relocated")
        run(name + "-install", ["cmake", "--install", build / name, "--prefix", original])
        original.rename(relocated)
        report["prefixes"][name] = str(relocated)
        for name in ("AndUEDumper", "fmt"):
            source = ROOT / "external/AndUEDumper" / ("LICENSE" if name == "AndUEDumper" else "deps/fmt/LICENSE")
            installed = relocated / "share/licenses/AndUEProber" / name / "LICENSE"
            if source.read_bytes() != installed.read_bytes():
                raise RuntimeError(f"Installed {name} license is not byte-identical")
        return relocated

    consumer = ROOT / "tests/dumper_consumer"
    consumer_targets = ("owned_dumper", "dumper_consumer", "header_DumperAdapter_hpp", "header_Reflection_hpp")
    producer_flags = ["-DANDUEPROBER_BUILD_AGENT=OFF", "-DANDUEPROBER_BUILD_TESTS=ON",
                      "-DANDUEPROBER_BUILD_INSPECTOR=OFF", "-DANDUEPROBER_WITH_PROCESS_MEMORY=OFF",
                      "-DANDUEPROBER_BUILD_DUMPER_ADAPTER=ON", "-DCMAKE_INSTALL_INCLUDEDIR=include/custom"]
    isolated = ["-DCMAKE_DISABLE_FIND_PACKAGE_AndSwapChainHook=ON", "-DCMAKE_DISABLE_FIND_PACKAGE_AndUEProberImGui=ON"]
    try:
        for name, extra in (("host-debug", ["-DCMAKE_BUILD_TYPE=Debug"]), ("host-release", []),
                            ("host-sanitized", ["-DCMAKE_BUILD_TYPE=Debug", "-DANDUEPROBER_SANITIZERS=ON"])):
            configure(name, consumer, [f"-DANDUEPROBER_SOURCE={ROOT}", *extra])
            compile(name, consumer_targets)
            execute(name)
        configure("host-producer", ROOT, producer_flags)
        compile("host-producer", ["AndUEProberCore", "AndUEProberProbe", "AndUEProberDumperAdapter", "andueprober_dumper_emitter"])
        run("host-private-emitter", [build / "host-producer/tests/andueprober_dumper_emitter"])
        symbols = run("host-emitter-symbols", ["nm", "-u", build / "host-producer/libAndUEProberDumperAdapter.a"])
        if re.search(r"UEWrappers|GetUEVars|UEMemory|Kitty|UE_UPackage", symbols):
            raise RuntimeError("Adapter archive contains a live dumper dependency")
        host = install("host-producer")
        configure("host-installed", consumer, [f"-DCMAKE_PREFIX_PATH={host}", *isolated])
        compile("host-installed", consumer_targets)
        execute("host-installed")
        # The clone is an isolated local verification input; no source repository is changed.
        mutated = build / "mutated-upstream"
        run("extraction-clone", ["git", "clone", "--shared", "--no-checkout", ROOT / "external/AndUEDumper", mutated])
        spec = runpy.run_path(str(ROOT / "tools/extract_dumper_emitter.py"))
        for name in spec["INPUTS"]:
            destination = mutated / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / "external/AndUEDumper" / name, destination)
        changed = mutated / "AndUEDumper/src/UPackageGenerator.cpp"
        original = changed.read_bytes()
        changed.write_bytes(original + b"\n")
        run("extraction-mutated-source", [sys.executable, ROOT / "tools/extract_dumper_emitter.py", "--source", mutated,
            "--output", build / "rejected-extraction"], "source digest mismatch")
        changed.write_bytes(original)
        run("extraction-move-local-ref", ["git", "-C", mutated, "update-ref", "HEAD", "HEAD^"])
        run("extraction-wrong-revision", [sys.executable, ROOT / "tools/extract_dumper_emitter.py", "--source", mutated,
            "--output", build / "rejected-extraction"], "revision mismatch")
        if args.ndk:
            ndk = args.ndk.resolve()
            report["ndk_source_properties_sha256"] = sha(ndk / "source.properties")
            cross = [f"-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake", "-DANDROID_ABI=arm64-v8a",
                     "-DANDROID_PLATFORM=android-27", "-DANDROID_STL=c++_static"]
            for name, mode in (("android-debug", "Debug"), ("android-release", "Release")):
                configure(name, consumer, [f"-DANDUEPROBER_SOURCE={ROOT}", f"-DCMAKE_BUILD_TYPE={mode}", *cross])
                compile(name, consumer_targets)
            configure("android-producer", ROOT, [*producer_flags, *cross])
            compile("android-producer", ["AndUEProberCore", "AndUEProberProbe", "AndUEProberDumperAdapter", "andueprober_dumper_emitter"])
            android = install("android-producer")
            configure("android-installed", consumer, [f"-DCMAKE_PREFIX_PATH={android}", f"-DCMAKE_FIND_ROOT_PATH={android}", *isolated, *cross])
            compile("android-installed", consumer_targets)
            configure("android-generated", consumer / "smoke", [f"-DHEADER_DIR={build / 'host-installed/generated'}", *cross])
            compile("android-generated")
        report["status"] = "passed"
    except Exception as error:
        report["status"] = "failed"
        report["error"] = str(error)
    finally:
        report["source_consistent"] = report["source_sha256"] == sources()
        if not report["source_consistent"]:
            report["source_after_sha256"] = sources()
            report["status"] = "failed"
        for path in sorted(build.rglob("*")):
            if "mutated-upstream" in path.parts or not path.is_file():
                continue
            if path.suffix in {".a", ".so", ".dylib", ".cmake", ".hpp", ".json"} or path.name in {
                    "dumper_consumer", "dumper_smoke", "andueprober_dumper_emitter", "CMakeCache.txt", "LICENSE", "THIRD_PARTY_NOTICES.md"}:
                report["artifact_sha256"][str(path)] = sha(path)
        (logs / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
        (ROOT / "tests/dumper_emitter/verification.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps({"status": report["status"], "source_consistent": report["source_consistent"],
                          "checks": len(report["checks"]), "evidence": str(logs / "verification.json")}))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
