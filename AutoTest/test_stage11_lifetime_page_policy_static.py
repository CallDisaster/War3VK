#!/usr/bin/env python3
"""Static and synthetic-reference checks for the Stage11 lifetime page policy.

The reference policy below mirrors the pure C++ header only for offline
contract checking.  It is not production C++ execution and not a GPU result.
"""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER_PATH = ROOT / "src/d3d9/war3/render/war3_stage11_lifetime_page_policy.h"
CPP_PATH = ROOT / "AutoTest/test_stage11_lifetime_page_policy.cpp"
PLAN_PATH = ROOT / "docs/plan/2026-09-20-stage11-lifetime-page-policy.md"

ALIGNMENT = 256
PAGE_BYTES = 16 << 20
SHARED_CAP = 384 << 20
MIN_CAP = 128 << 20
MAX_CAP = 512 << 20
HARD_MAX_PAGES = MAX_CAP // PAGE_BYTES


def align_up(value: int) -> int | None:
    if value <= 0 or value > (1 << 64) - ALIGNMENT:
        return None
    return (value + ALIGNMENT - 1) & ~(ALIGNMENT - 1)


def page_capacity(required: int, cap: int) -> int:
    aligned = align_up(required)
    if aligned is None or aligned > cap:
        return 0
    if aligned <= PAGE_BYTES:
        return PAGE_BYTES
    pages = (aligned + PAGE_BYTES - 1) // PAGE_BYTES
    if pages > cap // PAGE_BYTES:
        return 0
    return pages * PAGE_BYTES


class Request:
    def __init__(
        self,
        required_bytes: int,
        lifetime: str = "unknown",
        resident_bytes: int = 0,
        page_create_gate_open: bool = True,
        max_pages: int = HARD_MAX_PAGES,
        map_epoch: int = 7,
        device_epoch: int = 9,
        cap_bytes: int = SHARED_CAP,
        same_retention_intent_only: bool = False,
    ):
        self.required_bytes = required_bytes
        self.lifetime = lifetime
        self.resident_bytes = resident_bytes
        self.page_create_gate_open = page_create_gate_open
        self.max_pages = max_pages
        self.map_epoch = map_epoch
        self.device_epoch = device_epoch
        self.cap_bytes = cap_bytes
        self.same_retention_intent_only = same_retention_intent_only


class Page:
    def __init__(
        self,
        page_id: int,
        used: int = 0,
        capacity: int = PAGE_BYTES,
        lifetime: str = "unknown",
        owner_state: str = "active",
        map_epoch: int = 7,
        device_epoch: int = 9,
    ):
        self.id = page_id
        self.used = used
        self.capacity = capacity
        self.lifetime = lifetime
        self.owner_state = owner_state
        self.map_epoch = map_epoch
        self.device_epoch = device_epoch


LIFETIMES = ("unknown", "short", "long")
OWNER_STATES = ("active", "retirePending", "retired")


def normalize_lifetime(lifetime: str) -> str:
    return lifetime if lifetime in LIFETIMES else "unknown"


def may_share(page_lifetime: str, requested_lifetime: str) -> bool:
    page_lifetime = normalize_lifetime(page_lifetime)
    requested_lifetime = normalize_lifetime(requested_lifetime)
    return (
        page_lifetime == requested_lifetime
        or page_lifetime == "unknown"
        or requested_lifetime == "unknown"
    )


def _no_safe(reason: str, request: Request) -> dict:
    return {
        "kind": "noSafeSelection",
        "reason": reason,
        "pageId": 0,
        "offset": 0,
        "sliceBytes": 0,
        "nextUsed": 0,
        "pageBytes": 0,
        "residentBytesAfter": request.resident_bytes,
        "mixedLifetimeBorrow": False,
        "conservativeUnknownPage": False,
    }


