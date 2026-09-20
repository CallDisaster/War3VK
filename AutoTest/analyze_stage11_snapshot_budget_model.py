"""Offline policy model, NOT a production allocator/GPU test or gameplay trace.

Mirrors Stage11's aligned tail allocation + whole-page CPU-owner collection.
Compares one shared budget with optional lifetime-compatible page selection.
Synthetic GPU retention is explicit; no address reuse or GPU calls occur.
"""
from dataclasses import dataclass, field
import argparse
import hashlib
import json
from pathlib import Path

MIB = 1024 * 1024
ALIGN = 256
PAGE = 16 * MIB
CAP = 384 * MIB
U64_MAX = (1 << 64) - 1


def align(value):
    if value <= 0 or value > U64_MAX - (ALIGN - 1):
        raise ValueError("invalid or overflowing byte request")
    return (value + ALIGN - 1) & -ALIGN


@dataclass
class Slice:
    page: int
    offset: int
    requested: int
    capacity: int
    owners: int = 1


@dataclass
class Page:
    identity: int
    lifetime: str
    capacity: int
    used: int = 0
    slices: list = field(default_factory=list)
    gpu_uses: set = field(default_factory=set)


class Pool:
    """Active page budget matches product scope; retained backing is separate."""

    def __init__(self, *, cap=CAP, page=PAGE, grouped=False, quotas=None):
        self.cap = cap
        self.page_size = page
        self.grouped = grouped
        self.quotas = quotas or {}
        self.pages = []
        self.retired = []
        self.slices = []
        self.next_page = 1
        self.peak_active = 0
        self.created = 0
        self.reclaimed = 0
        self.rejected = 0

    def allocate(self, requested, lifetime):
        capacity = align(requested)

        def fit():
            for page in reversed(self.pages):
                if self.grouped and page.lifetime != lifetime:
                    continue
                if capacity <= page.capacity - page.used:
                    return page
            return None

        page = fit()
        if page is None:
            self.collect()
            page = fit()
        if page is None:
            page_bytes = ((capacity + self.page_size - 1) // self.page_size) * self.page_size
            resident = sum(p.capacity for p in self.pages)
            class_bytes = sum(p.capacity for p in self.pages if p.lifetime == lifetime)
            if page_bytes > self.cap - resident or page_bytes > self.quotas.get(lifetime, self.cap) - class_bytes:
                self.rejected += 1
                return None
            page = Page(self.next_page, lifetime, page_bytes)
            self.next_page += 1
            self.pages.append(page)
            self.created += 1
            self.peak_active = max(self.peak_active, resident + page_bytes)
        result = Slice(page.identity, page.used, requested, capacity)
        page.used += capacity
        page.slices.append(result)
        self.slices.append(result)
        return result

    def alias(self, allocation):
        assert allocation.owners > 0
        allocation.owners += 1
        return allocation

    def release(self, allocation):
        assert allocation.owners > 0
        allocation.owners -= 1

    def submit(self, allocation, completion):
        assert completion > 0
        page = next(p for p in self.pages if p.identity == allocation.page)
        page.gpu_uses.add(completion)

    def complete(self, completion):
        for page in self.pages + self.retired:
            page.gpu_uses = {s for s in page.gpu_uses if s > completion}
        self.retired = [p for p in self.retired if p.gpu_uses]

    def collect(self):
        kept = []
        for page in self.pages:
            if any(s.owners for s in page.slices):
                kept.append(page)
            else:
                self.reclaimed += 1
                if page.gpu_uses:
                    self.retired.append(page)
        self.pages = kept

    def stats(self):
        resident = sum(p.capacity for p in self.pages)
        used = sum(p.used for p in self.pages)
        live = sum(s.capacity for p in self.pages for s in p.slices if s.owners)
        pending = sum(p.capacity for p in self.retired)
        return dict(activePages=len(self.pages), resident=resident, used=used,
                    cpuOwnedAlignedBytes=live, cpuUnownedUsedBytes=used - live,
                    tail=resident - used, retainedAfterPageRelease=pending,
                    modeledBacking=resident + pending, peakActive=self.peak_active,
                    created=self.created, reclaimed=self.reclaimed, rejected=self.rejected)


def anchor_scenario(grouped=False, cap=CAP, count=24):
    pool = Pool(grouped=grouped, cap=cap)
    completed = 0
    for _ in range(count):
        anchor = pool.allocate(ALIGN, "long")
        bulk = pool.allocate(PAGE - ALIGN, "short")
        if anchor is None or bulk is None:
            break
        # Synthetic short slice is already finished on GPU before CPU release.
        pool.release(bulk)
        pool.collect()
        completed += 1
    before = pool.stats()
    accepted = pool.allocate(512 * 1024, "short") is not None
    return dict(completedCycles=completed, before=before, next512KiBAccepted=accepted,
                after=pool.stats())


def range_scenario(position_bytes):
    pool = Pool()
    for _ in range(400):
        assert pool.allocate(position_bytes, "same-epoch") is not None
        assert pool.allocate(210, "same-epoch") is not None
    return pool.stats()


def scenarios():
    unified = Pool(grouped=True)
    hard_split = Pool(grouped=True, quotas={"long": 64 * MIB, "short": 320 * MIB})
    live = Pool(grouped=True)
    results = {
        "mixed24": anchor_scenario(),
        "grouped24": anchor_scenario(True),
        "mixed48BiggerBudget": anchor_scenario(False, 768 * MIB, 48),
        "fullRange400": range_scenario(512 * 1024),
        "provenSmallRange400": range_scenario(52 * 32),
        "hardSplit65MiB": dict(sharedAccepted=unified.allocate(65 * MIB, "long") is not None,
                                fixedAccepted=hard_split.allocate(65 * MIB, "long") is not None),
    }
    accepted = sum(live.allocate(PAGE, "required") is not None for _ in range(25))
    results["trueRequiredOverCap"] = dict(accepted=accepted, attempted=25, account=live.stats())
    pending = Pool(cap=PAGE)
    old = pending.allocate(PAGE, "short")
    pending.submit(old, 9)
    pending.release(old)
    pending.collect()
    after_release = pending.stats()
    new = pending.allocate(PAGE, "short")
    assert old.page != new.page
    after_allocate = pending.stats()
    pending.complete(9)
    results["cpuReleaseIsNotGpuCompletion"] = dict(afterRelease=after_release,
            afterNewAllocation=after_allocate, afterCompletion=pending.stats())
    return results


def source_pins(root):
    names = ["src/d3d9/war3/render/war3_stage11_snapshot_page_policy.h",
             "src/d3d9/war3/render/war3_draw_time_snapshot_lifetime.h",
             "src/d3d9/war3/render/war3_shadow_drawtime_cache_policy.h",
             "src/d3d9/d3d9_device.cpp", "src/d3d9/d3d9_common_buffer.h"]
    header = (root / names[0]).read_text(encoding="utf-8")
    for declaration in ["kWar3Stage11SnapshotAlignment = 256u;",
                        "kWar3Stage11SnapshotResidentCapBytes = 384u << 20u;",
                        "kWar3Stage11SnapshotPageBytes = 16u << 20u;"]:
        if declaration not in header:
            raise ValueError("policy constants changed; re-audit model")
    return {name: hashlib.sha256((root / name).read_bytes()).hexdigest().upper() for name in names}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    result = dict(scope="synthetic policy mirror; not production execution or runtime benefit",
                  sourcePins=source_pins(root), scenarios=scenarios(),
                  limits=["No actual GPU memory, fence, allocator lock or D3D call is exercised.",
                          "Inputs are synthetic, not the BF938 player's per-page ownership trace.",
                          "Page-create throttle, cache TTL and binding classification are not simulated.",
                          "No memcpy throughput, CPU/GPU latency or fragmentation prevalence is measured.",
                          "Backing retention model has assumed completion events; it is not a Vulkan proof."])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8") as handle:
        json.dump(result, handle, indent=2, allow_nan=False)
    print(json.dumps(result["scenarios"], indent=2))


if __name__ == "__main__":
    main()
