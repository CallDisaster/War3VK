#!/usr/bin/env python3
"""1.30.00: the original game against WarVK with its lighting on, on this machine.

Two arms per map, alternating, the order flipping each round (the machine is shared):

  vanilla  the game as it ships, no WarVK at all: no d3d9.dll in the instance, and the
           game's own OpenGL renderer (run_x64_game_proxy_probe.py --vanilla --opengl).
           Its Direct3D renderer cannot start on an isolated desktop: the game reports
           that it could not initialize DirectX (v123-vanilla-diag-20261121-r2). The
           tests never run on the input desktop, so the Direct3D original is not measured.
  warvk    the release's x86 d3d9.dll in proxy mode with the release's x64 front end and
           host and the transport the player package sets (product-async-delta), every
           player setting at its default: sun shadows, volumetric light, fog,
           post-processing and the new water as a player gets them.

Both arms are counted the same way, from outside: the kernel's own present events
(ETW provider Microsoft-Windows-DxgKrnl, keyword Present, event 184), traced with
logman for the whole run, converted by tracerpt and cut to the same window of each run
(seconds after the probe saw the game start). The presenting process is the game in
the vanilla arm and the x64 host in the WarVK arm (the probe records the host's pid).
Frame rate and p50/p95/p99 frame times come from the intervals between presents. In the
WarVK arm the product's own frame counter is recorded too, as a cross-check (on
2026-11-21 the kernel counted 231.0 presents/s where the product counted 230.2 fps,
v123-warvk-etw-diag-20261121-r1).

The original game caps itself: its OpenGL renderer presented exactly 64 frames a second
on Lost Temple (v123-vanilla-gl-diag-20261121-r1), and its Direct3D renderer asks for
vertical sync. WarVK lifts the cap by default. A vanilla frame rate at the cap means the
game was waiting, not that the machine could do no more.

Also per run: the x86 process's free low 2 GiB and largest hole at the close, its peak
private bytes, and what else the machine was doing (busy cores, GPU load).
Same sandbox rules as the probe: isolated desktop, restricted token, nothing deployed,
no global input. The ETW session is a live diagnostic session of this run only.
"""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
import statistics
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from autotest_sessions import DEFAULT_SANDBOX_ROOT  # noqa: E402
from run_x64_phase52_ab_gate import (BELOW_NORMAL, LoadWatch, RUN_ATTEMPTS, desktop_refused,  # noqa: E402
    frame_times, median, wait_for_input_desktop)

ROOT = Path(__file__).resolve().parents[1]
ARTIFACT_ROOT = ROOT / "AutoTest" / "artifacts"
PROBE = ROOT / "AutoTest" / "run_x64_game_proxy_probe.py"
# window = (first, last) second of the measured stretch; the probe closes the game at `seconds`.
MAPS = {
    "losttemple": dict(path=DEFAULT_SANDBOX_ROOT / "Maps/(4)LostTemple.w3m", seconds=90, window=(50, 85), acts=[]),
    "echoisles": dict(path=DEFAULT_SANDBOX_ROOT / "Maps/FrozenThrone/(2)EchoIsles.w3x", seconds=90, window=(50, 85),
                      acts=[]),
    # Its own start dialog needs a key and it loads for a minute or more (run_x64_phase52_ab_gate.py).
    "shengyusi": dict(path=DEFAULT_SANDBOX_ROOT / "Maps/(4)生与死v1.28读档bug修复.w3x", seconds=170,
                      window=(135, 165), acts=["72:key=SPACE", "86:key=SPACE", "100:key=SPACE", "114:key=SPACE"]),
}
ARMS = ("vanilla", "warvk")
PROVIDER = ("Microsoft-Windows-DxgKrnl", "0x8000000", "4")  # keyword Present, level informational
PRESENT_EVENT = "184"
SETTLE_SECONDS = 12.0
FILETIME_UNIX = 11644473600


def require(condition, message):
    if not condition:
        raise ValueError(message)