def reference_plan(request: Request, pages: list[Page]) -> dict:
    result = _no_safe("invalidRequest", request)
    aligned = align_up(request.required_bytes)
    if aligned is None or aligned == 0:
        return result
    if request.map_epoch == 0 or request.device_epoch == 0:
        return _no_safe("invalidEpoch", request)
    if not MIN_CAP <= request.cap_bytes <= MAX_CAP:
        return _no_safe("sharedCapOutOfRange", request)
    if request.resident_bytes > request.cap_bytes:
        return _no_safe("residentMismatch", request)
    if (
        request.max_pages == 0
        or request.max_pages > HARD_MAX_PAGES
        or len(pages) > request.max_pages
    ):
        return _no_safe("pageVectorLimit", request)
    active_view_capacity = 0
    seen = set()
    for page in pages:
        if (
            page.id <= 0
            or page.capacity <= 0
            or page.capacity > MAX_CAP
            or page.used > page.capacity
            or page.capacity % ALIGNMENT
            or page.used % ALIGNMENT
            or page.lifetime not in LIFETIMES
            or page.owner_state not in OWNER_STATES
        ):
            return _no_safe("invalidPageMetadata", request)
        if page.id in seen:
            return _no_safe("pageIdentityConflict", request)
        seen.add(page.id)
        if page.owner_state == "active":
            active_view_capacity += page.capacity
    if active_view_capacity != request.resident_bytes:
        return _no_safe("residentMismatch", request)

    result["sliceBytes"] = aligned
    requested = normalize_lifetime(request.lifetime)
    same = conservative = mixed = None

    def better(candidate: dict, current: dict | None) -> bool:
        if current is None:
            return True
        if candidate["slack"] != current["slack"]:
            return candidate["slack"] < current["slack"]
        if candidate["nextUsed"] != current["nextUsed"]:
            return candidate["nextUsed"] > current["nextUsed"]
        return candidate["pageId"] < current["pageId"]

    for page in pages:
        if page.owner_state != "active":
            continue
        if page.map_epoch != request.map_epoch or page.device_epoch != request.device_epoch:
            continue
        if aligned > page.capacity - page.used:
            continue
        candidate = {
            "pageId": page.id,
            "offset": page.used,
            "nextUsed": page.used + aligned,
            "slack": page.capacity - (page.used + aligned),
        }
        page_lifetime = normalize_lifetime(page.lifetime)
        if page_lifetime == requested:
            if better(candidate, same):
                same = candidate
        elif (not request.same_retention_intent_only
              and may_share(page_lifetime, requested)):
            if better(candidate, conservative):
                conservative = candidate
        else:
            if better(candidate, mixed):
                mixed = candidate

    def existing(candidate: dict, reason: str, mixed_borrow: bool, conservative_page: bool) -> dict:
        out = dict(result)
        out.update(
            {
                "kind": "existingPage",
                "reason": reason,
                "pageId": candidate["pageId"],
                "offset": candidate["offset"],
                "nextUsed": candidate["nextUsed"],
                "pageBytes": 0,
                "residentBytesAfter": request.resident_bytes,
                "mixedLifetimeBorrow": mixed_borrow,
                "conservativeUnknownPage": conservative_page,
            }
        )
        return out

    if same is not None:
        return existing(same, "sameLifetime", False, False)
    if conservative is not None:
        return existing(conservative, "conservativeUnknownPage", False, True)

    create_reason = "noSafeSelection"
    if not request.page_create_gate_open:
        create_reason = "createGateClosed"
    elif len(pages) >= request.max_pages:
        create_reason = "pageVectorLimit"
    else:
        new_page_bytes = page_capacity(aligned, request.cap_bytes)
        if new_page_bytes == 0 or new_page_bytes > request.cap_bytes - request.resident_bytes:
            create_reason = "sharedCapReached"
        else:
            out = dict(result)
            out.update(
                {
                    "kind": "newPage",
                    "reason": "newPageForLifetime",
                    "pageId": 0,
                    "offset": 0,
                    "nextUsed": aligned,
                    "pageBytes": new_page_bytes,
                    "residentBytesAfter": request.resident_bytes + new_page_bytes,
                }
            )
            return out
    if mixed is not None:
        return existing(mixed, "mixedLifetimeBorrow", True, False)
    result["reason"] = create_reason
    return result


