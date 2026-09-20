"""Model tests only. Not product allocator, GPU lifetime or player acceptance."""
import random
import unittest
from analyze_stage11_snapshot_budget_model import ALIGN, CAP, MIB, PAGE, U64_MAX, Pool, align, scenarios


class SnapshotBudgetModelTests(unittest.TestCase):
    def test_alignment(self):
        for size, expected in [(1, 256), (256, 256), (257, 512), (1664, 1792)]:
            self.assertEqual(align(size), expected)

    def test_bad_sizes(self):
        for size in [-1, 0, U64_MAX, U64_MAX - 254]:
            with self.assertRaises(ValueError):
                align(size)

    def test_alias_is_not_double_counted(self):
        pool = Pool()
        position = pool.allocate(100, "short")
        uv = pool.alias(position)
        self.assertEqual(pool.stats()["cpuOwnedAlignedBytes"], ALIGN)
        pool.release(position)
        pool.collect()
        self.assertEqual(pool.stats()["activePages"], 1)
        pool.release(uv)
        pool.collect()
        self.assertEqual(pool.stats()["resident"], 0)

    def test_freed_hole_is_not_reused(self):
        pool = Pool()
        first = pool.allocate(1024, "short")
        pool.allocate(256, "long")
        pool.release(first)
        after = pool.allocate(1024, "short")
        self.assertEqual(after.offset, 1280)
        self.assertEqual(pool.stats()["cpuUnownedUsedBytes"], 1024)

    def test_failure_does_not_evict_required_owners(self):
        pool = Pool(cap=PAGE)
        allocation = pool.allocate(PAGE, "required")
        self.assertIsNone(pool.allocate(256, "required"))
        self.assertEqual(allocation.owners, 1)
        self.assertEqual(pool.stats()["used"], PAGE)

    def test_newest_fit_order(self):
        pool = Pool(cap=2 * PAGE)
        first = pool.allocate(PAGE - 512, "short")
        second = pool.allocate(1024, "short")
        third = pool.allocate(256, "short")
        self.assertNotEqual(first.page, second.page)
        self.assertEqual(second.page, third.page)

    def test_collection_checks_all_pages(self):
        pool = Pool(cap=2 * PAGE)
        old = pool.allocate(PAGE, "short")
        new = pool.allocate(PAGE, "short")
        pool.release(old)
        replacement = pool.allocate(256, "short")
        self.assertNotEqual(replacement.page, old.page)
        self.assertEqual(new.owners, 1)
        self.assertEqual(pool.stats()["reclaimed"], 1)

    def test_boundary_and_large_page_rounding(self):
        pool = Pool()
        self.assertIsNotNone(pool.allocate(PAGE + 1, "long"))
        self.assertEqual(pool.stats()["resident"], 2 * PAGE)
        self.assertEqual(pool.stats()["used"], PAGE + ALIGN)

    def test_unknown_gpu_completion_does_not_reuse_old_page(self):
        pool = Pool(cap=PAGE)
        old = pool.allocate(PAGE, "short")
        pool.submit(old, 2)
        pool.submit(old, 5)
        pool.release(old)
        pool.collect()
        new = pool.allocate(PAGE, "short")
        self.assertNotEqual(old.page, new.page)
        pool.complete(2)
        self.assertEqual(pool.stats()["modeledBacking"], 2 * PAGE)
        pool.complete(5)
        self.assertEqual(pool.stats()["modeledBacking"], PAGE)

    def test_anchor_pressure_and_shared_budget(self):
        result = scenarios()
        mixed = result["mixed24"]
        grouped = result["grouped24"]
        self.assertEqual(mixed["before"]["cpuOwnedAlignedBytes"], 6144)
        self.assertEqual(mixed["before"]["resident"], CAP)
        self.assertFalse(mixed["next512KiBAccepted"])
        self.assertEqual(grouped["before"]["cpuOwnedAlignedBytes"], 6144)
        self.assertEqual(grouped["before"]["resident"], PAGE)
        self.assertTrue(grouped["next512KiBAccepted"])
        self.assertEqual(grouped["after"]["resident"], 2 * PAGE)

    def test_increase_cap_only_delays_anchor_failure(self):
        result = scenarios()["mixed48BiggerBudget"]
        self.assertEqual(result["before"]["resident"], 768 * MIB)
        self.assertFalse(result["next512KiBAccepted"])

    def test_fixed_split_strands_headroom(self):
        result = scenarios()["hardSplit65MiB"]
        self.assertTrue(result["sharedAccepted"])
        self.assertFalse(result["fixedAccepted"])

    def test_required_set_cannot_be_fixed_by_grouping(self):
        result = scenarios()["trueRequiredOverCap"]
        self.assertEqual(result["accepted"], 24)
        self.assertEqual(result["account"]["cpuOwnedAlignedBytes"], CAP)

    def test_range_sizes_include_alignment_and_ib(self):
        result = scenarios()
        self.assertEqual(result["fullRange400"]["cpuOwnedAlignedBytes"], 400 * (524288 + 256))
        self.assertEqual(result["provenSmallRange400"]["cpuOwnedAlignedBytes"], 400 * (1792 + 256))
        self.assertEqual(result["fullRange400"]["resident"], 208 * MIB)
        self.assertEqual(result["provenSmallRange400"]["resident"], 16 * MIB)

    def test_seeded_account_closure(self):
        rng = random.Random(0x20260919)
        for grouped in (False, True):
            pool = Pool(grouped=grouped, cap=4 * PAGE)
            owned = []
            for _ in range(2000):
                if owned and rng.randrange(3) == 0:
                    pool.release(owned.pop(rng.randrange(len(owned))))
                else:
                    allocation = pool.allocate(rng.randrange(1, MIB), rng.choice(["long", "short"]))
                    if allocation:
                        owned.append(allocation)
                if rng.randrange(9) == 0:
                    pool.collect()
                stats = pool.stats()
                self.assertEqual(stats["resident"], stats["cpuOwnedAlignedBytes"] + stats["cpuUnownedUsedBytes"] + stats["tail"])
                self.assertLessEqual(stats["resident"], 4 * PAGE)
            for allocation in owned:
                pool.release(allocation)
            pool.collect()
            self.assertEqual(pool.stats()["resident"], 0)


if __name__ == "__main__":
    unittest.main()
