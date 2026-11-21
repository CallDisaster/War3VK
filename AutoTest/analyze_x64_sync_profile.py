#!/usr/bin/env python3
"""P0: per in-game frame attribution of proxy-mode synchronous waits and host GPU waits.

Reads the cumulative snapshots that the instrumented front end and host write
(WARVK_D3D9_SYNC_PROFILE_EVERY):

  game.stderr   D3D9 sync-profile: {"schema":"warvk-d3d9-sync-profile/v1", ...}   (front end, x86)
  host.log*     D3D9 host-profile: {"schema":"warvk-d3d9-host-profile/v1", ...}   (x64 host)

and the probe's receipt.json (run_x64_game_proxy_probe.py). Loading is dropped:
the window starts at the first snapshot taken at least --settle seconds after
the control plane's readyAtSeconds (the frameSeries rows carry the product's
wall clock, the snapshots carry their own) and ends at the last snapshot before
the first capture/va/close action (or --end-action). Everything is the
difference of two cumulative snapshots, divided by the in-game (world) frames
between them: the host's worldSceneEnters (annotation channel), falling back to
the front end's frameScenes / 5 and then to Present count.

Self-checks: the exit lines of the device session must satisfy the transport
identities of docs/research/2026-11-17-x64-traffic-profile.md section 3
(ring records, host submissions), and the profile's own exit snapshots must
reconcile with them (records published, submissions by call site, texture
commits).

Measurement only; it reads artifacts and writes nothing unless --json/--markdown is given.

usage: analyze_x64_sync_profile.py <run dir> [<run dir> ...] [--json out.json] [--markdown out.md]
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
from pathlib import Path

FRONT_PREFIX = "D3D9 sync-profile: "
HOST_PREFIX = "D3D9 host-profile: "
SETTLE_SECONDS = 12.0
FRAME_SCENES_PER_WORLD_FRAME = 5
# Naming: every host number is owner-thread WALL-CLOCK time spent on a path,
# not GPU execution time (no Vulkan timestamps are taken). WAIT_PATH_KINDS are
# the settle/retire/present/acquire/event wait paths (Retire and PresentQueue
# include CPU work and Sleep(1) polls); SubmitCpu is waiting for DXVK's
# submission thread; COPY_PATH_KINDS wrap more than a memcpy (DiscardCarry =
# allocateStorage + invalidateBuffer + memcpy; CasterCopy = the whole
# ShadowBeforeUi::addCaster). x86 waits contain host work which contains these
# waits: the three levels nest and must never be added together.
WAIT_PATH_KINDS = ("SettleIdle", "SettleGpu", "Retire", "PresentRetire", "Acquire", "PresentQueue",
                   "StreamLatency", "FlightReuse", "Event")
COPY_PATH_KINDS = ("DiscardCarry", "UploadCopy", "CasterCopy")
HIST = "<10µs/<50/<200/<1ms/<5ms/<20ms/≥20ms"


def json_lines(path: Path, prefix: str) -> list[dict]:
    rows = []
    if not path.is_file():
        return rows
    for line in path.read_text("utf-8", errors="replace").splitlines():
        at = line.find(prefix)
        if at < 0:
            continue
        try:
            rows.append(json.loads(line[at + len(prefix):]))
        except ValueError:
            continue
    return rows


def schema_lines(path: Path, schema: str) -> list[dict]:
    """Every JSON object on a line carrying "schema":"<schema>"."""
    rows = []
    if not path.is_file():
        return rows
    for line in path.read_text("utf-8", errors="replace").splitlines():
        if f'"schema":"{schema}"' not in line:
            continue
        try:
            rows.append(json.loads(line[line.index("{"):]))
        except ValueError:
            continue
    return rows


def front_snapshots(run: Path) -> list[dict]:
    """The device connection's snapshots: the connection with the most presents."""
    rows = json_lines(run / "game.stderr", FRONT_PREFIX)
    sessions, current = [], []
    for row in rows:
        # A new connection restarts its cumulative counters.
        if current and (row["pid"] != current[-1]["pid"] or row["seconds"] < current[-1]["seconds"]
                        or row["presents"] < current[-1]["presents"] or current[-1]["reason"] == "exit"):
            sessions.append(current)
            current = []
        current.append(row)
    if current:
        sessions.append(current)
    return max(sessions, key=lambda s: s[-1]["presents"], default=[])


