#!/usr/bin/env python3
"""Phase 5.2: the same game, in process and in proxy mode, side by side.

One product x86 d3d9.dll runs each map twice per round: with the device in the
game process (as players have it), and in proxy mode with the x64 host. Both
arms are runs of run_x64_game_proxy_probe.py - same sandbox instance rules,
same isolated desktop, same timed acts - so the only difference is where the
device lives. Arms alternate, and the order flips each round, because the
machine is shared and its load drifts.

What is compared, per map:

  memory   the x86 process at the end of the run: free address space and its
           largest hole (native_game_va_sample), and the peak private bytes.
           The migration exists for this: proxy mode must leave more free
           address space and use fewer private bytes than in process.
  pace     frames per second from the product's own frame counter, in windows
           of the control plane's polls, after the game has settled. The
           threshold (median frame time at most 1.5 times in process) is the
           proposal of the migration plan; the author has not confirmed it.
  picture  captures at the same moments from the same camera: size, mean
           absolute difference and the share of pixels that differ visibly.
           The pictures are never identical (units move, weather is random);
           the numbers and the side-by-side sheets are for reading.

Nothing is deployed; the player's DLL is not touched (the probe checks it).
"""
from __future__ import annotations

import argparse
import ctypes
import json
import os
from pathlib import Path
import statistics
import struct
import subprocess
import sys
import threading
import time

from autotest_sessions import DEFAULT_SANDBOX_ROOT

ROOT = Path(__file__).resolve().parents[1]
ARTIFACT_ROOT = ROOT / "AutoTest" / "artifacts"
PROBE = ROOT / "AutoTest" / "run_x64_game_proxy_probe.py"
BELOW_NORMAL = 0x00004000
MAPS = {
    "losttemple": dict(path=DEFAULT_SANDBOX_ROOT / "Maps/(4)LostTemple.w3m", seconds=70, acts=[]),
    "echoisles": dict(path=DEFAULT_SANDBOX_ROOT / "Maps/FrozenThrone/(2)EchoIsles.w3x", seconds=70, acts=[]),
    # A large custom map: its own start dialog needs one key, and it loads for a minute.
    # One key at 72 s (x64-phase52-ab-20261117-r1) hits the loading screen when the shared
    # machine is busy and loading takes ~110 s, so the key repeats; in game, SPACE only
    # jumps the camera to the last alert.
    "shengyusi": dict(path=DEFAULT_SANDBOX_ROOT / "Maps/(4)生与死v1.28读档bug修复.w3x", seconds=140,
                      acts=["72:key=SPACE", "86:key=SPACE", "100:key=SPACE", "114:key=SPACE"]),
}
# Median frame time in proxy mode against in process (proposed, not confirmed by the author).
PACE_LIMIT = 1.5
SETTLE_SECONDS = 12.0
# freeBytes / largestHoleBytes are native_game_va_sample's low 2 GiB; the probe's
# va=regions covers 4 GiB and adds the high half as separate fields.
LOW_END = 0x80000000


def require(condition, message):
    if not condition:
        raise ValueError(message)


# The probe refuses to run while the input desktop cannot be opened (the workstation is locked, or the
# secure desktop is up): its desktop check then fails with DesktopSafetyError. That lasts seconds, so
# the gate waits for a quiet input desktop before each run and reruns a run the check refused. The
# check itself is the probe's and is not relaxed here.
DESKTOP_QUIET_SECONDS = 10.0
DESKTOP_WAIT_SECONDS = 900.0
RUN_ATTEMPTS = 3


def input_desktop_open():
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.OpenInputDesktop.restype = ctypes.c_void_p
    user32.OpenInputDesktop.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32)
    user32.CloseDesktop.argtypes = (ctypes.c_void_p,)
    handle = user32.OpenInputDesktop(0, False, 0x0001)  # DESKTOP_READOBJECTS
    if not handle:
        return False
    user32.CloseDesktop(handle)
    return True