class Stage11LifetimePagePolicyStaticTest(unittest.TestCase):
    def test_same_lifetime_tail_and_alias_accounting(self):
        pages = [Page(1, 0, PAGE_BYTES, "short")]
        first = reference_plan(Request(4096, "short", PAGE_BYTES), pages)
        self.assertEqual((first["kind"], first["pageId"], first["offset"]), ("existingPage", 1, 0))
        pages[0].used = first["nextUsed"]
        second = reference_plan(Request(1, "short", PAGE_BYTES), pages)
        self.assertEqual((second["offset"], second["sliceBytes"]), (4096, 256))
        pages[0].used = second["nextUsed"]
        third = reference_plan(Request(512, "short", PAGE_BYTES), pages)
        self.assertEqual((third["offset"], third["nextUsed"]), (4352, 4864))

    def test_static_anchor_dynamic_rotation_and_retire_pending(self):
        pages = [Page(1, 256, PAGE_BYTES, "long"), Page(2, 0, PAGE_BYTES, "short")]
        dynamic = reference_plan(Request(4096, "short", 2 * PAGE_BYTES), pages)
        self.assertEqual(dynamic["pageId"], 2)
        pages[1].used = dynamic["nextUsed"]
        long_slice = reference_plan(Request(4096, "long", 2 * PAGE_BYTES), pages)
        self.assertEqual(long_slice["pageId"], 1)
        pages[0].used = long_slice["nextUsed"]
        pages[1].owner_state = "retirePending"
        rotated = reference_plan(Request(4096, "short", PAGE_BYTES), pages)
        self.assertEqual(rotated["kind"], "newPage")

    def test_unknown_is_conservative_and_borrow_is_explicit(self):
        unknown = [Page(9, 0, PAGE_BYTES, "unknown")]
        on_unknown = reference_plan(Request(256, "short", PAGE_BYTES), unknown)
        self.assertEqual(on_unknown["kind"], "existingPage")
        self.assertTrue(on_unknown["conservativeUnknownPage"])
        self.assertFalse(on_unknown["mixedLifetimeBorrow"])
        short_page = [Page(5, 0, PAGE_BYTES, "short")]
        unknown_req = reference_plan(Request(256, "unknown", PAGE_BYTES, False), short_page)
        self.assertEqual(unknown_req["pageId"], 5)
        self.assertTrue(unknown_req["conservativeUnknownPage"])
        long_page = [Page(6, 0, PAGE_BYTES, "long")]
        borrowed = reference_plan(Request(256, "short", PAGE_BYTES, False), long_page)
        self.assertTrue(borrowed["mixedLifetimeBorrow"])
        self.assertEqual(borrowed["reason"], "mixedLifetimeBorrow")

    def test_epoch_switch_and_retirement_refs(self):
        pages = [
            Page(1, 0, PAGE_BYTES, "short", map_epoch=7, device_epoch=9),
            Page(2, 0, PAGE_BYTES, "short", map_epoch=8, device_epoch=9),
            Page(3, 0, PAGE_BYTES, "short", owner_state="retirePending", map_epoch=8, device_epoch=9),
        ]
        request = Request(512, "short", 2 * PAGE_BYTES, map_epoch=8, device_epoch=9)
        current = reference_plan(request, pages)
        self.assertEqual(current["pageId"], 2)
        pages[1].owner_state = "retirePending"
        request.resident_bytes = PAGE_BYTES
        fresh = reference_plan(request, pages)
        self.assertEqual(fresh["kind"], "newPage")
        request.page_create_gate_open = False
        blocked = reference_plan(request, pages)
        self.assertEqual(blocked["reason"], "createGateClosed")

    def test_alignment_overflow_cap_and_page_limit(self):
        tiny = reference_plan(Request(1, "short"), [])
        self.assertEqual((tiny["kind"], tiny["sliceBytes"], tiny["pageBytes"]), ("newPage", 256, PAGE_BYTES))
        overflow = reference_plan(Request((1 << 64) - 1, "short"), [])
        self.assertEqual(overflow["reason"], "invalidRequest")
        full = [Page(1, SHARED_CAP, SHARED_CAP, "short")]
        no_cap = reference_plan(Request(256, "short", SHARED_CAP, cap_bytes=SHARED_CAP), full)
        self.assertEqual(no_cap["reason"], "sharedCapReached")
        no_vector = reference_plan(
            Request(256, "short", SHARED_CAP, max_pages=1, cap_bytes=SHARED_CAP), full
        )
        self.assertEqual(no_vector["reason"], "pageVectorLimit")

    def test_resident_accounting_and_share_matrix(self):
        pages = [Page(11, 256, PAGE_BYTES, "long")]
        existing = reference_plan(Request(256, "long", PAGE_BYTES), pages)
        self.assertEqual(existing["residentBytesAfter"], PAGE_BYTES)
        full = [Page(12, PAGE_BYTES, PAGE_BYTES, "long")]
        created = reference_plan(Request(256, "long", PAGE_BYTES), full)
        self.assertEqual(created["residentBytesAfter"], 2 * PAGE_BYTES)
        self.assertFalse(may_share("short", "long"))
        self.assertTrue(may_share("short", "short"))
        self.assertTrue(may_share("unknown", "long"))

    def test_edge_rejections(self):
        epoch_zero = reference_plan(Request(256, "short", map_epoch=0), [])
        self.assertEqual(epoch_zero["reason"], "invalidEpoch")
        cap_big = reference_plan(Request(256, "short", cap_bytes=MAX_CAP + PAGE_BYTES), [])
        self.assertEqual(cap_big["reason"], "sharedCapOutOfRange")
        cap_small = reference_plan(Request(256, "short", cap_bytes=MIN_CAP - PAGE_BYTES), [])
        self.assertEqual(cap_small["reason"], "sharedCapOutOfRange")
        duplicate = [Page(1, 0, PAGE_BYTES, "short"), Page(1, 0, PAGE_BYTES, "short")]
        dup = reference_plan(Request(256, "short", 2 * PAGE_BYTES), duplicate)
        self.assertEqual(dup["reason"], "pageIdentityConflict")
        conflict = [Page(1, 0, PAGE_BYTES, "short"), Page(1, 0, PAGE_BYTES, "long")]
        conflict_plan = reference_plan(Request(256, "short", 2 * PAGE_BYTES), conflict)
        self.assertEqual(conflict_plan["reason"], "pageIdentityConflict")
        wrong_resident = reference_plan(Request(256, "short", 0), [Page(2, 0, PAGE_BYTES, "short")])
        self.assertEqual(wrong_resident["reason"], "residentMismatch")
        too_many = reference_plan(Request(256, "short", 2 * PAGE_BYTES, max_pages=1), duplicate)
        self.assertEqual(too_many["reason"], "pageVectorLimit")
        self.assertEqual(reference_plan(Request(256, "unknown", 0), [])["kind"], "newPage")

    def test_source_and_doc_contracts(self):
        header = HEADER_PATH.read_text(encoding="utf-8")
        cpp = CPP_PATH.read_text(encoding="utf-8")
        plan = PLAN_PATH.read_text(encoding="utf-8")
        for token in (
            "War3PlanStage11LifetimePage",
            "War3Stage11PageLifetime",
            "War3Stage11PageOwnerState",
            "RetirePending",
            "War3Stage11LifetimeClassesMaySharePage",
            "War3Stage11ClassifyPageLifetime",
            "sameRetentionIntentOnly",
            "InvalidEpoch",
            "PageIdentityConflict",
            "ResidentMismatch",
            "SharedCapOutOfRange",
            "kWar3Stage11SnapshotPageBytes",
        ):
            self.assertIn(token, header)
        for forbidden in (
            "vkFreeMemory",
            "vkCmdCopyBuffer",
            "copyBuffer",
            "memcpy",
            "createBuffer",
            "std::free",
            "erase(",
        ):
            self.assertNotIn(forbidden, header)
        for case in (
            "TestAliasAndSharedPageTail",
            "TestStaticAnchorAndDynamicRotation",
            "TestFailedCaptureRollback",
            "TestEpochSwitchAndRetirementRefs",
            "TestAlignmentOverflowFullAndUnknown",
            "TestDoubleCountAccounting",
            "TestPolicyEdgeRejections",
            "TestStrictRetentionIntentSeparation",
        ):
            self.assertIn(case, cpp)
        for token in (
            "P1b",
            "same page",
            "failed capture",
            "Unknown",
            "borrow",
            "RetirePending",
            "synthetic",
            "not applied",
        ):
            self.assertIn(token, plan)