def host_snapshots(run: Path) -> tuple[Path | None, list[dict]]:
    best: tuple[Path | None, list[dict]] = (None, [])
    for log in sorted(run.glob("host.log*")):
        rows = json_lines(log, HOST_PREFIX)
        if rows and (not best[1] or rows[-1]["presents"] > best[1][-1]["presents"]):
            best = (log, rows)
    return best


def receipt_window(run: Path, settle: float, end_action: str | None):
    """(start unixMs, end unixMs, facts) from the probe receipt's control plane."""
    path = run / "receipt.json"
    facts: dict = {}
    if not path.is_file():
        return None, None, facts
    receipt = json.loads(path.read_text("utf-8"))
    observer = receipt.get("observer") or {}
    plane = observer.get("controlPlane") or {}
    series = [r for r in plane.get("frameSeries") or [] if r[1] is not None]
    ready = plane.get("readyAtSeconds")
    env = receipt.get("childEnvironment") or {}
    facts.update(readyAtSeconds=ready, mode=receipt.get("mode"), profile=receipt.get("profile"),
                 ok=receipt.get("ok"), failedChecks=[k for k, v in (receipt.get("checks") or {}).items() if not v],
                 errors=receipt.get("errors"),
                 ringKiB=env.get("WARVK_D3D9_COMMAND_RING_KIB"), every=env.get("WARVK_D3D9_SYNC_PROFILE_EVERY"))
    if not series:
        return None, None, facts

    def unix_at(elapsed):
        row = min(series, key=lambda r: abs(r[0] - elapsed))
        return row[1] + (elapsed - row[0]) * 1000.0

    actions = observer.get("actions") or []
    stops = [a["seconds"] for a in actions
             if (end_action and a["action"].startswith(end_action))
             or (not end_action and a["action"].split("=")[0] in ("capture", "va", "close"))]
    end = min(stops) if stops else series[-1][0]
    start = (ready if ready is not None else series[0][0]) + settle
    facts.update(windowElapsed=[round(start, 1), round(end, 1)])
    # The product's own frame counter over the same window (the A/B gate's pace measure).
    windows = []
    for before, after in zip(series, series[1:]):
        if before[0] < start or after[0] > end or not after[3] or after[2] is None or before[2] is None:
            continue
        ms, frames = after[1] - before[1], after[2] - before[2]
        if ms > 0 and frames > 0:
            windows.append(frames * 1000.0 / ms)
    facts["controlPlaneMedianFps"] = round(statistics.median(windows), 2) if windows else None
    return unix_at(start), unix_at(end), facts


def pick(rows, start_ms, end_ms):
    periodic = [r for r in rows if r.get("reason") == "periodic"]
    inside = [r for r in periodic if (start_ms is None or r["unixMs"] >= start_ms)
              and (end_ms is None or r["unixMs"] <= end_ms)]
    if len(inside) < 2:
        return None, None
    return inside[0], inside[-1]


def keyed(rows, key):
    return {r[key]: r for r in rows}


def diff(b: dict, a: dict, keys):
    return {k: b.get(k, 0) - a.get(k, 0) for k in keys}


def vec(b, a):
    a = a or [0] * len(b)
    return [x - y for x, y in zip(b, a)]


def percentile(values, q):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(round(q * (len(ordered) - 1))))]


def frame_times(front, a, b):
    """Game-thread frame times (us between Present returns) of the snapshots after a up to b."""
    inside = [r for r in front if a["presents"] < r["presents"] <= b["presents"] and r.get("reason") == "periodic"]
    values = [v for r in inside for v in r.get("frameUs", [])]
    if not values:
        return None
    return dict(count=len(values), dropped=sum(r.get("frameUsDropped", 0) for r in inside),
                p50Ms=round(percentile(values, 0.50) / 1000, 2), p95Ms=round(percentile(values, 0.95) / 1000, 2),
                p99Ms=round(percentile(values, 0.99) / 1000, 2), maxMs=round(max(values) / 1000, 2),
                meanMs=round(sum(values) / len(values) / 1000, 2),
                over50Ms=sum(v >= 50000 for v in values), over100Ms=sum(v >= 100000 for v in values))


