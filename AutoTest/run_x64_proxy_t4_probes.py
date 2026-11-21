"""T4 development probes for proxy mode: unit outline, off-screen shadows, map switch.

Not a gate. Each probe is one run of run_x64_game_proxy_probe.py per mode (the
product DLL in process with --in-process, and in proxy mode), driven by an
actions file this script writes; then this script compares the two runs.

  actions outline|sweep|extramap  -> prints the probe's --actions-file content
  analyze outline  <in-process run> <proxy run> <output folder>
  analyze sweep    <in-process run> <proxy run> <output folder>
  analyze extramap <in-process run> <proxy run> <output folder>

outline  : game paused; capture, register the first 1024 JASS handles for
           the outline (outline.pick rangeCount, showVisible on), capture,
           clear, capture again. The yellow pixels that appear are the
           outline; both modes at the same (start) camera. Status samples
           before and after record the share of unit casters with a non-zero
           batch handle (the outline matches only those).
sweep    : game paused, full-map visibility, low locked sun; the camera moves
           away from the start in fixed steps (+x, then -x). At each step one
           capture with shadows on and one with them off (csm.setEnabled); the
           pixels the shadows darken are counted per step: a curve per mode.
extramap : start on the cross-map gate's map A with A and B also staged at
           the paths their own ChangeLevel uses: A -> B -> A in one process.
           Status every 3 s (session generation, world frames), captures every
           10 s; the transport accounts at exit.
"""
from __future__ import annotations

import json
from pathlib import Path
import struct
import sys
import zlib

STEP = 250.0            # world units per sweep step
STEPS = 11              # 0..10 per direction
PAUSE_ENV = "DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE=0"


def actions(kind: str) -> list[str]:
    rows = []
    if kind == "outline":
        # The render object registry stays empty in the product (2026-11-17
        # r1/r2: outline.pick by kind found no candidate in either mode), so
        # the first 1024 JASS handles are registered instead: the same set in
        # both modes, as a map's script would register them. The window starts
        # at 0x100001: 0x100000 is handle id 0, which NormalizeHandle refuses
        # (r3 registered 1023 of 1024 and the pick reported a failure).
        rows += ["25:status=s0", "27:test=game.pause", "30:capture=off1",
                 "32:test=outline.pick:rangeLo=1048577;rangeCount=1024;showVisible=true;widthPx=3",
                 "36:capture=on1", "37:status=s1", "39:test=outline.status", "41:test=outline.clear",
                 "44:capture=off2", "45:status=s2"]
    elif kind == "sweep":
        rows += ["22:test=game.pause", "23:test=visibility.full_map:enabled=true",
                 "24:test=shadow.lock_sun:enabled=true;time01=0.3", "25:status=start"]
        at = 27.0
        for sign, label in ((1, "p"), (-1, "m")):
            for step in range(STEPS):
                rows += [f"{at:g}:camrel={sign * step * STEP:g};0",
                         f"{at + 1.2:g}:capture={label}{step}on",
                         f"{at + 1.4:g}:status={label}{step}",
                         f"{at + 1.6:g}:api=csm.setEnabled;b:0",
                         f"{at + 2.8:g}:capture={label}{step}off",
                         f"{at + 3.0:g}:api=csm.setEnabled;b:1"]
                at += 3.4
    elif kind == "extramap":
        rows += [f"{t}:status=e{t}" for t in range(15, 115, 3)]
        rows += [f"{t + 1}:capture=e{t}" for t in range(20, 115, 10)]
    else:
        raise SystemExit(f"unknown probe {kind}")
    return rows


def read_bmp(path: Path):
    data = path.read_bytes()
    offset, width, height, bits = (struct.unpack_from("<I", data, 10)[0], struct.unpack_from("<i", data, 18)[0],
                                   struct.unpack_from("<i", data, 22)[0], struct.unpack_from("<H", data, 28)[0])
    if bits not in (24, 32):
        raise ValueError(f"{path}: {bits}-bit BMP")
    step = bits // 8
    stride = (width * step + 3) & ~3
    rows = []
    for y in range(abs(height)):
        source = (abs(height) - 1 - y) if height > 0 else y
        line = data[offset + source * stride: offset + source * stride + width * step]
        rows.append(line if step == 3 else bytes(b for i, b in enumerate(line) if i % 4 != 3))
    return width, abs(height), rows  # rows of BGR bytes, top first