def wait_for_input_desktop():
    """Seconds waited until the input desktop stayed open for DESKTOP_QUIET_SECONDS; None if it never did."""
    began = time.monotonic()
    quiet_since = None
    while time.monotonic() - began < DESKTOP_WAIT_SECONDS:
        if input_desktop_open():
            quiet_since = quiet_since if quiet_since is not None else time.monotonic()
            if time.monotonic() - quiet_since >= DESKTOP_QUIET_SECONDS:
                return round(time.monotonic() - began - DESKTOP_QUIET_SECONDS, 1)
        else:
            quiet_since = None
        time.sleep(0.5)
    return None


def desktop_refused(row):
    return any("DesktopSafetyError" in str(error) for error in row.get("errors") or [])


# This machine is shared: other sessions' work shows up in the frame times. Each run records what the
# machine was doing, so a run that was measured under load can be told from one that was not. The CPU
# figure before the launch is other people's work alone; the figures during the run include the game
# (and the host, in the proxy arm).
LOAD_SAMPLE_SECONDS = 5.0


def system_times():
    idle, kernel, user = ctypes.c_uint64(), ctypes.c_uint64(), ctypes.c_uint64()
    ctypes.WinDLL("kernel32").GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user))
    return idle.value, kernel.value + user.value  # kernel time includes idle time


def busy_cores(before, after):
    total = after[1] - before[1]
    if total <= 0:
        return None
    return round((1.0 - (after[0] - before[0]) / total) * (os.cpu_count() or 1), 2)


def gpu_load():
    """The GPU's utilisation, power draw and memory in use right now; None without nvidia-smi."""
    try:
        line = subprocess.run(["nvidia-smi", "--query-gpu=utilization.gpu,power.draw,memory.used",
                               "--format=csv,noheader,nounits"], capture_output=True, text=True, timeout=10,
                              creationflags=BELOW_NORMAL).stdout.strip().splitlines()[0]
        utilization, power, memory = (float(value) for value in line.split(","))
        return dict(utilizationPercent=utilization, powerWatts=power, memoryMiB=memory)
    except (OSError, ValueError, IndexError, subprocess.SubprocessError):
        return None


class LoadWatch:
    """System-wide busy cores, sampled every LOAD_SAMPLE_SECONDS while a run lasts."""

    def __init__(self):
        first = system_times()
        time.sleep(2.0)
        self.before = busy_cores(first, system_times())
        self.gpu = gpu_load()
        self.samples = []
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.sample, daemon=True)
        self.thread.start()

    def sample(self):
        last = system_times()
        while not self.stop.wait(LOAD_SAMPLE_SECONDS):
            now = system_times()
            self.samples.append(busy_cores(last, now))
            last = now

    def finish(self):
        self.stop.set()
        self.thread.join()
        samples = [s for s in self.samples if s is not None]
        return dict(logicalProcessors=os.cpu_count(), cpuBusyCoresBefore=self.before, gpuBefore=self.gpu,
                    cpuBusyCoresMedian=round(statistics.median(samples), 2) if samples else None,
                    cpuBusyCoresMax=max(samples) if samples else None, samples=len(samples))