def delta_front(a, b, frames):
    seconds = b["seconds"] - a["seconds"]
    presents = b["presents"] - a["presents"]
    out = dict(frames=frames, presents=presents, frameScenes=b.get("frameScenes", 0) - a.get("frameScenes", 0),
               seconds=round(seconds, 3), presentsAt=[a["presents"], b["presents"]], unixMs=[a["unixMs"], b["unixMs"]],
               worldFps=round(frames / seconds, 2) if seconds > 0 else None,
               presentFps=round(presents / seconds, 2) if seconds > 0 else None)
    if frames <= 0 or seconds <= 0:
        return out
    per = lambda v: v / frames  # noqa: E731
    ops_a, ops_b = keyed(a["ops"], "name"), keyed(b["ops"], "name")
    ops = []
    for name, ob in ops_b.items():
        oa = ops_a.get(name, {})
        d = diff(ob, oa, ("sync", "syncUs", "syncBytes", "syncParks", "async", "asyncBytes", "full", "fullUs",
                          "fullParks"))
        if not any(d[k] for k in ("sync", "async", "full")):
            continue
        ops.append(dict(op=name, syncPerFrame=round(per(d["sync"]), 3), syncUsPerFrame=round(per(d["syncUs"]), 1),
                        meanUs=round(d["syncUs"] / d["sync"], 1) if d["sync"] else None,
                        maxUsCumulative=ob["syncMaxUs"], hist=vec(ob["hist"], oa.get("hist")),
                        syncParksPerFrame=round(per(d["syncParks"]), 2),
                        asyncPerFrame=round(per(d["async"]), 3), asyncBytesPerFrame=round(per(d["asyncBytes"])),
                        syncBytesPerFrame=round(per(d["syncBytes"])),
                        fullPerFrame=round(per(d["full"]), 3), fullUsPerFrame=round(per(d["fullUs"]), 1),
                        fullParksPerFrame=round(per(d["fullParks"]), 2), fullHist=vec(ob["fullHist"], oa.get("fullHist")),
                        fullMaxUsCumulative=ob.get("fullMaxUs")))
    ops.sort(key=lambda r: -(r["syncUsPerFrame"] + r["fullUsPerFrame"]))
    apis_a, apis_b = keyed(a["apis"], "api"), keyed(b["apis"], "api")
    apis = []
    for name, ab in apis_b.items():
        d = diff(ab, apis_a.get(name, {}), ("entries", "sync", "syncUs", "async", "asyncBytes", "full", "fullUs"))
        if not any(d.values()):
            continue
        apis.append(dict(api=name, entriesPerFrame=round(per(d["entries"]), 2), syncPerFrame=round(per(d["sync"]), 3),
                         syncUsPerFrame=round(per(d["syncUs"]), 1), asyncPerFrame=round(per(d["async"]), 2),
                         asyncBytesPerFrame=round(per(d["asyncBytes"])), fullPerFrame=round(per(d["full"]), 3),
                         fullUsPerFrame=round(per(d["fullUs"]), 1)))
    apis.sort(key=lambda r: -(r["syncUsPerFrame"] + r["fullUsPerFrame"]))
    pairs_a = {(p[0], p[1]): p for p in a["pairs"]}
    pairs = []
    for p in b["pairs"]:
        q = pairs_a.get((p[0], p[1]), [None, None, 0, 0])
        calls, us = p[2] - q[2], p[3] - q[3]
        if calls:
            pairs.append(dict(api=p[0], op=p[1], callsPerFrame=round(per(calls), 3), usPerFrame=round(per(us), 1)))
    pairs.sort(key=lambda r: -r["usPerFrame"])
    sync_us = sum(r["syncUsPerFrame"] for r in ops)
    full_us = sum(r["fullUsPerFrame"] for r in ops)
    frame_us = 1e6 * seconds / frames
    present = next((r for r in ops if r["op"] == "Present"), None)
    out.update(frameUs=round(frame_us, 1), syncCallsPerFrame=round(sum(r["syncPerFrame"] for r in ops), 2),
               syncUsPerFrame=round(sync_us, 1), ringFullUsPerFrame=round(full_us, 1),
               blockedShare=round((sync_us + full_us) / frame_us, 3),
               presentUsPerFrame=present["syncUsPerFrame"] if present else 0.0,
               asyncPerFrame=round(sum(r["asyncPerFrame"] for r in ops), 1),
               asyncBytesPerFrame=round(sum(r["asyncBytesPerFrame"] for r in ops)),
               ringBytesPerFrame=round(per(b["ringBytes"] - a["ringBytes"])),
               ringParksPerFrame=round(per(b["ringParks"] - a["ringParks"]), 2),
               ringWakesPerFrame=round(per(b["ringWakes"] - a["ringWakes"]), 2),
               ringCapacity=b.get("ringCapacity"), truncated=b.get("truncated"),
               ops=ops, apis=apis, pairs=pairs[:25])
    ta, tb = a.get("textures") or {}, b.get("textures") or {}
    if tb:
        top_a = {(t[0], t[1], t[2]): t for t in ta.get("top", [])}
        top = []
        for t in tb.get("top", []):
            q = top_a.get((t[0], t[1], t[2]), [0] * 16)
            updates = t[7] - q[7]
            if updates > 0:
                top.append(dict(id=t[0], generation=t[1], level=t[2], width=t[3], height=t[4], format=t[5], levels=t[6],
                                updatesPerFrame=round(per(updates), 3), partialShare=round((t[8] - q[8]) / updates, 2),
                                bytesPerFrame=round(per(t[9] - q[9])), bytesPerUpdate=round((t[9] - q[9]) / updates),
                                chunksPerUpdate=round((t[10] - q[10]) / updates, 2),
                                newInWindow=(t[0], t[1], t[2]) not in top_a,
                                creator=t[11] if len(t) > 11 else None, stage=t[14] if len(t) > 14 else None))
        top.sort(key=lambda r: -r["updatesPerFrame"])
        d = diff(tb, ta, ("initial", "initialBytes", "updates", "updateBytes", "chunks", "untracked"))
        out["textures"] = dict(initialPerFrame=round(per(d["initial"]), 3),
                               initialBytesPerFrame=round(per(d["initialBytes"])),
                               updatesPerFrame=round(per(d["updates"]), 2),
                               updateBytesPerFrame=round(per(d["updateBytes"])),
                               chunksPerFrame=round(per(d["chunks"]), 2), untrackedInWindow=d["untracked"],
                               trackedLevels=tb.get("tracked"),
                               topUpdatesShare=round(sum(r["updatesPerFrame"] for r in top) / per(d["updates"]), 3)
                               if d["updates"] else None,
                               top=top)
    return out