class LifetimeSimulationTests(unittest.TestCase):
    """Fixed-seed allocation/retention/release fixture (synthetic model)."""

    @staticmethod
    def _make_requests(scenario: int, seed: int) -> list[dict]:
        rng = __import__("random").Random(seed)
        requests = []
        count = 40 if scenario in (1, 5) else 80
        for step in range(count):
            if scenario == 0:
                long_anchor = step % 16 == 0
                lifetime = "long" if long_anchor else "short"
                size = 4096 if long_anchor else 8 * 1024 * 1024
            elif scenario == 1:
                lifetime, size = "long", 16 * 1024 * 1024
            elif scenario == 2:
                lifetime, size = "short", 8 * 1024 * 1024
            elif scenario == 3:
                lifetime = "long" if step % 7 == 0 else "unknown"
                size = 8 * 1024 * 1024
            elif scenario == 4:
                lifetime = "short" if step % 2 == 0 else "unknown"
                size = 8 * 1024 * 1024
            else:
                lifetime, size = "long", 16 * 1024 * 1024
            if scenario == 1:
                cpu_release = 1 << 60
                gpu_release = 1 << 60
            elif scenario == 0:
                if lifetime == "long":
                    cpu_release = 1 << 60
                else:
                    cpu_release = step + 20
                gpu_release = cpu_release + 1 + rng.randrange(0, 4)
            elif scenario in (2, 3):
                cpu_release = step + 20
                gpu_release = cpu_release + 1 + rng.randrange(0, 4)
            else:
                cpu_release = step + 2
                gpu_release = cpu_release + 1 + rng.randrange(0, 4)
            if scenario == 4 and rng.random() < 0.25:
                gpu_release = 1 << 60
            if scenario == 5:
                gpu_release = 1 << 60
            requests.append({
                "size": size,
                "lifetime": lifetime,
                "cpu_release": cpu_release,
                "gpu_release": gpu_release,
            })
        return requests

    @staticmethod
    def _simulate(requests: list[dict], grouped: bool) -> dict:
        pages: list[Page] = []
        slices = []
        admitted = rejected = 0
        hole_reuse = early_reclaim = 0
        peak_active_capacity = 0
        peak_gpu_pending_bytes = 0
        peak_physical_backing = 0
        next_page_id = 1
        max_release = max(
            request["gpu_release"] for request in requests
        ) if requests else 0
        total_steps = len(requests) + min(max_release, 10000) + 2
        for step in range(total_steps):
            for slice in slices:
                page = next((p for p in pages if p.id == slice["page_id"]), None)
                if page is None:
                    continue
                if slice["cpu_release"] == step:
                    page.cpu_refs = max(0, page.cpu_refs - 1)
                if slice["gpu_release"] == step:
                    page.gpu_refs = max(0, page.gpu_refs - 1)
            for page in pages:
                if page.owner_state == "retired":
                    continue
                if page.cpu_refs > 0:
                    page.owner_state = "active"
                elif page.gpu_refs > 0:
                    page.owner_state = "retirePending"
                else:
                    if page.gpu_refs != 0:
                        early_reclaim += 1
                    page.owner_state = "retired"
            resident = sum(
                page.capacity for page in pages
                if page.owner_state == "active"
            )
            peak_active_capacity = max(peak_active_capacity, resident)
            peak_gpu_pending_bytes = max(
                peak_gpu_pending_bytes,
                sum(slice["size"] for slice in slices
                    if slice["gpu_release"] > step),
            )
            peak_physical_backing = max(
                peak_physical_backing,
                sum(page.capacity for page in pages),
            )
            if step >= len(requests):
                continue
            request = requests[step]
            request_lifetime = request["lifetime"] if grouped else "unknown"
            plan = reference_plan(
                Request(
                    request["size"],
                    request_lifetime,
                    resident,
                    cap_bytes=SHARED_CAP,
                ),
                pages,
            )
            if plan["kind"] == "noSafeSelection":
                rejected += 1
                continue
            if plan["kind"] == "existingPage":
                page = next(
                    (p for p in pages if p.id == plan["pageId"]), None)
                if page is None:
                    rejected += 1
                    continue
                if plan["offset"] != page.used:
                    hole_reuse += 1
                page.used = plan["nextUsed"]
                page_id = page.id
            else:
                page = Page(
                    next_page_id,
                    used=plan["nextUsed"],
                    capacity=plan["pageBytes"],
                    lifetime=request_lifetime,
                )
                page.cpu_refs = 0
                page.gpu_refs = 0
                pages.append(page)
                page_id = next_page_id
                next_page_id += 1
            page = next(p for p in pages if p.id == page_id)
            page.cpu_refs += 1
            page.gpu_refs += 1
            slices.append({
                "page_id": page_id,
                "size": request["size"],
                "cpu_release": request["cpu_release"],
                "gpu_release": request["gpu_release"],
            })
            admitted += 1
            peak_physical_backing = max(
                peak_physical_backing,
                sum(page.capacity for page in pages),
            )
        active_capacity = sum(
            page.capacity for page in pages if page.owner_state == "active"
        )
        peak_active_capacity = max(peak_active_capacity, active_capacity)
        physical_backing = sum(page.capacity for page in pages)
        peak_physical_backing = max(peak_physical_backing, physical_backing)
        gpu_pending_bytes = sum(
            slice["size"] for slice in slices
            if slice["gpu_release"] > total_steps - 1
        )
        peak_gpu_pending_bytes = max(peak_gpu_pending_bytes, gpu_pending_bytes)
        return {
            "admitted": admitted,
            "rejected": rejected,
            "hole_reuse": hole_reuse,
            "early_reclaim": early_reclaim,
            "active_page_capacity": active_capacity,
            "peak_active_capacity": peak_active_capacity,
            "physical_backing_final": physical_backing,
            "peak_physical_backing": peak_physical_backing,
            "gpu_pending_bytes": gpu_pending_bytes,
            "peak_gpu_pending_bytes": peak_gpu_pending_bytes,
            "page_count": len(pages),
        }

    def test_fixed_seed_two_strategy_safety(self):
        results = []
        for scenario in range(6):
            requests = self._make_requests(scenario, 0x20260921 + scenario)
            mixed = self._simulate(requests, grouped=False)
            grouped = self._simulate(requests, grouped=True)
            for result in (mixed, grouped):
                self.assertEqual(result["hole_reuse"], 0)
                self.assertEqual(result["early_reclaim"], 0)
                self.assertLessEqual(result["page_count"], 32)
                self.assertGreaterEqual(result["active_page_capacity"], 0)
                self.assertGreaterEqual(result["gpu_pending_bytes"], 0)
            results.append((scenario, mixed, grouped))
        # Keep the comparison explicit but do not turn the synthetic model
        # into a player-visible performance claim.
        for scenario, mixed, grouped in results:
            self.assertIsInstance(mixed["admitted"], int)
            self.assertIsInstance(grouped["admitted"], int)
        self.assertEqual(len(results), 6)