def run_arm(output, name, arm, spec, args, stamp):
    """One probe run; returns its receipt (or a failure row)."""
    seconds = spec["seconds"]
    captures = [f"{seconds - 16}:capture=ab1", f"{seconds - 6}:capture=ab2", f"{seconds - 3}:va=regions"]
    actions = ",".join(spec["acts"] + captures)
    command = [sys.executable, "-B", str(PROBE), "--proxy", str(args.proxy), "--host", str(args.host),
               "--product-dll", str(args.product_dll), "--output", str(output), "--instance-id", stamp,
               "--profile", args.profile, "--map", str(spec["path"]), "--seconds", str(seconds),
               "--timeout-seconds", str(seconds + 150), "--dump-every", "100000", "--actions", actions]
    if arm == "inprocess":
        command.append("--in-process")
    log = output.with_suffix(".log")
    load = LoadWatch()
    began = time.monotonic()
    with open(log, "w", encoding="utf-8", errors="replace") as out:
        code = subprocess.run(command, cwd=ROOT, stdout=out, stderr=subprocess.STDOUT,
                              creationflags=BELOW_NORMAL).returncode
    receipt_path = output / "receipt.json"
    row = dict(map=name, arm=arm, output=output.name, exit=code, wallSeconds=round(time.monotonic() - began, 1),
               load=load.finish())
    if not receipt_path.is_file():
        row["error"] = "no receipt"
        return row
    receipt = json.loads(receipt_path.read_text("utf-8"))
    observer = receipt.get("observer") or {}
    # Frame 0 is the only dump at this interval, and it is blank by design.
    failed = [k for k, v in (receipt.get("checks") or {}).items() if not v and k != "frameNotFlat"]
    row.update(probeOk=not failed and not receipt.get("errors"), failedChecks=failed, errors=receipt.get("errors"))
    samples = observer.get("samples") or []
    va = observer.get("vaSample") or {}
    row["memory"] = dict(
        peakPrivateBytes=max((s["privateBytes"] for s in samples), default=None),
        lastPrivateBytes=samples[-1]["privateBytes"] if samples else None,
        freeBytes=va.get("freeBytes"), largestHoleBytes=va.get("largestAlignedFreeHoleBytes"))
    plane = observer.get("controlPlane") or {}
    series = [r for r in plane.get("frameSeries") or [] if r[1] is not None and r[2] is not None and r[3]]
    ready = plane.get("readyAtSeconds")
    windows = []
    for before, after in zip(series, series[1:]):
        if ready is None or before[0] < ready + SETTLE_SECONDS:
            continue
        milliseconds, frames = after[1] - before[1], after[2] - before[2]
        if milliseconds > 0 and frames > 0:
            windows.append(round(frames * 1000.0 / milliseconds, 2))
    row["pace"] = dict(readyAtSeconds=ready, windows=len(windows),
        medianFps=round(statistics.median(windows), 2) if windows else None,
        slowestFps=min(windows) if windows else None, fastestFps=max(windows) if windows else None)
    row["pace"]["frameTimes"] = frame_times(plane.get("framePaceSettled"), plane.get("framePaceEnd"))
    # Static TLS: the loader gives every thread of the process one copy of the
    # product DLL's TLS template, as one process-heap block of a known size.
    block = (args.tls_template_bytes + 0x20 + 0xFFF) & ~0xFFF
    regions = sorted(output.glob("va-regions-*.json"))
    if regions:
        allocations = json.loads(regions[-1].read_text("utf-8"))
        # The probe's va=regions scans all 4 GiB now; these totals keep their
        # low-2-GiB meaning (Storm's budget), the high half is reported apart.
        low = [a for a in allocations if a["base"] < LOW_END]
        high = [a for a in allocations if a["base"] >= LOW_END]
        row["memory"].update(
            privateCommittedBytes=sum(a["committed"] for a in low if a["type"] == "private"),
            imageBytes=sum(a["bytes"] for a in low if a["type"] == "image"),
            highPrivateCommittedBytes=sum(a["committed"] for a in high if a["type"] == "private"),
            highAllocatedBytes=sum(a["bytes"] for a in high))
        if args.tls_template_bytes:
            copies = sum(1 for a in allocations if a["type"] == "private" and a["committed"] == block)
            row["memory"].update(staticTlsCopies=copies, staticTlsBytes=copies * block)
    # freeBytes / largestHoleBytes above stay native_game_va_sample's (low 2 GiB,
    # raw MEM_FREE bytes, at the close) so rounds compare like with like. The
    # va=regions scan (a few seconds earlier) adds both halves, raw and usable.
    scan = next((a for a in observer.get("actions") or [] if a.get("action") == "va=regions" and a.get("posted")), None)
    if scan and scan.get("high"):
        row["memory"].update(
            scanLowFreeBytes=scan["low"]["freeBytes"], scanLowUsableFreeBytes=scan["low"].get("usableFreeBytes"),
            highFreeBytes=scan["high"]["freeBytes"], highUsableFreeBytes=scan["high"].get("usableFreeBytes"),
            highLargestHoleBytes=scan["high"]["largestAlignedFreeHoleBytes"])
    row["captures"] = {n: str(output / f"{n}.bmp") for n in ("ab1", "ab2") if (output / f"{n}.bmp").is_file()}
    back = (plane.get("capture") or {})
    row["backBuffer"] = [back.get("width"), back.get("height")]
    return row