def delta_host(a, b, frames):
    seconds = b["seconds"] - a["seconds"]
    out = dict(frames=frames, seconds=round(seconds, 3), presentsAt=[a["presents"], b["presents"]],
               worldSceneEnters=b.get("worldSceneEnters", 0) - a.get("worldSceneEnters", 0),
               syncInterval=b.get("syncInterval"), streamMode=b.get("streamMode"))
    if frames <= 0 or seconds <= 0:
        return out
    per = lambda v: v / frames  # noqa: E731
    wall_us = seconds * 1e6
    idle, apply, write = (b[k] - a[k] for k in ("idleUs", "applyUs", "profileWriteUs"))
    out.update(idleShare=round(idle / wall_us, 3), busyShare=round(apply / wall_us, 3),
               profileWriteShare=round(write / wall_us, 4), idleUsPerFrame=round(per(idle), 1),
               applyUsPerFrame=round(per(apply), 1),
               submitsPerFrame=round(per(b["submitted"] - a["submitted"]), 2),
               drawsPerFrame=round(per(b["draws"] - a["draws"]), 1))
    ops_a, ops_b = keyed(a["ops"], "name"), keyed(b["ops"], "name")
    ops = []
    for name, ob in ops_b.items():
        oa = ops_a.get(name, {})
        count, us = ob["count"] - oa.get("count", 0), ob["us"] - oa.get("us", 0)
        if count:
            ops.append(dict(op=name, countPerFrame=round(per(count), 3), usPerFrame=round(per(us), 1),
                            meanUs=round(us / count, 1), maxUsCumulative=ob["maxUs"],
                            slowPerFrame=[round(per(x), 3) for x in vec(ob.get("slow", [0, 0, 0]), oa.get("slow"))]))
    ops.sort(key=lambda r: -r["usPerFrame"])
    waits_a = {(w["site"], w["kind"]): w for w in a["waits"]}
    waits = []
    for w in b["waits"]:
        v = waits_a.get((w["site"], w["kind"]), {})
        count, us, polls = w["count"] - v.get("count", 0), w["us"] - v.get("us", 0), w["polls"] - v.get("polls", 0)
        if count:
            waits.append(dict(site=w["site"], kind=w["kind"], countPerFrame=round(per(count), 3),
                              usPerFrame=round(per(us), 1), pollsPerFrame=round(per(polls), 2),
                              meanUs=round(us / count, 1),
                              slowPerFrame=[round(per(x), 3) for x in vec(w.get("slow", [0, 0, 0]), v.get("slow"))]))
    waits.sort(key=lambda r: -r["usPerFrame"])
    by_site, by_kind = {}, {}
    for w in waits:
        if w["kind"] in WAIT_PATH_KINDS or w["kind"] == "SubmitCpu":
            by_site[w["site"]] = round(by_site.get(w["site"], 0) + w["usPerFrame"], 1)
        by_kind[w["kind"]] = round(by_kind.get(w["kind"], 0) + w["usPerFrame"], 1)
    submits_a = keyed(a["submits"], "site")
    submits = []
    for s in b["submits"]:
        v = submits_a.get(s["site"], dict(submits=0, waitForGpu=0))
        n, g = s["submits"] - v["submits"], s["waitForGpu"] - v["waitForGpu"]
        if n:
            submits.append(dict(site=s["site"], submitsPerFrame=round(per(n), 3), waitForGpuPerFrame=round(per(g), 3)))
    submits.sort(key=lambda r: -r["submitsPerFrame"])
    out.update(waitPathUsPerFrame=round(sum(w["usPerFrame"] for w in waits if w["kind"] in WAIT_PATH_KINDS), 1),
               submitCpuUsPerFrame=round(sum(w["usPerFrame"] for w in waits if w["kind"] == "SubmitCpu"), 1),
               copyPathUsPerFrame=round(sum(w["usPerFrame"] for w in waits if w["kind"] in COPY_PATH_KINDS), 1),
               sleepPollsPerFrame=round(sum(w["pollsPerFrame"] for w in waits), 2),
               ops=ops, waits=waits, waitsBySite=dict(sorted(by_site.items(), key=lambda kv: -kv[1])),
               waitsByKind=dict(sorted(by_kind.items(), key=lambda kv: -kv[1])), submits=submits)
    return out