FOREVER = 1 << 60


class LiveByteLedger:
    """Independent allocated/freed/live-byte oracle for the model."""

    def __init__(self):
        self.allocated = 0
        self.freed = 0
        self.live_peak = 0
        self.checks = 0

    @property
    def live(self):
        return self.allocated - self.freed

    def allocate(self, byte_count):
        if byte_count <= 0:
            raise ValueError("allocate must be positive")
        self.allocated += byte_count
        self.checks += 1
        self.live_peak = max(self.live_peak, self.live)

    def free(self, byte_count):
        if byte_count <= 0 or byte_count > self.live:
            raise ValueError("free without live backing")
        self.freed += byte_count
        self.checks += 1


class AccountedPage(Page):
    """Page with separate CPU cache refs, CS/GPU completion refs and retire hold."""

    def __init__(self, page_id, capacity, lifetime, created_step):
        super().__init__(page_id, used=0, capacity=capacity, lifetime=lifetime)
        self.created_step = created_step
        self.freed_step = None
        self.cpu_refs = 0
        self.gpu_refs = 0
        self.hold_until = 0
        self.owner_state = "active"


def _accounted_main_demands():
    """Bounded deterministic schedule with all correction cases in one trace."""
    demands = []

    def add(size, lifetime, cpu_delay, gpu_delay, reset_delay=None,
            hold_steps=0, note=""):
        step = len(demands)
        demands.append({
            "step": step,
            "size": size,
            "lifetime": lifetime,
            "cpu_release": FOREVER if cpu_delay is None else step + cpu_delay,
            "gpu_complete": FOREVER if gpu_delay is None else step + gpu_delay,
            "reset_step": FOREVER if reset_delay is None else step + reset_delay,
            "hold_steps": hold_steps,
            "note": note,
        })

    # Tiny long-lived anchor; it intentionally keeps one whole page live.
    add(256, "long", None, None, note="tiny_long_anchor")
    # Large short spans with delayed GPU/CS completion.
    for i in range(3):
        add(PAGE_BYTES - 4096, "short", 2 + i, 12 + i,
            note="large_short_%d" % i)
    # Same completion step, different CPU release steps; if both land on one
    # page the page must remain pending until both CPU refs and both GPU refs
    # are gone.
    add(4 << 20, "short", 3, 18, note="pair_gpu_a")
    add(4 << 20, "short", 7, 18, note="pair_gpu_b")
    # Reset/retired hold: page is held after reset even when both refs finish.
    add(1 << 20, "short", 2, 6, reset_delay=1, hold_steps=8,
        note="reset_retired_hold")
    # Unknown-lifetime pressure and delayed completion.
    for i in range(6):
        add(12 << 20, "unknown", 4, 20, note="unknown_pressure_%d" % i)
    # Repeat a prior demand after the hold page is due to have been freed.
    add(1 << 20, "short", 2, 5, note="repeated_return")
    for i in range(2):
        add(8 << 20, "unknown", 3, 18, note="unknown_tail_%d" % i)
    return demands


def _accounted_pending_overflow_demands():
    """Create > shared-cap live pending backing without exceeding active CPU cap."""
    demands = []
    for i in range(25):
        demands.append({
            "step": i,
            "size": PAGE_BYTES,
            "lifetime": "unknown",
            "cpu_release": i + 1,
            "gpu_complete": i + 50,
            "reset_step": FOREVER,
            "hold_steps": 0,
            "note": "pending_overflow_%d" % i,
        })
    return demands