def frame_times(first, last):
    """p50/p95/p99 frame time (ms) between two snapshots of the product's cumulative frame-time histogram
    (get_runtime_status framePace: 0.25 ms buckets below 100 ms, longer frames counted apart)."""
    if not first or not last or last["frames"] <= first["frames"]:
        return None
    width = last["bucketUs"]
    before = dict(first["buckets"])
    counts = sorted((index, count - before.get(index, 0)) for index, count in last["buckets"])
    over = last["over"] - first["over"]
    total = sum(count for _, count in counts) + over

    def percentile(share):
        need, seen = share * total, 0
        for index, count in counts:
            seen += count
            if seen >= need:
                return round((index + 1) * width / 1000.0, 2)  # the bucket's upper edge
        return None  # beyond the histogram: among the frames of 100 ms and longer

    return dict(frames=total, seconds=round(last["atSeconds"] - first["atSeconds"], 2),
                p50Ms=percentile(0.50), p95Ms=percentile(0.95), p99Ms=percentile(0.99),
                framesOver100Ms=over, overMs=round((last["overUs"] - first["overUs"]) / 1000.0, 1),
                maxMsSinceStart=round(last["maxUs"] / 1000.0, 1))


def compare_pictures(a, b, sheet):
    from PIL import Image, ImageChops, ImageStat
    first, second = (Image.open(p).convert("RGB") for p in (a, b))
    result = dict(sizes=[list(first.size), list(second.size)])
    if first.size != second.size:
        return result
    difference = ImageChops.difference(first, second)
    result["meanAbs"] = round(sum(ImageStat.Stat(difference).mean) / 3, 3)
    result["over8Share"] = round(sum(difference.convert("L").histogram()[9:]) / (first.size[0] * first.size[1]), 4)
    scale = max(1, first.size[0] // 960)
    small = [im.resize((im.size[0] // scale, im.size[1] // scale)) for im in (first, second)]
    page = Image.new("RGB", (small[0].size[0], small[0].size[1] * 2 + 4), "white")
    page.paste(small[0], (0, 0))
    page.paste(small[1], (0, small[0].size[1] + 4))
    page.save(sheet)
    result["sheet"] = sheet.name
    return result


def median(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def tls_template_bytes(dll):
    """Size of a 32-bit PE image's TLS template (raw data plus zero fill), 0 if it has none."""
    data = Path(dll).read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    sections, optional_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    require(struct.unpack_from("<H", data, optional)[0] == 0x10B, "32-bit product DLL")
    directory = struct.unpack_from("<I", data, optional + 96 + 9 * 8)[0]
    if not directory:
        return 0
    for index in range(sections):
        at = optional + optional_size + index * 40
        size, address, raw_size, raw = struct.unpack_from("<IIII", data, at + 8)
        if address <= directory < address + max(size, raw_size):
            start, end, _, _, zero = struct.unpack_from("<IIIII", data, raw + directory - address)
            return end - start + zero
    raise ValueError("TLS directory outside the sections")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--proxy", required=True, type=Path, help="x86 front end DLL (d3d9_proxy.cpp)")
    parser.add_argument("--host", required=True, type=Path, help="x64 warvk-d3d9-host-dev.exe")
    parser.add_argument("--product-dll", required=True, type=Path, help="x86 product d3d9.dll, both arms")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--maps", default="losttemple,echoisles,shengyusi")
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--profile", choices=("product", "product-async", "product-async-delta", "product-async-resources"), default="product",
                        help="probe profile of the proxy arm (run_x64_game_proxy_probe.py PROFILES)")
    args = parser.parse_args(argv)
    output = args.output.resolve()
    receipt = dict(schema="warvk-x64-phase52-ab/v1", ok=False, runs=[], maps={}, checks={}, errors=[], desktopRefusals=[],
                   paceLimit=PACE_LIMIT, paceLimitConfirmedByAuthor=False, profile=args.profile)
    try:
        require(ARTIFACT_ROOT in output.parents and not output.exists(), "fresh output under AutoTest/artifacts")
        names = [n for n in args.maps.split(",") if n]
        require(names and all(n in MAPS for n in names) and 1 <= args.rounds <= 4, "maps / rounds")
        for path in (args.proxy, args.host, args.product_dll, *(MAPS[n]["path"] for n in names)):
            require(Path(path).is_file(), f"missing {path}")
        args.tls_template_bytes = receipt["productTlsTemplateBytes"] = tls_template_bytes(args.product_dll)
        output.mkdir(parents=True)
        serial = 0
        for name in names:
            for round_index in range(args.rounds):
                order = ("inprocess", "proxy") if round_index % 2 == 0 else ("proxy", "inprocess")
                for arm in order:
                    for attempt in range(1, RUN_ATTEMPTS + 1):
                        serial += 1
                        stamp = f"52{time.strftime('%H%M%S')}{serial:02d}"
                        waited = wait_for_input_desktop()
                        folder = f"{name}-r{round_index + 1}-{arm}" + ("" if attempt == 1 else f"-try{attempt}")
                        row = run_arm(output / folder, name, arm, MAPS[name], args, stamp)
                        row.update(round=round_index + 1, attempt=attempt, desktopWaitSeconds=waited)
                        if not desktop_refused(row) or attempt == RUN_ATTEMPTS:
                            break
                        # Kept on disk and listed apart: a refused run measured nothing.
                        receipt["desktopRefusals"].append({k: row.get(k) for k in
                            ("map", "arm", "round", "attempt", "output", "wallSeconds", "errors", "desktopWaitSeconds")})
                    receipt["runs"].append(row)
                    print(json.dumps({k: row.get(k) for k in ("map", "arm", "round", "exit", "probeOk", "failedChecks",
                                                               "memory", "pace", "load")}, default=str), flush=True)
                    (output / "receipt.json").write_text(json.dumps(receipt, ensure_ascii=False, indent=2, default=str),
                                                         encoding="utf-8")
        checks = receipt["checks"]
        checks["allRunsPassed"] = all(r.get("probeOk") for r in receipt["runs"])
        for name in names:
            rows = {arm: [r for r in receipt["runs"] if r["map"] == name and r["arm"] == arm and r.get("probeOk")]
                    for arm in ("inprocess", "proxy")}
            summary = {}
            for arm, runs in rows.items():
                summary[arm] = dict(runs=len(runs),
                    freeBytes=median([r["memory"]["freeBytes"] for r in runs]),
                    largestHoleBytes=median([r["memory"]["largestHoleBytes"] for r in runs]),
                    peakPrivateBytes=median([r["memory"]["peakPrivateBytes"] for r in runs]),
                    staticTlsBytes=median([r["memory"].get("staticTlsBytes") for r in runs]),
                    highFreeBytes=median([r["memory"].get("highFreeBytes") for r in runs]),
                    medianFps=median([r["pace"]["medianFps"] for r in runs]),
                    # The product's own frame times (median over the rounds of each percentile).
                    p50Ms=median([(r["pace"].get("frameTimes") or {}).get("p50Ms") for r in runs]),
                    p95Ms=median([(r["pace"].get("frameTimes") or {}).get("p95Ms") for r in runs]),
                    p99Ms=median([(r["pace"].get("frameTimes") or {}).get("p99Ms") for r in runs]),
                    framesOver100Ms=sum((r["pace"].get("frameTimes") or {}).get("framesOver100Ms") or 0 for r in runs),
                    slowestFps=min([r["pace"]["slowestFps"] for r in runs if r["pace"]["slowestFps"]], default=None),
                    backBuffer=runs[0]["backBuffer"] if runs else None)
            a, b = summary["inprocess"], summary["proxy"]
            complete = all(v is not None for s in (a, b) for k, v in s.items()
                           if k not in ("staticTlsBytes", "highFreeBytes", "p50Ms", "p95Ms", "p99Ms"))
            if complete:
                summary["memoryRelief"] = dict(
                    freeBytesGained=b["freeBytes"] - a["freeBytes"],
                    largestHoleGained=b["largestHoleBytes"] - a["largestHoleBytes"],
                    peakPrivateSaved=a["peakPrivateBytes"] - b["peakPrivateBytes"])
                summary["frameTimeRatio"] = round(a["medianFps"] / b["medianFps"], 3)
                # Proxy over in process, from the product's own frame times (reported, not gated).
                for key in ("p50Ms", "p95Ms", "p99Ms"):
                    if a.get(key) and b.get(key):
                        summary[key.replace("Ms", "Ratio")] = round(b[key] / a[key], 3)
                checks[f"{name}:moreFreeAddressSpace"] = b["freeBytes"] > a["freeBytes"]
                checks[f"{name}:largerLargestHole"] = b["largestHoleBytes"] > a["largestHoleBytes"]
                checks[f"{name}:fewerPrivateBytes"] = b["peakPrivateBytes"] < a["peakPrivateBytes"]
                checks[f"{name}:sameBackBuffer"] = a["backBuffer"] == b["backBuffer"]
                checks[f"{name}:paceWithinLimit"] = summary["frameTimeRatio"] <= PACE_LIMIT
            else:
                checks[f"{name}:measured"] = False
            pictures = []
            for round_index in range(args.rounds):
                pair = {r["arm"]: r for r in receipt["runs"] if r["map"] == name and r["round"] == round_index + 1}
                for capture in ("ab1", "ab2"):
                    paths = [pair.get(arm, {}).get("captures", {}).get(capture) for arm in ("proxy", "inprocess")]
                    if all(paths):
                        row = compare_pictures(paths[0], paths[1], output / f"{name}-r{round_index + 1}-{capture}.png")
                        row.update(round=round_index + 1, capture=capture, top="proxy", bottom="inprocess")
                        pictures.append(row)
            summary["pictures"] = pictures
            checks[f"{name}:picturesCompared"] = bool(pictures) and all(
                p["sizes"][0] == p["sizes"][1] and "meanAbs" in p for p in pictures)
            receipt["maps"][name] = summary
        receipt["ok"] = all(checks.values())
    except Exception as error:
        receipt["errors"].append(f"{type(error).__name__}: {error}")
    if output.is_dir():
        (output / "receipt.json").write_text(json.dumps(receipt, ensure_ascii=False, indent=2, default=str), encoding="utf-8")
    print(json.dumps(dict(ok=receipt["ok"], checks=receipt["checks"], errors=receipt["errors"],
        maps={n: {k: v for k, v in s.items() if k != "pictures"} | {"pictures": [
            (p["round"], p["capture"], p.get("meanAbs"), p.get("over8Share")) for p in s.get("pictures", [])]}
              for n, s in receipt["maps"].items()}), ensure_ascii=False, indent=2, default=str))
    return 0 if receipt["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
