#!/usr/bin/env python3
"""Verify public static-to-DSO and component-selective package consumption."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sources():
    paths = {ROOT / "CMakeLists.txt", Path(__file__).resolve()}
    paths.add(ROOT / "tools/extract_dumper_emitter.py")
    for directory in ("cmake", "include", "source", "tests/component_consumer", "tests/inspector_consumer", "tests/agent_consumer", "tests/package_consumer",
                      "external/AndUEDumper/AndUEDumper/src", "external/AndUEDumper/deps",
                      "external/AndSwapChainHook/external/KittyMemoryEx/KittyMemoryEx"):
        paths.update(path for path in (ROOT / directory).rglob("*")
                     if path.is_file() and path.suffix in {".cpp", ".c", ".h", ".hpp", ".in", ".cmake", ".txt"})
    return {str(path.relative_to(ROOT)): sha(path) for path in sorted(paths)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-type", choices=["Debug", "Release"], default="Release",
                        help="Build type for source and installed consumers; host-sanitized always uses Debug")
    parser.add_argument("--host-memory-prefix", type=Path, required=True)
    parser.add_argument("--ndk", type=Path)
    parser.add_argument("--android-memory-prefix", type=Path)
    args = parser.parse_args()
    if args.ndk and not args.android_memory_prefix:
        parser.error("--ndk requires --android-memory-prefix")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + args.build_type.lower()
    build = ROOT / "build/component-validation" / stamp
    logs = ROOT / "artifacts/component-validation" / stamp
    build.mkdir(parents=True)
    logs.mkdir(parents=True)
    report = {"schema_version": 1, "timestamp_utc": stamp.rsplit("-", 1)[0], "build_type": args.build_type,
              "sanitizer_build_type": "Debug",
              "scope": "Public static components in a caller DSO; component-selective installed packages; Agent C linkage",
              "source_sha256": sources(), "checks": [], "dependency_artifacts": {},
              "android_runtime": "not executed by this runner", "artifact_sha256": {}, "prefixes": {}}

    def run(name, command, expected=0, expected_text=None):
        path = logs / (name + ".log")
        print(name, flush=True)
        with path.open("w") as stream:
            stream.write(json.dumps([str(value) for value in command]) + "\n")
            stream.flush()
            result = subprocess.run([str(value) for value in command], cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
        passed = result.returncode == expected if expected == 0 else result.returncode != 0
        if expected_text:
            passed = passed and expected_text in path.read_text().split("\n", 1)[1]
        report["checks"].append({"name": name, "command": [str(value) for value in command],
            "exit_code": result.returncode, "expected": "success" if expected == 0 else "failure",
            "passed": passed, "expected_output": expected_text, "log": str(path), "log_sha256": sha(path)})
        if not passed:
            raise RuntimeError(f"{name} failed; inspect {path}")

    def configure(name, source, flags):
        run(name + "-configure", ["cmake", "-S", source, "-B", build / name, "-G", "Ninja",
                                  "-DCMAKE_BUILD_TYPE=" + args.build_type, *flags])
        run(name + "-build", ["cmake", "--build", build / name, "--parallel", "4", "--verbose"])

    def execute(name):
        run(name + "-ctest", ["ctest", "--test-dir", build / name, "--output-on-failure", "-V"])

    def dependency(prefix, name):
        prefix = prefix.resolve()
        selected = [prefix / "lib/libAndSwapChainHookMemory.a",
                    prefix / "include/AndSwapChainHook/Memory.h"]
        selected.extend((prefix / "lib/cmake/AndSwapChainHook").glob("*.cmake"))
        report["dependency_artifacts"][name] = {str(path): sha(path) for path in selected}
        return prefix

    def install(name):
        original = build / (name + "-install")
        relocated = build / (name + "-relocated")
        run(name + "-install", ["cmake", "--install", build / name, "--prefix", original])
        original.rename(relocated)
        report["prefixes"][name] = str(relocated)
        return relocated

    def search(prefixes):
        value = ";".join(str(prefix) for prefix in prefixes)
        return ["-DCMAKE_PREFIX_PATH=" + value, "-DCMAKE_FIND_ROOT_PATH=" + value]

    consumer = ROOT / "tests/component_consumer"
    isolated = ["-DCMAKE_DISABLE_FIND_PACKAGE_AndSwapChainHook=ON", "-DCMAKE_DISABLE_FIND_PACKAGE_AndUEProberImGui=ON"]
    producer_flags = ["-DANDUEPROBER_BUILD_TESTS=OFF", "-DANDUEPROBER_BUILD_INSPECTOR=ON",
                      "-DANDUEPROBER_BUILD_DUMPER_ADAPTER=ON",
                      "-DANDUEPROBER_WITH_PROCESS_MEMORY=ON", "-DCMAKE_INSTALL_INCLUDEDIR=include/custom"]
    try:
        host = dependency(args.host_memory_prefix, "host-memory")
        for name, extra in (("host-source", []), ("host-sanitized", ["-DCMAKE_BUILD_TYPE=Debug", "-DANDUEPROBER_SANITIZERS=ON"])):
            configure(name, consumer, [f"-DANDUEPROBER_SOURCE={ROOT}", *extra])
            execute(name)
        configure("host-source-full", consumer, [f"-DANDUEPROBER_SOURCE={ROOT}", "-DCOMPONENT_WITH_INSPECTOR=ON",
                  "-DCOMPONENT_WITH_MEMORY=ON", "-DCOMPONENT_WITH_DUMPER=ON", *search([host])])
        execute("host-source-full")
        configure("host-inspector-source", ROOT / "tests/inspector_consumer", [f"-DANDUEPROBER_SOURCE={ROOT}"])
        execute("host-inspector-source")
        configure("host-producer", ROOT, ["-DANDUEPROBER_BUILD_AGENT=OFF", *producer_flags, *search([host])])
        host_install = install("host-producer")
        cases = (
            ("missing-memory", ["-DPACKAGE_COMPONENTS=ProcessMemory", "-DCMAKE_DISABLE_FIND_PACKAGE_AndSwapChainHook=ON"], 1, "CMAKE_DISABLE_FIND_PACKAGE_AndSwapChainHook"),
            ("missing-imgui", ["-DPACKAGE_COMPONENTS=Inspector", "-DCMAKE_DISABLE_FIND_PACKAGE_AndUEProberImGui=ON"], 1, "CMAKE_DISABLE_FIND_PACKAGE_AndUEProberImGui"),
            ("missing-agent", ["-DPACKAGE_COMPONENTS=Agent"], 1, "set AndUEProber_FOUND to FALSE"),
            ("private-emitter", ["-DPACKAGE_COMPONENTS=DumperEmitter"], 1, "set AndUEProber_FOUND to FALSE"),
            ("independent-dumper", ["-DPACKAGE_COMPONENTS=DumperAdapter", *isolated], 0, None),
            ("optional-agent", ["-DPACKAGE_COMPONENTS=Core", "-DPACKAGE_OPTIONAL_COMPONENTS=Agent", "-DPACKAGE_ABSENT_COMPONENTS=Agent"], 0, None),
            ("optional-memory", ["-DPACKAGE_COMPONENTS=Core", "-DPACKAGE_OPTIONAL_COMPONENTS=ProcessMemory", "-DPACKAGE_ABSENT_COMPONENTS=ProcessMemory", "-DCMAKE_DISABLE_FIND_PACKAGE_AndSwapChainHook=ON"], 0, None),
            ("optional-inspector", ["-DPACKAGE_COMPONENTS=Core", "-DPACKAGE_OPTIONAL_COMPONENTS=Inspector", "-DPACKAGE_ABSENT_COMPONENTS=Inspector", "-DCMAKE_DISABLE_FIND_PACKAGE_AndUEProberImGui=ON"], 0, None),
            ("invalid-caller", ["-DPACKAGE_COMPONENTS=Inspector", "-DANDUEPROBER_IMGUI_TARGET=AbsentCaller"], 1, "ANDUEPROBER_IMGUI_TARGET must name an existing caller target"),
            ("optional-invalid-caller", ["-DPACKAGE_COMPONENTS=Core", "-DPACKAGE_OPTIONAL_COMPONENTS=Inspector", "-DPACKAGE_ABSENT_COMPONENTS=Inspector", "-DANDUEPROBER_IMGUI_TARGET=AbsentCaller"], 0, None),
        )
        for name, flags, expected, message in cases:
            run("package-" + name, ["cmake", "-S", ROOT / "tests/package_consumer", "-B", build / ("package-" + name),
                "-G", "Ninja", *search([host_install]), *flags], expected=expected, expected_text=message)
        configure("host-installed-core", consumer, [*search([host_install]), *isolated])
        execute("host-installed-core")
        configure("host-installed-inspector", consumer, ["-DCOMPONENT_WITH_INSPECTOR=ON", *search([host_install]),
                  "-DCMAKE_DISABLE_FIND_PACKAGE_AndSwapChainHook=ON"])
        execute("host-installed-inspector")
        configure("host-installed-full", consumer, ["-DCOMPONENT_WITH_INSPECTOR=ON", "-DCOMPONENT_WITH_MEMORY=ON",
                  "-DCOMPONENT_WITH_DUMPER=ON", *search([host_install, host])])
        execute("host-installed-full")
        configure("host-inspector-installed", ROOT / "tests/inspector_consumer", [*search([host_install]),
                  "-DCMAKE_DISABLE_FIND_PACKAGE_AndSwapChainHook=ON"])
        execute("host-inspector-installed")
        if args.ndk:
            ndk = args.ndk.resolve()
            android = dependency(args.android_memory_prefix, "android-memory")
            report["ndk_source_properties_sha256"] = sha(ndk / "source.properties")
            cross = [f"-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake", "-DANDROID_ABI=arm64-v8a",
                     "-DANDROID_PLATFORM=android-27", "-DANDROID_STL=c++_static"]
            configure("android-source", consumer, [f"-DANDUEPROBER_SOURCE={ROOT}", *cross])
            configure("android-agent-source", ROOT / "tests/agent_consumer", [f"-DANDUEPROBER_SOURCE={ROOT}", *cross])
            configure("android-producer", ROOT, ["-DANDUEPROBER_BUILD_AGENT=ON", *producer_flags, *search([android]), *cross])
            android_install = install("android-producer")
            configure("android-installed-core", consumer, [*search([android_install]), *isolated, *cross])
            configure("android-installed-full", consumer, ["-DCOMPONENT_WITH_INSPECTOR=ON", "-DCOMPONENT_WITH_MEMORY=ON",
                      "-DCOMPONENT_WITH_DUMPER=ON", *search([android_install, android]), *cross])
            configure("android-inspector-source", ROOT / "tests/inspector_consumer", [f"-DANDUEPROBER_SOURCE={ROOT}", *cross])
            configure("android-inspector-installed", ROOT / "tests/inspector_consumer", [*search([android_install]),
                      "-DCMAKE_DISABLE_FIND_PACKAGE_AndSwapChainHook=ON", *cross])
            configure("android-agent-installed", ROOT / "tests/agent_consumer", [*search([android_install]), *isolated,
                      "-DCMAKE_DISABLE_FIND_PACKAGE_Threads=ON", *cross])
        report["status"] = "passed"
    except Exception as error:
        report["status"] = "failed"
        report["error"] = str(error)
    finally:
        report["source_consistent"] = report["source_sha256"] == sources()
        if not report["source_consistent"]:
            report["status"] = "failed"
            report["source_after_sha256"] = sources()
        for path in sorted(build.rglob("*")):
            if path.is_file() and (path.suffix in {".a", ".so", ".dylib", ".cmake"} or "license" in path.name.lower() or
                                  "notices" in path.name.lower() or
                                  path.name in {"component_consumer", "inspector_consumer", "agent_consumer", "CMakeCache.txt"}):
                report["artifact_sha256"][str(path)] = sha(path)
        (logs / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
        (ROOT / "tests/component-verification.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps({"status": report["status"], "source_consistent": report["source_consistent"],
                          "checks": len(report["checks"]), "evidence": str(logs / "verification.json")}))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