def machine():
    """What the numbers were measured on."""
    script = ("$c=Get-CimInstance Win32_Processor|Select-Object -First 1 Name,NumberOfCores,NumberOfLogicalProcessors,"
              "MaxClockSpeed;$m=Get-CimInstance Win32_ComputerSystem|Select-Object TotalPhysicalMemory;"
              "$g=@(Get-CimInstance Win32_VideoController|Select-Object Name,DriverVersion,CurrentHorizontalResolution,"
              "CurrentVerticalResolution,CurrentRefreshRate);$o=Get-CimInstance Win32_OperatingSystem|"
              "Select-Object Caption,BuildNumber;[pscustomobject]@{cpu=$c;memory=$m;gpus=$g;os=$o}|ConvertTo-Json -Depth 4")
    row = {}
    try:
        text = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script], capture_output=True,
                              text=True, timeout=60, creationflags=BELOW_NORMAL).stdout
        row = json.loads(text)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        row["error"] = f"{type(error).__name__}: {error}"
    try:
        line = subprocess.run(["nvidia-smi", "--query-gpu=name,driver_version,memory.total",
                               "--format=csv,noheader"], capture_output=True, text=True, timeout=10,
                              creationflags=BELOW_NORMAL).stdout.strip()
        row["nvidiaSmi"] = line
    except (OSError, subprocess.SubprocessError):
        row["nvidiaSmi"] = None
    return row


class PresentTrace:
    """A live ETW session of the kernel's present events for one run (logman), read back with tracerpt."""

    def __init__(self, name, etl):
        self.name, self.etl, self.started = name, etl, False

    def start(self):
        result = subprocess.run(["logman", "start", self.name, "-p", *PROVIDER,
                                 "-o", str(self.etl), "-ets"], capture_output=True, text=True,
                                creationflags=BELOW_NORMAL)
        self.started = result.returncode == 0
        return dict(started=self.started, code=result.returncode)

    def stop(self):
        if not self.started:
            return dict(stopped=False)
        result = subprocess.run(["logman", "stop", self.name, "-ets"], capture_output=True, text=True,
                                creationflags=BELOW_NORMAL)
        self.started = result.returncode != 0
        return dict(stopped=result.returncode == 0, code=result.returncode)

    def presents(self, pids, first, last):
        """Event counts of the given processes, and their present times between two FILETIMEs.

        The trace is large (about 1 MB a second) and is deleted once read."""
        out = self.etl.with_suffix(".csv")
        subprocess.run(["tracerpt", str(self.etl), "-o", str(out), "-of", "CSV", "-y"], capture_output=True,
                       text=True, timeout=600, creationflags=BELOW_NORMAL)
        require(out.is_file(), "tracerpt wrote no CSV")
        kinds, times = {}, []
        with open(out, encoding="utf-8", errors="replace", newline="") as handle:
            reader = csv.reader(handle, skipinitialspace=True)
            header = [h.strip() for h in next(reader)]
            at = {name: header.index(name) for name in ("Event Name", "Type", "Event ID", "PID", "Clock-Time")}
            for row in reader:
                if len(row) <= max(at.values()):
                    continue
                try:
                    if int(row[at["PID"]].strip(), 16) not in pids:
                        continue
                    clock = int(row[at["Clock-Time"]].strip())
                except ValueError:
                    continue
                kind = f"{row[at['Event Name']].strip()}/{row[at['Type']].strip()}/{row[at['Event ID']].strip()}"
                kinds[kind] = kinds.get(kind, 0) + 1
                if row[at["Event ID"]].strip() == PRESENT_EVENT and first <= clock <= last:
                    times.append(clock)
        out.unlink()
        self.etl.unlink()
        return kinds, sorted(times)


def interval_stats(times):
    """Frame rate and p50/p95/p99 frame time (ms) from Present start times in 100 ns units."""
    if len(times) < 3:
        return None
    gaps = sorted((b - a) / 10000.0 for a, b in zip(times, times[1:]))

    def percentile(share):
        return round(gaps[min(len(gaps) - 1, int(share * len(gaps)))], 2)

    seconds = (times[-1] - times[0]) / 1e7
    return dict(frames=len(gaps), seconds=round(seconds, 2), fps=round(len(gaps) / seconds, 2),
                p50Ms=percentile(0.50), p95Ms=percentile(0.95), p99Ms=percentile(0.99),
                framesOver100Ms=sum(1 for g in gaps if g >= 100.0), longestMs=round(gaps[-1], 1))


