"""Bounded CPU-only startup-witness + exact native Storm low-domain gate.

Use only after the integration owner freezes writers and configures/builds the
dedicated product tree. This runner builds ONLY named CPU-test targets, at
BelowNormal/-j2, and runs them on the existing restricted-medium non-input
Desktop. It never launches Game/War3, deploys a DLL, changes registry/system
state or starts a DBWIN listener. Native Storm is an exact-hash private copy,
passed explicitly to the fixture via --storm, not Game.dll loading.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import sys

import analyze_startup_witness as reader
from isolated_render_desktop import IsolatedRenderDesktop
from run_device_owned_build import inputs
from run_fixed_function_key_equivalence import completed
from run_native_d3d9_survey import NativeObserver, checked_path, strict_json
from run_render_host_core_gate import execute
from run_render_host_protocol_gate import ROOT, identity, pe_identity

STORM_SHA256 = "F8F519CFAA6275A5172A014F0ABED2212284390A33F1194677155A7D408E63EB"
CORE_TARGET = "src/d3d9/warvk-startup-core-test.exe"
RUNTIME_TARGET = "src/d3d9/warvk-startup-runtime-test.exe"
DEFAULT_NATIVE_TARGET = "src/d3d9/warvk-storm-low-domain-test.exe"
FIXTURE_PID = 1234
FIXTURE_CREATION = 0x1122334455667788
NATIVE_MATRIX = (
    ("native-off-plain-injected", "off", "plain", True, False),
    ("native-search-plain", "search", "plain", False, False),
    ("native-off-debug", "off", "debug", False, False),
    ("native-off-protect", "off", "protect", False, False),
    ("native-off-plain-disabled", "off", "plain", False, True),
    ("native-search-plain-disabled", "search", "plain", False, True),
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def native_target(value):
    """One CPU-test leaf, never a shipping DLL or arbitrary Ninja expression."""
    require(re.fullmatch(r"src/d3d9/warvk-[a-z0-9-]+-test\.exe", value) is not None,
            "native target must be one src/d3d9/warvk-*-test.exe")
    return value


def verify_runtime_stdout(text, launch, mode):
    match = re.fullmatch(r"startup runtime checks=(\d+) failures=0 pid=(\d+) "
                         r"creation100ns=(\d+) mode=(--emit|--off|--existing)\s*", text)
    require(match is not None, "runtime fixture did not emit its exact success marker")
    checks, pid, creation = map(int, match.groups()[:3])
    require(checks > 0 and pid == launch["pid"] and creation == launch["creation100ns"]
            and match.group(4) == mode, "runtime fixture owner/mode mismatch")
    return dict(checks=checks, pid=pid, creation100ns=creation, mode=mode)


def verify_native_stdout(text, *, native, repair="off", option="plain",
                         injected=False, disabled=False):
    rows = {}
    zero_rows = []
    for line in text.splitlines():
        if not line.strip():
            continue
        prefix, separator, payload = line.partition(" ")
        if prefix == "ZERO_REALLOC" and separator:
            require(len(zero_rows) < 8, "extra zero-size native result")
            zero_rows.append(strict_json(payload))
            continue
        require(separator and prefix in ("RESULT", "NATIVE") and prefix not in rows,
                "unknown/duplicate native fixture output")
        rows[prefix] = strict_json(payload)
    require(set(rows) == ({"RESULT", "NATIVE"} if native else {"RESULT"}),
            "native/core fixture marker set mismatch")
    result = rows["RESULT"]
    require(set(result) == {"checks", "failures", "native", "ok"}, "RESULT shape mismatch")
    require(type(result["checks"]) is int and result["checks"] > 0 and
            type(result["failures"]) is int and result["failures"] == 0 and
            result["native"] is native and result["ok"] is True,
            "native/core fixture failed or did not run checks")
    if native:
        expected_zero = []
        for phase in ("unhooked-reference", "disabled-target" if disabled else "hooked-target"):
            for old_size in (4096, 0x20010):
                external = option == "protect" or old_size > 0xFE7B
                for flags in (0x10, 0):
                    refused = external and flags == 0x10
                    expected_zero.append(dict(phase=phase, oldSize=old_size, flags=flags,
                        external=external, resultNull=refused, samePointer=not external,
                        liveSize=old_size if refused else 0))
        require(len(zero_rows) == 8, "missing native zero-size reference/target matrix")
        for actual, expected in zip(zero_rows, expected_zero):
            require(set(actual) == set(expected) and all(
                type(actual[key]) is type(value) and actual[key] == value
                for key, value in expected.items()), "native zero-size semantics differ from exact contract")
        rows["ZERO_REALLOC"] = zero_rows
        details = rows["NATIVE"]
        numbers = {"enabled", "scopes", "reserves", "highRejected", "searchQueries",
                   "failures", "preexisting", "creates", "createdLow", "injections"}
        require(set(details) == numbers | {"option", "repair"}, "NATIVE shape mismatch")
        require(all(type(details[key]) is int and 0 <= details[key] <= 0xFFFFFFFF
                    for key in numbers), "invalid native counters")
        require(details["enabled"] == (0 if disabled else 1) and
                details["option"] == option and details["repair"] == repair and
                details["failures"] == 0, "native actual mode/result mismatch")
        require(details["injections"] == (1 if injected else 0), "high injection coverage mismatch")
        if disabled:
            require(details["scopes"] == 0 and details["reserves"] == 0 and
                    details["creates"] == 0 and details["createdLow"] == 0,
                    "disabled low-domain path still performed work")
        else:
            require(details["scopes"] > 0 and details["reserves"] > 0 and
                    (option == "protect" or details["creates"] > 0) and
                    details["creates"] == details["createdLow"],
                    "native low-domain production paths not exercised")
        if injected:
            require(details["highRejected"] > 0 and details["searchQueries"] > 0,
                    "injected high reserve did not exercise rejection/search")
    else:
        require(not zero_rows, "core fixture cannot claim native zero-size coverage")
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--storm", type=Path, required=True,
                        help="exact 1.27a Storm.dll source; copied under fresh artifacts")
    parser.add_argument("--native-target", default=DEFAULT_NATIVE_TARGET,
                        help="configured native CPU-test target accepting --storm exact-copy")
    args = parser.parse_args()
    require(os.name == "nt", "Windows native CPU gate required")
    build = checked_path(args.build, exists=True)
    output = checked_path(args.output)
    storm_source = checked_path(args.storm, exists=True)
    require(build.parent == ROOT and build.name.startswith("build32_device_owner_"),
            "dedicated already-configured product build required")
    require((build / "build.ninja").is_file(), "configure through the integration owner first")
    require(output.parent == ROOT / "AutoTest/artifacts", "fresh direct artifact child required")
    target = native_target(args.native_target)
    output.mkdir(exist_ok=False)
    paths = sorted(set(inputs()) | {
        Path(__file__).resolve(), Path(reader.__file__).resolve(),
        ROOT / "AutoTest/test_startup_witness.py",
        ROOT / "AutoTest/test_startup_witness_gate.py",
        ROOT / "AutoTest/test_startup_witness_core.cpp",
        ROOT / "AutoTest/test_startup_witness_runtime.cpp",
        ROOT / "AutoTest/test_storm_low_domain.cpp",
        ROOT / "AutoTest/isolated_render_desktop.py",
        ROOT / "AutoTest/run_native_d3d9_survey.py",
        ROOT / "AutoTest/run_fixed_function_key_equivalence.py",
        ROOT / "AutoTest/run_render_host_core_gate.py",
        ROOT / "AutoTest/run_render_host_protocol_gate.py",
    })
    receipt = dict(schema="warvk-storm-low-domain-cpu-gate/v1", ok=False,
                   cpuOnly=True, gameLaunched=False, gpuExecuted=False,
                   shippingDllBuilt=False, deployed=False, releaseReady=False,
                   nativeTarget=target, steps=[], runs=[])
    player = Path("E:/Work/Warcraft III/d3d9.dll")
    before = protected = None
    leases = None
    try:
        before = [identity(path) for path in paths]
        protected = identity(player)
        receipt.update(sources=before, preservedBefore=protected)
        leases = NativeObserver()  # Only lease/close; no process bind or IPC.
        leases.lease(storm_source)
        receipt["stormSource"] = identity(storm_source)
        require(receipt["stormSource"]["sha256"] == STORM_SHA256,
                "native Storm source does not match the frozen exact module")
        require(pe_identity(storm_source) == (0x14C, 0x10B), "native Storm must be x86 PE32")
        storm_copy = output / "Storm.dll"
        with storm_source.open("rb") as source, storm_copy.open("xb") as destination:
            shutil.copyfileobj(source, destination, 1 << 20)
        leases.lease(storm_copy)
        receipt["stormCopy"] = identity(storm_copy)
        require(receipt["stormCopy"]["sha256"] == STORM_SHA256,
                "native Storm copy identity mismatch")
        receipt["stormDenyWriteLeases"] = True

        env = os.environ.copy()
        env["PATH"] = "D:/Environment/Python3.13.11/Scripts;E:/Dev/Vulkan SDK/Bin;" + env["PATH"]
        targets = [CORE_TARGET, RUNTIME_TARGET, target]
        receipt["steps"].append(execute(["ninja", "-C", build, "-j2", *targets],
                                         ROOT, output / "build", 240, env))
        receipt["steps"].append(execute(["ninja", "-C", build, "-n", *targets,
                                         "src/d3d9/d3d9.dll"], ROOT, output / "no-work", 30, env))
        require("no work to do" in (output / "no-work.stdout").read_text("utf-8"),
                "candidate/test source drift; product build is the owner's responsibility")
        products = [build / name for name in targets]
        require(all(pe_identity(path) == (0x14C, 0x10B) for path in products),
                "all CPU fixture executables must be x86 PE32")
        receipt["products"] = [identity(path) for path in products]
        receipt["dll"] = identity(build / "src/d3d9/d3d9.dll")
        child = {key: value for key, value in env.items() if key.upper() in {
            "SYSTEMROOT", "WINDIR", "COMSPEC", "PATH", "TEMP", "TMP", "USERPROFILE",
            "APPDATA", "LOCALAPPDATA", "PROGRAMDATA"}}
        core_fixture = output / "startup-core.bin"
        runtime_fixture = output / "startup-runtime.bin"
        off_fixture = output / "startup-disabled-must-not-exist.bin"

        with IsolatedRenderDesktop(restricted_medium=True) as desktop:
            receipt["desktopSafety"] = desktop.receipt

            def run(label, command):
                launch = desktop.run(command, cwd=output,
                    stdout_path=output / (label + ".stdout"),
                    stderr_path=output / (label + ".stderr"), timeout_s=30, env=child)
                row = dict(label=label, launch=launch)
                receipt["runs"].append(row)
                completed(launch, 0)
                require((output / (label + ".stderr")).read_bytes() == b"", label + " stderr")
                return row

            run("startup-core", [products[0], "--emit", core_fixture])
            runtime = run("startup-runtime", [products[1], "--emit", runtime_fixture])
            runtime["fixtureSummary"] = verify_runtime_stdout(
                (output / "startup-runtime.stdout").read_text("utf-8"), runtime["launch"], "--emit")
            # All reads below follow completed(exact launch, 0), never live mapping reads.
            receipt["runtimeRoundtrip"] = reader.analyze_file(runtime_fixture, process_exited=True,
                expected_pid=runtime["launch"]["pid"],
                expected_creation_time=runtime["launch"]["creation100ns"])
            require(receipt["runtimeRoundtrip"]["recordingComplete"] and
                    receipt["runtimeRoundtrip"]["eventCount"] == 3 and
                    receipt["runtimeRoundtrip"]["runtimeReadyObserved"] and
                    receipt["runtimeRoundtrip"]["mainRunnerOriginalCalls"][0]["returnObserved"],
                    "actual runtime writer/reader roundtrip mismatch")
            runtime_before = identity(runtime_fixture)
            off = run("startup-off", [products[1], "--off", off_fixture])
            off["fixtureSummary"] = verify_runtime_stdout(
                (output / "startup-off.stdout").read_text("utf-8"), off["launch"], "--off")
            require(not off_fixture.exists(), "runtime-OFF created a witness file")
            existing = run("startup-existing", [products[1], "--existing", runtime_fixture])
            existing["fixtureSummary"] = verify_runtime_stdout(
                (output / "startup-existing.stdout").read_text("utf-8"), existing["launch"], "--existing")
            require(runtime_before == identity(runtime_fixture), "CREATE_NEW modified existing evidence")
            native_core = run("storm-core", [products[2], "--core"])
            native_core["fixtureSummary"] = verify_native_stdout(
                (output / "storm-core.stdout").read_text("utf-8"), native=False)
            for label, repair, option, injected, disabled in NATIVE_MATRIX:
                command = [products[2], "--storm", storm_copy,
                           "--repair-mode", repair, "--option", option]
                if injected:
                    command.append("--inject-high")
                if disabled:
                    command.append("--disabled")
                native = run(label, command)
                native["fixtureSummary"] = verify_native_stdout(
                    (output / (label + ".stdout")).read_text("utf-8"), native=True,
                    repair=repair, option=option, injected=injected, disabled=disabled)
                native["stormCopy"] = identity(storm_copy)

        safety = receipt["desktopSafety"]
        require(safety["ok"] and safety["desktopClosed"] and safety["creatorExited"] and
                not safety["cleanupErrors"] and not safety["globalInputUsed"] and
                not safety["desktopSwitchRequested"], "isolated Desktop cleanup failed")
        receipt["coreRoundtrip"] = reader.analyze_file(core_fixture, process_exited=True,
            expected_pid=FIXTURE_PID, expected_creation_time=FIXTURE_CREATION)
        require(receipt["coreRoundtrip"]["recordingComplete"] and
                receipt["coreRoundtrip"]["eventCount"] == 12 and
                receipt["coreRoundtrip"]["pipeCreateFailures"] == [dict(sequence=9, win32Error=5)],
                "production-core fixture/reader mismatch")
        env["WARVK_STARTUP_WITNESS_FIXTURE"] = str(core_fixture)
        receipt["steps"].append(execute([sys.executable, "-B", ROOT / "AutoTest/test_startup_witness.py"],
                                         ROOT, output / "startup-reader", 30, env))
        reader_output = (output / "startup-reader.stderr").read_text("utf-8")
        require("skipped=" not in reader_output and "OK" in reader_output,
                "real writer-reader test was skipped or not passed")
        receipt["steps"].append(execute([sys.executable, "-B", ROOT / "AutoTest/test_startup_witness_gate.py"],
                                         ROOT, output / "gate-reader", 30, env))
        receipt["fixtures"] = [identity(core_fixture), identity(runtime_fixture)]
        receipt["stormSourceUnchanged"] = receipt["stormSource"] == identity(storm_source)
        receipt["stormCopyUnchanged"] = receipt["stormCopy"] == identity(storm_copy)
        require(receipt["stormSourceUnchanged"] and receipt["stormCopyUnchanged"], "native module drift")
        receipt["ok"] = True
    except Exception as error:
        receipt["error"] = f"{type(error).__name__}: {error}"
    finally:
        if leases is not None:
            receipt["leaseCleanupErrors"] = leases.close()
            if receipt["leaseCleanupErrors"]:
                receipt["ok"] = False
        try:
            receipt["sourceUnchanged"] = before is not None and before == [identity(path) for path in paths]
            receipt["preservedAfter"] = identity(player)
            receipt["preservedUnchanged"] = protected is not None and protected == receipt["preservedAfter"]
            receipt["ok"] &= receipt["sourceUnchanged"] and receipt["preservedUnchanged"]
        except Exception as error:
            receipt["ok"] = False
            receipt["finalIdentityError"] = f"{type(error).__name__}: {error}"
        with (output / "receipt.json").open("x", encoding="utf-8") as stream:
            json.dump(receipt, stream, indent=2)
    print(json.dumps({key: receipt.get(key) for key in
                     ("ok", "error", "sourceUnchanged", "preservedUnchanged")}))
    return 0 if receipt["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