def last_of(rows):
    return rows[-1] if rows else {}


def identities(run: Path, host_log: Path | None, front: list[dict], host: list[dict]) -> dict:
    """Transport identities on the device session's exit lines, and the profile's own reconciliation."""
    stderr = run / "game.stderr"
    ordered = last_of(schema_lines(stderr, "warvk-d3d9-ordered-async/v1"))
    fixed = last_of(schema_lines(stderr, "warvk-d3d9-fixed-batch-proxy/v1"))
    annotation = last_of(schema_lines(stderr, "warvk-d3d9-annotation-proxy/v1"))
    ring = last_of(schema_lines(stderr, "warvk-d3d9-command-ring-proxy/v1"))
    clear = last_of(schema_lines(stderr, "warvk-d3d9-async-clear/v1"))
    # P2 switches (2026-11-18): records that left the synchronous count for the ordered one.
    extra = last_of(schema_lines(stderr, "warvk-d3d9-ordered-extra/v1"))
    extra_published = sum(extra.get(key, 0) for key in ("texturePublished", "flushPublished", "scenePublished",
                                                         "presentsAhead"))
    checks: dict = {}
    if ordered and fixed and annotation and ring and clear:
        lhs = ring["records"]
        rhs = (ordered["uploadsPublished"] + ordered["statesPublished"] + fixed["flushedBatches"]
               + annotation["messages"] + ring["syncCalls"] + clear["clearPublished"] + extra_published)
        checks["ringRecords"] = dict(lhs=lhs, rhs=rhs, holds=lhs == rhs)
    exit_front = next((r for r in reversed(front) if r.get("reason") == "exit"), None)
    if exit_front and ring:
        published = sum(o["sync"] + o["async"] for o in exit_front["ops"])
        checks["profilePublished"] = dict(lhs=published, rhs=ring["published"], holds=published == ring["published"])
        ring_ops = sum(o["sync"] for o in exit_front["ops"])
        # Records published on the pipe before the ring attached are synchronous calls too.
        checks["profileSyncCalls"] = dict(lhs=ring_ops, rhs=ring["syncCalls"],
                                          pipeSync=ring["published"] - ring["records"],
                                          holds=0 <= ring_ops - ring["syncCalls"] <= ring["published"] - ring["records"])
    if host_log is not None:
        stream = last_of(schema_lines(host_log, "warvk-d3d9-stream-host/v1"))
        total = last_of(schema_lines(host_log, "warvk-d3d9-host/v1"))
        if stream and total and ordered:
            textures = total["uploads"] - ordered["uploadsPublished"]
            # A texture upload recorded as a staging slice rides in the stream list: no submission of its own.
            exit_profile = next((r for r in reversed(host) if r.get("reason") == "exit"), None) or {}
            sliced = (exit_profile.get("uploadSlices") or {}).get("recorded", 0)
            # A pipelined Present or Clear (WARVK_D3D9_PRESENT_PIPELINE) is recorded in the stream list too.
            pipeline = exit_profile.get("presentPipeline") or {}
            rhs = (stream["streamSubmissions"] + textures - sliced + total["texturesCreated"] + total["clears"]
                   + total["presentAttempts"] + total["readbacks"] + 1
                   - pipeline.get("presents", 0) - pipeline.get("clears", 0))
            checks["hostSubmitted"] = dict(lhs=total["submitted"], rhs=rhs, holds=total["submitted"] == rhs)
            exit_host = next((r for r in reversed(host) if r.get("reason") == "exit"), None)
            if exit_host:
                by_site = sum(s["submits"] for s in exit_host["submits"])
                checks["profileSubmitsBySite"] = dict(lhs=by_site, rhs=total["submitted"],
                                                      holds=by_site == total["submitted"])
                applied = sum(o["count"] for o in exit_host["ops"])
                checks["profileHostApplied"] = dict(lhs=applied, rhs=total["accepted"], holds=applied == total["accepted"])
            if exit_front and exit_front.get("textures"):
                t = exit_front["textures"]
                checks["profileTextureCommits"] = dict(lhs=t["initial"] + t["updates"], rhs=textures,
                                                       holds=t["initial"] + t["updates"] == textures)
    checks["allHold"] = all(v["holds"] for v in checks.values() if isinstance(v, dict)) if checks else None
    return checks