def run_arm(output, name, arm, spec, args, stamp):
    seconds, (first, last) = spec["seconds"], spec["window"]
    actions = ",".join(spec["acts"] + [f"{last + 1}:va=regions"])
    command = [sys.executable, "-B", str(PROBE), "--output", str(output), "--instance-id", stamp,
               "--map", str(spec["path"]), "--seconds", str(seconds), "--timeout-seconds", str(seconds + 150),
               "--dump-every", "100000", "--actions", actions]
    if arm == "vanilla":
        command += ["--vanilla", "--opengl"]
    else:
        command += ["--proxy", str(args.front), "--host", str(args.host), "--product-dll", str(args.product_dll),
                    "--profile", "product-async-delta"]
    trace = PresentTrace(f"WarVK_Compare_{stamp}", output.parent / f"{output.name}.etl")
    row = dict(map=name, arm=arm, output=output.name, window=[first, last])
    load = LoadWatch()
    began = time.monotonic()
    try:
        row["trace"] = trace.start()
        require(row["trace"]["started"], "logman could not start the ETW session")
        with open(output.with_suffix(".log"), "w", encoding="utf-8", errors="replace") as out:
            row["exit"] = subprocess.run(command, cwd=ROOT, stdout=out, stderr=subprocess.STDOUT,
                                         creationflags=BELOW_NORMAL).returncode
    finally:
        row.setdefault("trace", {}).update(trace.stop())
    row.update(wallSeconds=round(time.monotonic() - began, 1), load=load.finish())
    receipt_path = output / "receipt.json"
    if not receipt_path.is_file():
        row["error"] = "no receipt"
        return row
    receipt = json.loads(receipt_path.read_text("utf-8"))
    observer = receipt.get("observer") or {}
    failed = [k for k, v in (receipt.get("checks") or {}).items() if not v and k != "frameNotFlat"]
    row.update(probeOk=not failed and not receipt.get("errors"), failedChecks=failed, errors=receipt.get("errors"))
    samples = observer.get("samples") or []
    va = observer.get("vaSample") or {}
    row["memory"] = dict(peakPrivateBytes=max((s["privateBytes"] for s in samples), default=None),
                         freeBytes=va.get("freeBytes"), largestHoleBytes=va.get("largestAlignedFreeHoleBytes"))
    scan = next((a for a in observer.get("actions") or [] if a.get("action") == "va=regions" and a.get("posted")), None)
    if scan and scan.get("low"):
        row["memory"]["scanLowUsableFreeBytes"] = scan["low"].get("usableFreeBytes")
    origin = observer.get("beganUnixSeconds")
    pid = (observer.get("processIdentity") or {}).get("pid")
    pids = [pid] if arm == "vanilla" else (observer.get("hostPids") or [])
    row["presentingPids"] = pids
    if origin is None or not pids or None in pids or not trace.etl.is_file():
        row["pace"] = None
        row["paceError"] = "no start time, presenting process or trace"
        return row
    row["etlBytes"] = trace.etl.stat().st_size
    zero = int((origin + FILETIME_UNIX) * 1e7)
    kinds, times = trace.presents(set(int(p) for p in pids), zero + int(first * 1e7), zero + int(last * 1e7))
    row["eventKinds"] = kinds
    row["pace"] = interval_stats(times)
    (output / "presents.json").write_text(json.dumps([round((t - zero) / 1e7, 5) for t in times]), encoding="utf-8")
    if arm == "warvk":
        plane = observer.get("controlPlane") or {}
        ready = plane.get("readyAtSeconds")
        series = [r for r in plane.get("frameSeries") or [] if r[1] is not None and r[2] is not None and r[3]]
        inside = [r for r in series if first <= r[0] <= last]
        if len(inside) >= 2 and inside[-1][1] > inside[0][1]:
            # The product's own counter over the same window: a cross-check of the outside count.
            frames, milliseconds = inside[-1][2] - inside[0][2], inside[-1][1] - inside[0][1]
            row["productFps"] = round(frames * 1000.0 / milliseconds, 2)
        histogram = frame_times(plane.get("framePaceSettled"), plane.get("framePaceEnd")) or {}
        row["productFrameTimes"] = {k: histogram.get(k) for k in ("p50Ms", "p95Ms", "p99Ms", "framesOver100Ms",
                                                                    "seconds")}
        row["readyAtSeconds"] = ready
        row["readyInTime"] = ready is not None and ready + SETTLE_SECONDS <= first
        row["backBuffer"] = [(plane.get("capture") or {}).get(k) for k in ("width", "height")]
    return row


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--product-dll", required=True, type=Path, help="the release's x86 d3d9.dll")
    parser.add_argument("--front", required=True, type=Path, help="the release's WarVK/x64/warvk-d3d9-front.dll")
    parser.add_argument("--host", required=True, type=Path, help="the release's WarVK/x64/warvk-d3d9-host.exe")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--maps", default="losttemple,echoisles,shengyusi")
    parser.add_argument("--rounds", type=int, default=2)
    args = parser.parse_args(argv)
    output = args.output.resolve()
    receipt = dict(schema="warvk-v130-vanilla-compare/v1", ok=False, runs=[], maps={}, checks={}, errors=[],
                   desktopRefusals=[], machine=None)
    try:
        require(ARTIFACT_ROOT in output.parents and not output.exists(), "fresh output under AutoTest/artifacts")
        names = [n for n in args.maps.split(",") if n]
        require(names and all(n in MAPS for n in names) and 1 <= args.rounds <= 4, "maps / rounds")
        for path in (args.product_dll, args.front, args.host, *(MAPS[n]["path"] for n in names)):
            require(Path(path).is_file(), f"missing {path}")
        output.mkdir(parents=True)
        receipt["machine"] = machine()
        serial = 0
        for name in names:
            for round_index in range(args.rounds):
                order = ARMS if round_index % 2 == 0 else ARMS[::-1]
                for arm in order:
                    for attempt in range(1, RUN_ATTEMPTS + 1):
                        serial += 1
                        stamp = f"v1{time.strftime('%H%M%S')}{serial:02d}"
                        waited = wait_for_input_desktop()
                        folder = f"{name}-r{round_index + 1}-{arm}" + ("" if attempt == 1 else f"-try{attempt}")
                        row = run_arm(output / folder, name, arm, MAPS[name], args, stamp)
                        row.update(round=round_index + 1, attempt=attempt, desktopWaitSeconds=waited)
                        if not desktop_refused(row) or attempt == RUN_ATTEMPTS:
                            break
                        receipt["desktopRefusals"].append({k: row.get(k) for k in
                            ("map", "arm", "round", "attempt", "output", "wallSeconds", "errors")})
                    receipt["runs"].append(row)
                    print(json.dumps({k: row.get(k) for k in ("map", "arm", "round", "exit", "probeOk", "failedChecks",
                                                               "pace", "productFps", "memory", "readyAtSeconds",
                                                               "load")},
                                     default=str), flush=True)
                    (output / "receipt.json").write_text(json.dumps(receipt, ensure_ascii=False, indent=2, default=str),
                                                         encoding="utf-8")
        checks = receipt["checks"]
        checks["allRunsPassed"] = all(r.get("probeOk") for r in receipt["runs"])
        checks["warvkReadyInTime"] = all(r.get("readyInTime") for r in receipt["runs"] if r["arm"] == "warvk")
        for name in names:
            summary = {}
            for arm in ARMS:
                runs = [r for r in receipt["runs"] if r["map"] == name and r["arm"] == arm and r.get("probeOk")
                        and r.get("pace")]
                summary[arm] = dict(runs=len(runs),
                    fps=median([r["pace"]["fps"] for r in runs]),
                    p50Ms=median([r["pace"].get("p50Ms") for r in runs]),
                    p95Ms=median([r["pace"].get("p95Ms") for r in runs]),
                    p99Ms=median([r["pace"].get("p99Ms") for r in runs]),
                    freeBytes=median([r["memory"]["freeBytes"] for r in runs]),
                    largestHoleBytes=median([r["memory"]["largestHoleBytes"] for r in runs]),
                    peakPrivateBytes=median([r["memory"]["peakPrivateBytes"] for r in runs]),
                    busyCoresMedian=median([r["load"].get("cpuBusyCoresMedian") for r in runs]))
            a, b = summary["vanilla"], summary["warvk"]
            if a["fps"] and b["fps"]:
                summary["fpsRatio"] = round(b["fps"] / a["fps"], 3)
            checks[f"{name}:measured"] = a["runs"] == args.rounds and b["runs"] == args.rounds
            receipt["maps"][name] = summary
        receipt["ok"] = all(checks.values())
    except Exception as error:
        receipt["errors"].append(f"{type(error).__name__}: {error}")
    if output.is_dir():
        (output / "receipt.json").write_text(json.dumps(receipt, ensure_ascii=False, indent=2, default=str),
                                             encoding="utf-8")
    print(json.dumps(dict(ok=receipt["ok"], checks=receipt["checks"], errors=receipt["errors"], maps=receipt["maps"]),
                     ensure_ascii=False, indent=2, default=str))
    return 0 if receipt["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