def _accounted_pair_completion_demands():
    """Two slices on one likely page; same GPU completion, different CPU refs."""
    return [
        {
            "step": 0, "size": 4 << 20, "lifetime": "short",
            "cpu_release": 5, "gpu_complete": 6, "reset_step": FOREVER,
            "hold_steps": 0, "note": "pair_completion_a",
        },
        {
            "step": 1, "size": 4 << 20, "lifetime": "short",
            "cpu_release": 2, "gpu_complete": 6, "reset_step": FOREVER,
            "hold_steps": 0, "note": "pair_completion_b",
        },
    ]


def _accounted_hold_demands():
    """One page moved to reset/retired hold after both ref domains release."""
    return [{
        "step": 0, "size": 1 << 20, "lifetime": "short",
        "cpu_release": 1, "gpu_complete": 2, "reset_step": 1,
        "hold_steps": 5, "note": "reset_hold_page",
    }]

def _accounted_simulate(demands, grouped, skip_release=None):
    """Run the deterministic model with separate CPU/GPU accounting.

    `skip_release` is a test hook only: "cpu" leaves cache refs held, "gpu"
    leaves completion refs held.  It is not a production policy switch.
    """
    if skip_release not in (None, "cpu", "gpu"):
        raise ValueError("invalid skip_release")

    pages = []
    page_by_id = {}
    slices = []
    ledger = LiveByteLedger()
    next_page_id = 1
    admitted = rejected = 0
    freed_pages = 0
    hole_reuse = early_reclaim = premature_free = 0
    alignment_violations = used_over_capacity = oracle_mismatches = 0
    active_peak = retired_pending_peak = retired_hold_peak = 0
    gpu_delayed_observed = pair_completion_observed = hold_observed = False
    unknown_demands = sum(1 for d in demands if d["lifetime"] == "unknown")

    def page_is_active(page):
        return page.cpu_refs > 0

    def page_is_pending(page, step):
        return (
            page.cpu_refs == 0
            and (page.gpu_refs > 0 or page.hold_until > step)
        )

    def page_is_hold(page, step):
        return page.cpu_refs == 0 and page.gpu_refs == 0 and page.hold_until > step

    def refresh_page_states(step):
        nonlocal active_peak, retired_pending_peak, retired_hold_peak
        for page in pages:
            if page_is_active(page):
                page.owner_state = "active"
            else:
                page.owner_state = "retirePending"
        active_now = sum(page.capacity for page in pages if page_is_active(page))
        pending_now = sum(page.capacity for page in pages if page_is_pending(page, step))
        hold_now = sum(page.capacity for page in pages if page_is_hold(page, step))
        active_peak = max(active_peak, active_now)
        retired_pending_peak = max(retired_pending_peak, pending_now)
        retired_hold_peak = max(retired_hold_peak, hold_now)
        return active_now

    def free_completed_pages(step):
        nonlocal freed_pages, premature_free
        for page in list(pages):
            if page.cpu_refs == 0 and page.gpu_refs == 0 and page.hold_until <= step:
                ledger.free(page.capacity)
                page.freed_step = step
                pages.remove(page)
                page_by_id.pop(page.id, None)
                freed_pages += 1
            elif page.cpu_refs < 0 or page.gpu_refs < 0:
                premature_free += 1

    finite_releases = []
    for demand in demands:
        for key in ("cpu_release", "gpu_complete", "reset_step"):
            if demand[key] < FOREVER:
                finite_releases.append(demand[key])
    total_steps = max(finite_releases + [len(demands)]) + max(
        (d["hold_steps"] for d in demands), default=0) + 2

    for step in range(total_steps):
        # Apply reset/retired hold scheduling.
        for item in slices:
            if item["reset_step"] == step and item["hold_steps"] > 0:
                page = page_by_id.get(item["page_id"])
                if page is not None:
                    page.hold_until = max(page.hold_until, step + item["hold_steps"])
                    hold_observed = True

        # CPU cache release domain.
        if skip_release != "cpu":
            for item in slices:
                if not item["cpu_released"] and item["cpu_release"] == step:
                    page = page_by_id.get(item["page_id"])
                    if page is None:
                        continue
                    item["cpu_released"] = True
                    if page.cpu_refs > 0:
                        page.cpu_refs -= 1
                    if item["gpu_complete"] > item["cpu_release"]:
                        gpu_delayed_observed = True

        # CS/GPU completion domain.
        if skip_release != "gpu":
            for item in slices:
                if not item["gpu_completed"] and item["gpu_complete"] == step:
                    page = page_by_id.get(item["page_id"])
                    if page is None:
                        continue
                    item["gpu_completed"] = True
                    if page.gpu_refs > 0:
                        page.gpu_refs -= 1

        refresh_page_states(step)
        free_completed_pages(step)
        refresh_page_states(step)
        if ledger.live != sum(page.capacity for page in pages):
            oracle_mismatches += 1

        if step < len(demands):
            demand = demands[step]
            requested_lifetime = demand["lifetime"] if grouped else "unknown"
            aligned = align_up(demand["size"])
            if aligned is None:
                rejected += 1
                continue
            if aligned % ALIGNMENT:
                alignment_violations += 1
            active_resident = sum(
                page.capacity for page in pages if page_is_active(page)
            )
            plan = reference_plan(
                Request(
                    aligned,
                    requested_lifetime,
                    active_resident,
                    cap_bytes=SHARED_CAP,
                ),
                pages,
            )
            if plan["kind"] == "noSafeSelection":
                rejected += 1
                continue
            if plan["kind"] == "existingPage":
                page = page_by_id[plan["pageId"]]
                if plan["offset"] != page.used:
                    hole_reuse += 1
                page.used = plan["nextUsed"]
            else:
                page = AccountedPage(
                    next_page_id,
                    plan["pageBytes"],
                    requested_lifetime,
                    step,
                )
                next_page_id += 1
                pages.append(page)
                page_by_id[page.id] = page
                page.used = plan["nextUsed"]
                ledger.allocate(page.capacity)
            if page.used > page.capacity:
                used_over_capacity += 1
            page.cpu_refs += 1
            page.gpu_refs += 1
            slices.append({
                "id": len(slices) + 1,
                "page_id": page.id,
                "size": aligned,
                "cpu_release": demand["cpu_release"],
                "gpu_complete": demand["gpu_complete"],
                "reset_step": demand["reset_step"],
                "hold_steps": demand["hold_steps"],
                "cpu_released": False,
                "gpu_completed": False,
            })
            admitted += 1
            refresh_page_states(step)

    # Final independent closure checks.
    final_page_bytes = sum(page.capacity for page in pages)
    if ledger.live != final_page_bytes:
        oracle_mismatches += 1
    for page in pages:
        if page.used > page.capacity:
            used_over_capacity += 1
        if page.used % ALIGNMENT:
            alignment_violations += 1

    for page_id in set(item["page_id"] for item in slices):
        page_slices = [item for item in slices if item["page_id"] == page_id]
        for i in range(len(page_slices)):
            for j in range(i + 1, len(page_slices)):
                left, right = page_slices[i], page_slices[j]
                if (left["gpu_complete"] == right["gpu_complete"]
                        and left["cpu_release"] != right["cpu_release"]):
                    pair_completion_observed = True

    demand_signature = tuple(
        (d["step"], d["size"], d["lifetime"], d["cpu_release"],
         d["gpu_complete"], d["reset_step"], d["hold_steps"])
        for d in demands
    )
    return {
        "strategy": "grouped" if grouped else "mixed",
        "demand_count": len(demands),
        "unknown_demands": unknown_demands,
        "demand_signature": demand_signature,
        "admitted": admitted,
        "rejected": rejected,
        "page_creations": next_page_id - 1,
        "page_frees": freed_pages,
        "allocated_total": ledger.allocated,
        "freed_total": ledger.freed,
        "live_final": ledger.live,
        "live_peak": ledger.live_peak,
        "active_peak": active_peak,
        "active_final": sum(page.capacity for page in pages if page_is_active(page)),
        "retired_pending_peak": retired_pending_peak,
        "retired_pending_final": sum(
            page.capacity for page in pages if page_is_pending(page, total_steps)
        ),
        "retired_hold_peak": retired_hold_peak,
        "hole_reuse": hole_reuse,
        "early_reclaim": early_reclaim,
        "premature_free": premature_free,
        "alignment_violations": alignment_violations,
        "used_over_capacity": used_over_capacity,
        "oracle_mismatches": oracle_mismatches,
        "ledger_checks": ledger.checks,
        "gpu_delayed_observed": gpu_delayed_observed,
        "pair_completion_observed": pair_completion_observed,
        "hold_observed": hold_observed,
        "soft_cap": SHARED_CAP,
        "total_steps": total_steps,
    }