def analyze(run: Path, settle: float, end_action: str | None) -> dict:
    result = dict(run=str(run), name=run.name)
    start_ms, end_ms, facts = receipt_window(run, settle, end_action)
    result["receipt"] = facts
    front = front_snapshots(run)
    log, host = host_snapshots(run)
    result.update(frontSnapshots=len(front), hostLog=log.name if log else None, hostSnapshots=len(host))
    result["identities"] = identities(run, log, front, host)
    a, b = pick(front, start_ms, end_ms)
    if a is None:
        result["error"] = "fewer than two front-end snapshots inside the in-game window"
        return result
    by_presents = {r["presents"]: r for r in host if r.get("reason") == "periodic"}
    ha, hb = by_presents.get(a["presents"]), by_presents.get(b["presents"])
    result["hostMatchedBy"] = "presents"
    if ha is None or hb is None:
        ha, hb = pick(host, a["unixMs"] - 1, b["unixMs"] + 1)
        result["hostMatchedBy"] = "unixMs"
    world = (hb.get("worldSceneEnters", 0) - ha.get("worldSceneEnters", 0)) if ha and hb else 0
    scenes = b.get("frameScenes", 0) - a.get("frameScenes", 0)
    if world > 0:
        frames, basis = world, "host worldSceneEnters"
    elif scenes > 0:
        frames, basis = scenes // FRAME_SCENES_PER_WORLD_FRAME, "front frameScenes/5"
    else:
        frames, basis = b["presents"] - a["presents"], "presents"
    # Gameplay numbers need a ready control plane and world frames in the window;
    # anything else is a loading-phase diagnostic, never a gameplay fps.
    valid = facts.get("readyAtSeconds") is not None and basis != "presents"
    result.update(frameBasis=basis, sampleValidForGameplay=valid,
                  phase="gameplay" if valid else "loading-or-unready diagnostic")
    result["front"] = delta_front(a, b, frames)
    result["front"]["frameTimes"] = frame_times(front, a, b)
    if not valid:
        result["front"]["loadingPresentFps"] = result["front"].pop("worldFps", None)
        result["front"]["worldFps"] = None
    if ha is not None and hb is not None:
        result["host"] = delta_host(ha, hb, frames)
    return result


