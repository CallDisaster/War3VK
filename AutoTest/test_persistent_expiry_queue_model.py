import heapq
import random
import unittest


class FullScanExpiryModel:
    def __init__(self, max_age):
        self.max_age = max_age
        self.entries = {}

    def create(self, geometry_id, frame, size):
        self.entries[geometry_id] = [frame, size]

    def hit(self, geometry_id, frame):
        self.entries[geometry_id][0] = frame

    def erase(self, geometry_id):
        self.entries.pop(geometry_id, None)

    def gc(self, frame):
        expired = {
            geometry_id
            for geometry_id, (last_seen, _) in self.entries.items()
            if frame > last_seen and frame - last_seen > self.max_age
        }
        for geometry_id in expired:
            del self.entries[geometry_id]
        return expired


class LazyExpiryModel:
    def __init__(self, max_age):
        self.max_age = max_age
        self.entries = {}
        self.queue = []

    def create(self, geometry_id, frame, size):
        self.entries[geometry_id] = [frame, size]
        heapq.heappush(self.queue, (frame, geometry_id))

    def hit(self, geometry_id, frame):
        self.entries[geometry_id][0] = frame

    def erase(self, geometry_id):
        # The C++ emergency budget repair intentionally leaves a stale token.
        self.entries.pop(geometry_id, None)

    def gc(self, frame):
        expired = set()
        stats = {"popped": 0, "requeued": 0, "stale": 0, "age": 0}
        while self.queue:
            token_frame, geometry_id = self.queue[0]
            if (
                frame <= token_frame
                or frame - token_frame <= self.max_age
            ):
                break
            heapq.heappop(self.queue)
            stats["popped"] += 1
            entry = self.entries.get(geometry_id)
            if entry is None:
                stats["stale"] += 1
                continue
            if entry[0] != token_frame:
                heapq.heappush(self.queue, (entry[0], geometry_id))
                stats["requeued"] += 1
                continue
            del self.entries[geometry_id]
            expired.add(geometry_id)
            stats["age"] += 1
        return expired, stats


class PersistentExpiryQueueModelTests(unittest.TestCase):
    def assert_models_equal(self, reference, lazy):
        self.assertEqual(reference.entries, lazy.entries)
        self.assertGreaterEqual(len(lazy.queue), len(lazy.entries))
        self.assertEqual(
            sum(entry[1] for entry in reference.entries.values()),
            sum(entry[1] for entry in lazy.entries.values()),
        )

    def test_randomized_equivalence_with_hits_and_budget_erases(self):
        rng = random.Random(0x57415233)
        reference = FullScanExpiryModel(max_age=240)
        lazy = LazyExpiryModel(max_age=240)
        next_id = 1

        for frame in range(1, 25001):
            expected = reference.gc(frame)
            actual, stats = lazy.gc(frame)
            self.assertEqual(expected, actual)
            self.assertEqual(
                stats["popped"],
                stats["requeued"] + stats["stale"] + stats["age"],
            )
            self.assert_models_equal(reference, lazy)

            live_ids = list(reference.entries)
            rng.shuffle(live_ids)
            for geometry_id in live_ids[: rng.randrange(0, 9)]:
                reference.hit(geometry_id, frame)
                lazy.hit(geometry_id, frame)

            for _ in range(rng.randrange(0, 4)):
                size = rng.randrange(64, 1 << 18)
                reference.create(next_id, frame, size)
                lazy.create(next_id, frame, size)
                next_id += 1

            if frame % 17 == 0 and reference.entries:
                victims = sorted(
                    reference.entries,
                    key=lambda key: reference.entries[key][0],
                )[: rng.randrange(0, 3)]
                for geometry_id in victims:
                    reference.erase(geometry_id)
                    lazy.erase(geometry_id)

            while len(reference.entries) > 400:
                victim = min(
                    reference.entries,
                    key=lambda key: reference.entries[key][0],
                )
                reference.erase(victim)
                lazy.erase(victim)
            self.assert_models_equal(reference, lazy)

    def test_active_cohort_requeues_then_expires_on_exact_reference_frame(self):
        reference = FullScanExpiryModel(max_age=240)
        lazy = LazyExpiryModel(max_age=240)
        for geometry_id in range(1, 1001):
            reference.create(geometry_id, 0, 256)
            lazy.create(geometry_id, 0, 256)

        burst = None
        for frame in range(1, 242):
            expected = reference.gc(frame)
            actual, stats = lazy.gc(frame)
            self.assertEqual(expected, actual)
            if frame == 241:
                burst = stats
            for geometry_id in tuple(reference.entries):
                reference.hit(geometry_id, frame)
                lazy.hit(geometry_id, frame)

        self.assertEqual(
            burst,
            {"popped": 1000, "requeued": 1000, "stale": 0, "age": 0},
        )
        for frame in range(242, 483):
            expected = reference.gc(frame)
            actual, stats = lazy.gc(frame)
            self.assertEqual(expected, actual)
            self.assertEqual(
                stats["popped"],
                stats["requeued"] + stats["stale"] + stats["age"],
            )
        self.assertEqual(reference.entries, {})
        self.assertEqual(lazy.entries, {})


if __name__ == "__main__":
    unittest.main()