class AccountedLifetimeModelTests(unittest.TestCase):
    """Corrected accounting model; separate from the historical six scenarios."""

    def test_separates_active_pending_hold_and_freed_live_bytes(self):
        demands = _accounted_main_demands()
        grouped = _accounted_simulate(demands, grouped=True)
        mixed = _accounted_simulate(demands, grouped=False)
        for result in (grouped, mixed):
            self.assertEqual(result["oracle_mismatches"], 0)
            self.assertEqual(result["alignment_violations"], 0)
            self.assertEqual(result["used_over_capacity"], 0)
            self.assertEqual(result["hole_reuse"], 0)
            self.assertEqual(result["early_reclaim"], 0)
            self.assertEqual(result["premature_free"], 0)
            self.assertEqual(
                result["allocated_total"],
                result["freed_total"] + result["live_final"],
            )
            self.assertGreater(result["freed_total"], 0)
            self.assertGreater(result["allocated_total"], result["live_final"])
            self.assertGreater(result["retired_pending_peak"], 0)
            self.assertTrue(result["gpu_delayed_observed"])
            self.assertTrue(result["hold_observed"])
            self.assertLessEqual(result["active_peak"], SHARED_CAP)
            self.assertGreater(result["unknown_demands"], 0)
        self.assertEqual(grouped["demand_signature"], mixed["demand_signature"])

    def test_same_completion_different_cpu_refs_stay_pending(self):
        result = _accounted_simulate(
            _accounted_pair_completion_demands(), grouped=True
        )
        self.assertEqual(result["page_creations"], 1)
        self.assertTrue(result["pair_completion_observed"])
        self.assertGreater(result["retired_pending_peak"], 0)
        self.assertEqual(result["freed_total"], PAGE_BYTES)
        self.assertEqual(result["live_final"], 0)
        self.assertEqual(result["premature_free"], 0)

    def test_reset_retired_hold_keeps_completed_page_live(self):
        result = _accounted_simulate(_accounted_hold_demands(), grouped=True)
        self.assertTrue(result["hold_observed"])
        self.assertGreater(result["retired_hold_peak"], 0)
        self.assertEqual(result["freed_total"], PAGE_BYTES)
        self.assertEqual(result["live_final"], 0)
        self.assertEqual(result["premature_free"], 0)
    def test_pending_backing_can_exceed_soft_pool_cap(self):
        demands = _accounted_pending_overflow_demands()
        result = _accounted_simulate(demands, grouped=True)
        self.assertEqual(result["page_creations"], 25)
        self.assertEqual(result["allocated_total"], 25 * PAGE_BYTES)
        self.assertEqual(result["freed_total"], 25 * PAGE_BYTES)
        self.assertEqual(result["live_final"], 0)
        self.assertEqual(result["unknown_demands"], 25)
        self.assertLessEqual(result["active_peak"], SHARED_CAP)
        self.assertGreater(result["live_peak"], SHARED_CAP)
        self.assertEqual(result["hole_reuse"], 0)
        self.assertEqual(result["early_reclaim"], 0)
        self.assertEqual(result["premature_free"], 0)

    def test_missing_release_domains_do_not_free_pages(self):
        demands = [{
            "step": 0,
            "size": 8 << 20,
            "lifetime": "short",
            "cpu_release": 1,
            "gpu_complete": 1,
            "reset_step": FOREVER,
            "hold_steps": 0,
            "note": "missing-domain-negative",
        }]
        normal = _accounted_simulate(demands, grouped=True)
        skip_gpu = _accounted_simulate(demands, grouped=True, skip_release="gpu")
        skip_cpu = _accounted_simulate(demands, grouped=True, skip_release="cpu")
        self.assertGreater(normal["freed_total"], 0)
        self.assertEqual(normal["live_final"], 0)
        self.assertEqual(skip_gpu["freed_total"], 0)
        self.assertGreater(skip_gpu["live_final"], 0)
        self.assertGreater(skip_gpu["retired_pending_peak"], 0)
        self.assertEqual(skip_cpu["freed_total"], 0)
        self.assertGreater(skip_cpu["live_final"], 0)
        self.assertGreater(skip_cpu["active_final"], 0)

    def test_live_byte_ledger_is_independent_and_nonnegative(self):
        ledger = LiveByteLedger()
        ledger.allocate(10)
        self.assertEqual((ledger.allocated, ledger.freed, ledger.live), (10, 0, 10))
        ledger.free(4)
        self.assertEqual((ledger.allocated, ledger.freed, ledger.live), (10, 4, 6))
        self.assertEqual(ledger.live_peak, 10)
        with self.assertRaises(ValueError):
            ledger.free(7)