def fmt_table(rows, columns):
    head = "| " + " | ".join(c[0] for c in columns) + " |"
    rule = "|" + "|".join("---" for _ in columns) + "|"
    body = ["| " + " | ".join(str(r.get(c[1], "")) for c in columns) + " |" for r in rows]
    return "\n".join([head, rule, *body])


def markdown(results) -> str:
    out = []
    summary = []
    for r in results:
        f, h = r.get("front") or {}, r.get("host") or {}
        ft = f.get("frameTimes") or {}
        summary.append(dict(run=r["name"], valid=r.get("sampleValidForGameplay"), worldFps=f.get("worldFps"),
                            loadingFps=f.get("loadingPresentFps"), presentFps=f.get("presentFps"),
                            p50=ft.get("p50Ms"), p95=ft.get("p95Ms"), p99=ft.get("p99Ms"), maxMs=ft.get("maxMs"),
                            cpFps=(r["receipt"] or {}).get("controlPlaneMedianFps"),
                            frameUs=f.get("frameUs"), sync=f.get("syncCallsPerFrame"), syncUs=f.get("syncUsPerFrame"),
                            presentUs=f.get("presentUsPerFrame"), fullUs=f.get("ringFullUsPerFrame"),
                            blocked=f.get("blockedShare"), asyncKiB=round((f.get("asyncBytesPerFrame") or 0) / 1024, 1),
                            waitUs=h.get("waitPathUsPerFrame"), submitCpuUs=h.get("submitCpuUsPerFrame"),
                            copyUs=h.get("copyPathUsPerFrame"), polls=h.get("sleepPollsPerFrame"),
                            hostBusy=h.get("busyShare"), hostIdle=h.get("idleShare"),
                            submits=h.get("submitsPerFrame"), interval=h.get("syncInterval"),
                            frames=f.get("frames"), basis=r.get("frameBasis"),
                            window=(r["receipt"] or {}).get("windowElapsed"),
                            ids=(r.get("identities") or {}).get("allHold")))
    out.append(fmt_table(summary, [("run", "run"), ("gameplay sample", "valid"), ("world fps", "worldFps"),
        ("loading present fps (diagnostic)", "loadingFps"), ("present fps", "presentFps"),
        ("frame p50 ms", "p50"), ("p95 ms", "p95"), ("p99 ms", "p99"), ("max ms", "maxMs"),
        ("control-plane fps", "cpFps"), ("frame µs", "frameUs"), ("sync/frame", "sync"),
        ("sync µs/frame", "syncUs"), ("Present µs", "presentUs"), ("ring-full µs", "fullUs"),
        ("x86 blocked share", "blocked"), ("async KiB/frame", "asyncKiB"),
        ("host wait-path wall µs (nested)", "waitUs"), ("host submit-wait µs (nested)", "submitCpuUs"),
        ("host copy-path wall µs (nested)", "copyUs"), ("Sleep(1)/frame", "polls"),
        ("host busy", "hostBusy"), ("host idle", "hostIdle"), ("submits/frame", "submits"),
        ("syncInterval", "interval"), ("frames", "frames"), ("frame basis", "basis"), ("window s", "window"),
        ("identities hold", "ids")]))
    for r in results:
        out.append(f"\n### {r['name']}\n")
        out.append(f"identities: `{json.dumps(r.get('identities'), ensure_ascii=False)}`\n")
        if r.get("error"):
            out.append(f"error: {r['error']}")
            continue
        f, h = r["front"], r.get("host") or {}
        out.append("Front end, sync and ring-full by op (per in-game frame):\n")
        out.append(fmt_table(f["ops"][:14], [("op", "op"), ("sync/frame", "syncPerFrame"),
            ("sync µs/frame", "syncUsPerFrame"), ("mean µs", "meanUs"), (f"hist {HIST}", "hist"),
            ("parks/frame", "syncParksPerFrame"), ("async/frame", "asyncPerFrame"),
            ("async B/frame", "asyncBytesPerFrame"), ("full/frame", "fullPerFrame"),
            ("full µs/frame", "fullUsPerFrame"), ("full parks/frame", "fullParksPerFrame"),
            (f"full hist {HIST}", "fullHist")]))
        out.append("\nFront end by API:\n")
        out.append(fmt_table(f["apis"][:16], [("api", "api"), ("entries/frame", "entriesPerFrame"),
            ("sync/frame", "syncPerFrame"), ("sync µs/frame", "syncUsPerFrame"), ("async/frame", "asyncPerFrame"),
            ("async B/frame", "asyncBytesPerFrame"), ("full µs/frame", "fullUsPerFrame")]))
        out.append("\n(API, op) sync pairs:\n")
        out.append(fmt_table(f["pairs"][:12], [("api", "api"), ("op", "op"), ("calls/frame", "callsPerFrame"),
                                               ("µs/frame", "usPerFrame")]))
        t = f.get("textures")
        if t:
            out.append(f"\nTexture uploads: initial/frame {t['initialPerFrame']}, updates/frame {t['updatesPerFrame']}, "
                       f"update B/frame {t['updateBytesPerFrame']}, chunks/frame {t['chunksPerFrame']}, "
                       f"top-32 share of updates {t['topUpdatesShare']}, untracked in window {t['untrackedInWindow']}\n")
            out.append(fmt_table(t["top"][:16], [("id", "id"), ("level", "level"), ("w", "width"), ("h", "height"),
                ("format", "format"), ("levels", "levels"), ("updates/frame", "updatesPerFrame"),
                ("partial share", "partialShare"), ("B/update", "bytesPerUpdate"), ("chunks/update", "chunksPerUpdate"),
                ("new in window", "newInWindow"), ("creator", "creator"), ("last stage", "stage")]))
        if h:
            out.append("\nHost waits by (site, kind):\n")
            out.append(fmt_table(h["waits"][:20], [("site", "site"), ("kind", "kind"), ("count/frame", "countPerFrame"),
                ("µs/frame", "usPerFrame"), ("mean µs", "meanUs"), ("Sleep(1) polls/frame", "pollsPerFrame"),
                ("≥1/5/20 ms per frame", "slowPerFrame")]))
            out.append(f"\nHost wait-path + submit-wait wall µs/frame by site (nested, never add to x86 waits): `{json.dumps(h['waitsBySite'])}`\n")
            out.append(f"Host µs/frame by kind: `{json.dumps(h['waitsByKind'])}`\n")
            out.append("\nHost apply by op:\n")
            out.append(fmt_table(h["ops"][:12], [("op", "op"), ("count/frame", "countPerFrame"),
                ("µs/frame", "usPerFrame"), ("mean µs", "meanUs"), ("≥1/5/20 ms per frame", "slowPerFrame")]))
            out.append("\nHost submits by site:\n")
            out.append(fmt_table(h["submits"], [("site", "site"), ("submits/frame", "submitsPerFrame"),
                                               ("waitForGpu/frame", "waitForGpuPerFrame")]))
    return "\n".join(out) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("runs", nargs="+", type=Path)
    parser.add_argument("--settle", type=float, default=SETTLE_SECONDS)
    parser.add_argument("--end-action", default=None,
                        help="end the window at the first action starting with this (default: first capture/va/close)")
    parser.add_argument("--json", type=Path)
    parser.add_argument("--markdown", type=Path)
    args = parser.parse_args(argv)
    results = [analyze(run, args.settle, args.end_action) for run in args.runs]
    text = markdown(results)
    if args.json:
        args.json.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    if args.markdown:
        args.markdown.write_text(text, encoding="utf-8")
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    print(text)
    return 0 if all("error" not in r for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