def write_png(path: Path, width: int, height: int, rgb_rows) -> None:
    raw = b"".join(b"\0" + bytes(row) for row in rgb_rows)

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def yellow(b, g, r) -> bool:
    return r >= 190 and g >= 190 and b <= 120


def outline_mask(off: Path, on: Path):
    w, h, a = read_bmp(off)
    w2, h2, b = read_bmp(on)
    if (w, h) != (w2, h2):
        raise ValueError("capture sizes differ")
    mask = set()
    for y in range(h):
        ra, rb = a[y], b[y]
        for x in range(0, w * 3, 3):
            if yellow(rb[x], rb[x + 1], rb[x + 2]) and not yellow(ra[x], ra[x + 1], ra[x + 2]):
                mask.add((x // 3, y))
    return w, h, mask, b


def mask_png(path: Path, w: int, h: int, mask, base_rows) -> None:
    rows = []
    for y in range(h):
        src = base_rows[y]
        row = bytearray(w * 3)
        for x in range(w):
            if (x, y) in mask:
                row[x * 3:x * 3 + 3] = b"\xff\xff\x00"
            else:
                row[x * 3:x * 3 + 3] = bytes((src[x * 3 + 2] // 3, src[x * 3 + 1] // 3, src[x * 3] // 3))
        rows.append(row)
    write_png(path, w, h, rows)


def bbox(mask):
    if not mask:
        return None
    xs, ys = [p[0] for p in mask], [p[1] for p in mask]
    return [min(xs), min(ys), max(xs), max(ys)]


def status(run: Path, name: str) -> dict:
    path = run / f"status-{name}.json"
    return json.loads(path.read_text("utf-8")) if path.is_file() else {}


def receipt_actions(run: Path) -> list:
    data = json.loads((run / "receipt.json").read_text("utf-8"))
    return (data.get("observer") or {}).get("actions") or []


def analyze_outline(runs: dict, out: Path) -> dict:
    result = {}
    masks = {}
    for mode, run in runs.items():
        row = {"handles": {}}
        for name in ("s0", "s1", "s2"):
            casters = (status(run, name).get("proxyMode") or {}).get("casters") or {}
            last = casters.get("lastFrame") or {}
            row["handles"][name] = {k: casters.get(k) for k in ("sent", "units", "withHandle", "unitsWithHandle",
                                                                "outlineMatched")}
            row["handles"][name]["lastFrame"] = last
            if casters.get("units"):
                row["handles"][name]["unitHandleShare"] = round(casters["unitsWithHandle"] / casters["units"], 4)
            if last.get("units"):
                row["handles"][name]["lastFrameUnitHandleShare"] = round(last["unitsWithHandle"] / last["units"], 4)
        # Proxy mode: the host's shadow runs that drew the outline pass
        # (ShadowRunFacts.outline = receiverNeedOutlinePass), from its exit account.
        for log in sorted(run.glob("host.log*")):
            for line in log.read_text("utf-8", errors="replace").splitlines():
                if line.startswith("D3D9 shadow-host: {"):
                    account = json.loads(line.split(": ", 1)[1])
                    row["hostShadow"] = {"log": log.name, "runs": account.get("runs"),
                                         "lastOutline": (account.get("last") or {}).get("outline"),
                                         "runLogWithOutline": sum(1 for r in account.get("runLog") or []
                                                                  if r.get("outline")),
                                         "runLogLength": len(account.get("runLog") or [])}
        row["picks"] = [dict(at=a["seconds"], action=a["action"][:60], posted=a.get("posted"), error=a.get("error"),
                             picked=(a.get("result") or {}).get("picked"), rawcode=(a.get("result") or {}).get("rawcode"),
                             candidates=(a.get("result") or {}).get("candidateCount"),
                             rangeCount=(a.get("result") or {}).get("rangeCount"),
                             handleCount=(a.get("result") or {}).get("handleCount"))
                        for a in receipt_actions(run) if "outline.pick" in a["action"]]
        for label, off, on in (("range", "off1", "on1"), ("cleared", "off1", "off2")):
            if not (run / f"{off}.bmp").is_file() or not (run / f"{on}.bmp").is_file():
                row[label] = {"captured": False}
                continue
            w, h, mask, base = outline_mask(run / f"{off}.bmp", run / f"{on}.bmp")
            masks[(mode, label)] = mask
            png = out / f"outline-{label}-{mode}.png"
            mask_png(png, w, h, mask, base)
            row[label] = {"captured": True, "yellowPixels": len(mask), "bbox": bbox(mask), "maskPng": png.name,
                          "size": [w, h]}
        result[mode] = row
    for label in ("range", "cleared"):
        a, b = masks.get(("inprocess", label)), masks.get(("proxy", label))
        if a is not None and b is not None:
            union = len(a | b)
            result[f"{label}IoU"] = round(len(a & b) / union, 4) if union else None
    return result


def shadow_count(off: Path, on: Path, strip: float) -> dict:
    """Pixels the shadows darken (on darker than off by > 12 in luma), whole
    frame and the left/right edge strips."""
    w, h, a = read_bmp(off)
    _, _, b = read_bmp(on)
    total = left = right = 0
    edge = int(w * strip)
    for y in range(int(h * 0.05), int(h * 0.72)):  # world area only, above the game's UI panel
        ra, rb = a[y], b[y]
        for x in range(0, w, 2):
            i = x * 3
            la = ra[i] * 0.114 + ra[i + 1] * 0.587 + ra[i + 2] * 0.299
            lb = rb[i] * 0.114 + rb[i + 1] * 0.587 + rb[i + 2] * 0.299
            if la - lb > 12:
                total += 1
                if x < edge:
                    left += 1
                elif x >= w - edge:
                    right += 1
    return {"shadowPixels": total * 2, "leftStrip": left * 2, "rightStrip": right * 2}


def analyze_sweep(runs: dict, out: Path) -> dict:
    result = {}
    for mode, run in runs.items():
        curve = {}
        for label in ("p", "m"):
            rows = []
            for step in range(STEPS):
                off, on = run / f"{label}{step}off.bmp", run / f"{label}{step}on.bmp"
                row = {"step": step, "offset": (1 if label == "p" else -1) * step * STEP}
                if off.is_file() and on.is_file():
                    row.update(shadow_count(off, on, 0.2))
                casters = (status(run, f"{label}{step}").get("proxyMode") or {}).get("casters") or {}
                if casters:
                    row["castersLastFrame"] = (casters.get("lastFrame") or {})
                shadow = status(run, f"{label}{step}").get("shadow") or {}
                for key in ("semanticSceneShadowCastersCount", "semanticSceneReplayDrawsCount",
                            "semanticSceneShadowMapDrawnCasters"):
                    if key in shadow:
                        row[key] = shadow[key]
                rows.append(row)
            curve[label] = rows
        result[mode] = curve
    chart(result, out / "sweep-curve.svg")
    return result


def chart(result: dict, path: Path) -> None:
    """Shadow pixels against camera offset, one line per mode and direction."""
    series = []
    for mode, curve in result.items():
        for label, rows in curve.items():
            series.append((f"{mode} {'+x' if label == 'p' else '-x'}",
                           [(r["offset"], r.get("shadowPixels")) for r in rows if r.get("shadowPixels") is not None]))
    peak = max([v for _, pts in series for _, v in pts] + [1])
    span = STEP * (STEPS - 1)
    colors = {"inprocess +x": "#2a6fdb", "inprocess -x": "#7aa6ef", "proxy +x": "#d9480f", "proxy -x": "#f4a06b"}
    lines = []
    for name, points in series:
        coords = " ".join(f"{60 + (x + span) / (2 * span) * 600:.1f},{340 - v / peak * 300:.1f}" for x, v in points)
        lines.append(f'<polyline fill="none" stroke="{colors.get(name, "#333")}" stroke-width="2" points="{coords}"/>')
    legend = "".join(f'<text x="70" y="{20 + 16 * i}" font-size="12" fill="{colors.get(n, "#333")}">{n}</text>'
                     for i, (n, _) in enumerate(series))
    path.write_text(f'<svg xmlns="http://www.w3.org/2000/svg" width="700" height="380">'
                    f'<rect width="700" height="380" fill="white"/>'
                    f'<line x1="60" y1="340" x2="660" y2="340" stroke="#999"/><line x1="60" y1="40" x2="60" y2="340" stroke="#999"/>'
                    f'<text x="300" y="370" font-size="12">camera offset (world units)</text>'
                    f'<text x="5" y="30" font-size="12">shadow px (max {peak})</text>{"".join(lines)}{legend}</svg>',
                    encoding="utf-8")


def analyze_extramap(runs: dict, out: Path) -> dict:
    result = {}
    for mode, run in runs.items():
        rows = []
        for path in sorted(run.glob("status-e*.json"), key=lambda p: int(p.stem[8:])):
            data = json.loads(path.read_text("utf-8"))
            frame = (data.get("proxyMode") or {}).get("frame") or {}
            shadow = data.get("shadow") or {}
            rows.append({"t": int(path.stem[8:]), "frameIndex": data.get("frameIndex"),
                         "isInGame": (data.get("render") or {}).get("isInGame"),
                         "sessionGeneration": frame.get("sessionGeneration"), "worldScenes": frame.get("worldScenes"),
                         "insertions": frame.get("insertions"), "refused": frame.get("refused"),
                         "shadowMapEpoch": shadow.get("shadowMapEpoch")})
        receipt = json.loads((run / "receipt.json").read_text("utf-8"))
        instance = Path(receipt.get("instanceRoot", ""))
        markers = {p.name: p.stat().st_mtime for p in (instance / "WarVK/Temp").glob("crossmap_*.txt")} \
            if instance.is_dir() else {}
        # Marker times on the probe's clock, through the control plane's
        # [elapsed, timestampMs, frameIndex, inGame] series.
        series = ((receipt.get("observer") or {}).get("controlPlane") or {}).get("frameSeries") or []
        if series and series[0][1]:
            markers = {k: round(series[0][0] + (v * 1000 - series[0][1]) / 1000, 1) for k, v in markers.items()}
        stderr = (run / "game.stderr").read_text("utf-8", errors="replace") if (run / "game.stderr").is_file() else ""
        annotation = [json.loads(l.split(": ", 1)[1]) for l in stderr.splitlines()
                      if l.startswith("D3D9 annotation-proxy: {")]
        hosts = {}
        for log in sorted(run.glob("host.log*")):
            for line in log.read_text("utf-8", errors="replace").splitlines():
                if line.startswith("D3D9 annotation-host: {"):
                    hosts[log.name] = json.loads(line.split(": ", 1)[1])
        data_log = run / "logs"
        resets = []
        for log in data_log.glob("*_d3d9.log"):
            resets += [l.strip()[:160] for l in log.read_text("utf-8", errors="replace").splitlines()
                       if "session reset generation=" in l]
        generations = [r["sessionGeneration"] for r in rows if r["sessionGeneration"] is not None]
        result[mode] = {"samples": rows, "markers": markers, "annotationProxy": annotation[-1:] if annotation else [],
                        "annotationHost": hosts, "sessionResetLines": resets,
                        "generationRises": [b - a for a, b in zip(generations, generations[1:]) if b != a],
                        "captures": sorted(p.name for p in run.glob("e*.bmp")), "ok": receipt.get("ok"),
                        "checks": receipt.get("checks")}
    return result


def main(argv: list[str]) -> int:
    if len(argv) == 2 and argv[0] == "actions":
        print("\n".join(actions(argv[1])))
        return 0
    if len(argv) == 5 and argv[0] == "analyze":
        kind, inprocess, proxy, out = argv[1], Path(argv[2]), Path(argv[3]), Path(argv[4])
        out.mkdir(parents=True, exist_ok=True)
        runs = {"inprocess": inprocess, "proxy": proxy}
        result = {"outline": analyze_outline, "sweep": analyze_sweep, "extramap": analyze_extramap}[kind](runs, out)
        (out / f"{kind}-analysis.json").write_text(json.dumps(result, ensure_ascii=False, indent=1), encoding="utf-8")
        print(json.dumps(result, ensure_ascii=False)[:6000])
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