class StrictRetentionIntentReferenceTests(unittest.TestCase):
    def test_strict_intent_separation_counterexamples(self):
        unknown = [Page(9, 0, PAGE_BYTES, "unknown")]
        compat = reference_plan(Request(256, "long", PAGE_BYTES), unknown)
        self.assertEqual(compat["kind"], "existingPage")
        self.assertTrue(compat["conservativeUnknownPage"])

        strict = Request(256, "long", PAGE_BYTES, same_retention_intent_only=True)
        create_over_unknown = reference_plan(strict, unknown)
        self.assertEqual(create_over_unknown["kind"], "newPage")
        self.assertFalse(create_over_unknown["conservativeUnknownPage"])
        self.assertFalse(create_over_unknown["mixedLifetimeBorrow"])

        short_page = [Page(5, 0, PAGE_BYTES, "short")]
        create_over_short = reference_plan(strict, short_page)
        self.assertEqual(create_over_short["kind"], "newPage")
        self.assertFalse(create_over_short["mixedLifetimeBorrow"])

        blocked = Request(256, "long", PAGE_BYTES, page_create_gate_open=False,
                          same_retention_intent_only=True)
        borrow_unknown = reference_plan(blocked, unknown)
        self.assertEqual(borrow_unknown["kind"], "existingPage")
        self.assertEqual(borrow_unknown["pageId"], 9)
        self.assertTrue(borrow_unknown["mixedLifetimeBorrow"])
        self.assertFalse(borrow_unknown["conservativeUnknownPage"])

        full_opposite = [Page(6, 0, SHARED_CAP, "short")]
        cap_full = Request(256, "long", SHARED_CAP, cap_bytes=SHARED_CAP,
                           same_retention_intent_only=True)
        borrow_cap_full = reference_plan(cap_full, full_opposite)
        self.assertEqual(borrow_cap_full["kind"], "existingPage")
        self.assertEqual(borrow_cap_full["pageId"], 6)
        self.assertTrue(borrow_cap_full["mixedLifetimeBorrow"])
        self.assertEqual(borrow_cap_full["reason"], "mixedLifetimeBorrow")

        large = reference_plan(
            Request(20 << 20, "long", same_retention_intent_only=True), [])
        self.assertEqual(large["kind"], "newPage")
        self.assertEqual(large["nextUsed"], 20 << 20)
        self.assertEqual(large["pageBytes"], 32 << 20)

        bad_epoch = reference_plan(
            Request(256, "long", map_epoch=0,
                    same_retention_intent_only=True), [])
        self.assertEqual(bad_epoch["reason"], "invalidEpoch")

        old_tail = reference_plan(
            Request(256, "long", PAGE_BYTES, page_create_gate_open=False,
                    same_retention_intent_only=True), unknown)
        self.assertEqual(old_tail["kind"], "existingPage")
        self.assertEqual(old_tail["pageId"], 9)
        self.assertTrue(old_tail["mixedLifetimeBorrow"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
