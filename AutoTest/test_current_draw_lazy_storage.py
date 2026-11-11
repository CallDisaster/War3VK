"""CPU integration test for CurrentDraw's actual lazy-storage functions.

The fixture extracts complete production functions, structs, constants and TLS
owners; it includes the real LazyThreadArray and FNV implementation. Only the
unrelated enum definitions and configuration/native frame-tag providers are
stubbed. This is NOT a full game ABI, GPU, reset/retirement or pixel test.

Run with an explicitly leased compiler, for example:
  python AutoTest/test_current_draw_lazy_storage.py --compiler <clang++>
    --output AutoTest/artifacts/current-draw-lazy-storage
Use --emit-only to check extraction and emit the fixture without compiling.
Compilation and execution are bounded and BelowNormal on Windows. No product
build, game, process inspection or deployment is performed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
CPP = "src/d3d9/war3/render/war3_current_draw_contract.cpp"
HEADER = "src/d3d9/war3/render/war3_current_draw_contract.h"
LAZY = "src/d3d9/war3/memory/war3_lazy_thread_array.h"

FUNCTIONS = (
    "uint64_t CurrentDrawGeometryIdentityAlias(",
    "size_t CurrentDrawGeometryLedgerSetBase(",
    "bool CurrentDrawGeometryRecordMatchesIdentity(",
    "void StoreCurrentDrawGeometryLedgerAlias(",
    "void StoreCurrentDrawGeometryLedgerRecord(",
    "size_t CurrentDrawCacheSetIndex(",
    "size_t CurrentDrawContractSetBase(",
    "CurrentDrawContractRecord* FindLocalCurrentDrawContract(",
    "size_t SelectLocalCurrentDrawContractSlot(",
    "size_t PaletteSnapshotSetBase(",
    "PaletteSnapshotEntry* FindLocalPaletteSnapshot(",
    "size_t SelectLocalPaletteSnapshotSlot(",
    "ContractLookupStatus LookupCurrentDrawContractRecord(",
    "bool QueryCurrentDrawGeometryContract(",
)
CONSTANTS = (
    "kMaxPaletteMatrices", "kContractCacheSize", "kContractCacheWays",
    "kContractCacheSetCount", "kPaletteSnapshotCacheSize",
    "kPaletteSnapshotCacheWays", "kPaletteSnapshotCacheSetCount",
    "kGeometryContractLedgerSize", "kGeometryContractLedgerWays",
    "kGeometryContractLedgerSetCount",
)
OWNERS = (
    "g_currentDrawContractCache", "g_paletteSnapshotCache",
    "g_currentDrawGeometryLedger",
)


def mask_noncode(text: str) -> str:
    """Keep offsets/newlines while hiding ordinary C++ comments and literals.

    The selected definitions contain no raw literals. Reject those explicitly
    instead of silently accepting a brace parser that would misread them.
    """
    token = r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    return re.sub(token, lambda m: re.sub(r"[^\n]", " ", m[0]), text,
                  flags=re.DOTALL)


def unique_match(pattern: str, text: str) -> re.Match:
    matches = list(re.finditer(pattern, text, flags=re.MULTILINE))
    if len(matches) != 1:
        raise ValueError(f"expected one definition for {pattern!r}, got {len(matches)}")
    return matches[0]


def block(text: str, prefix: str, *, semicolon: bool = False) -> str:
    masked = mask_noncode(text)
    found = unique_match(r"^" + re.escape(prefix), masked)
    start = found.start()
    opening = masked.index("{", found.end())
    depth = 0
    for pos in range(opening, len(masked)):
        depth += (masked[pos] == "{") - (masked[pos] == "}")
        if depth == 0:
            end = pos + 1
            if semicolon:
                if masked[end:end + 1] != ";":
                    raise ValueError(f"missing semicolon after {prefix}")
                end += 1
            result = text[start:end]
            if re.search(r'\bR"', result):
                raise ValueError(f"raw literal unsupported in {prefix}")
            return result
    raise ValueError(f"unbalanced definition: {prefix}")


def statement(text: str, pattern: str) -> str:
    match = unique_match(pattern, mask_noncode(text))
    return text[match.start():match.end()]


PRELUDE = r'''
#include "src/d3d9/war3/memory/war3_lazy_thread_array.h"
#include "src/util/util_bit.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_map>

// Count C++ allocations in narrowly marked single-thread test windows. This
// does not claim to count CRT/Windows malloc or the game's complete memory use.
static unsigned long long allocations = 0;
void* operator new(std::size_t bytes) {
  ++allocations;
  if (void* result = std::malloc(bytes ? bytes : 1)) return result;
  throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { ::operator delete(value); }
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete[](void* value, std::size_t) noexcept { ::operator delete(value); }
static unsigned checks = 0, failures = 0;
static void Check(bool passed, const char* expression, unsigned line) {
  ++checks;
  if (!passed) {
    ++failures;
    std::fprintf(stderr, "FAIL line=%u %s\n", line, expression);
  }
}
#define CHECK(expression) Check(bool(expression), #expression, __LINE__)
#define WARVK_DATA_SCOPE(ignored) ((void)0)
namespace dxvk::war3::render {
// Test-only placeholders for fields untouched by the extracted algorithms.
// Their real production definitions/ABI are outside this fixture's claim.
enum class ObjectKind { Unknown };
enum class War3BatchTag { Unknown };
enum class ShadowProducerKind { CurrentDrawContract };
'''

PROVIDERS = r'''
static bool globalEnabled = false, frameTagAvailable = false;
static uint32_t testFrameTag = 0;
static unsigned frameTagReads = 0;
bool GlobalCurrentDrawPublishEnabled() { return globalEnabled; }
bool TryReadCurrentPaletteFrameTag(uint32_t& out) {
  ++frameTagReads;
  out = testFrameTag;
  return frameTagAvailable;
}
std::mutex g_publishedCurrentDrawMutex;
std::unordered_map<void*, CurrentDrawContractRecord> g_publishedCurrentDrawByPart;
thread_local CurrentDrawContractRecord g_globalFallbackRecord = {};
'''

TESTS = r'''
static void* Ptr(uintptr_t value) { return reinterpret_cast<void*>(value); }
static CurrentDrawContractRecord Record(void* part, uint64_t serial) {
  CurrentDrawContractRecord result;
  result.renderablePart = part;
  result.meshPayloadPtr = Ptr(0x2100);
  result.jHandle = 0x301;
  result.sceneNode = Ptr(0x3200);
  result.worldObjectEntry = Ptr(0x3300);
  result.unitPtr = Ptr(0x3400);
  result.renderFrameIndex = 7;
  result.frameTag = 17;
  result.captureSerial = serial;
  result.producerStage = 11;
  result.producerFreshThisFrame = true;
  result.payloadWord108 = uint32_t(serial);
  return result;
}
static void ExpectGeometryMiss(const CurrentDrawContractRecord& record,
                               uint32_t frame = 7) {
  auto out = Record(Ptr(0x9990), 999);
  CHECK(!QueryCurrentDrawGeometryContract(record.renderablePart,
      record.sceneNode, record.worldObjectEntry, record.unitPtr,
      record.jHandle, frame, out));
  CHECK(EqualRecord(out, CurrentDrawContractRecord{}));
}

static void ColdReadsAndRejectedStores() {
  static_assert(kContractCacheSize == 4096 && kContractCacheWays == 1);
  static_assert(kPaletteSnapshotCacheSize == 512 && kPaletteSnapshotCacheWays == 1);
  static_assert(kGeometryContractLedgerSize == 8192 && kGeometryContractLedgerWays == 4);
  const auto before = allocations;
  CHECK(!g_currentDrawContractCache.peek());
  CHECK(!g_paletteSnapshotCache.peek());
  CHECK(!g_currentDrawGeometryLedger.peek());
  auto record = Record(Ptr(0x1000), 1);
  for (void* part : std::array<void*, 2>{nullptr, record.renderablePart}) {
    CHECK(!FindLocalCurrentDrawContract(part));
    CHECK(!FindLocalPaletteSnapshot(part));
    const CurrentDrawContractRecord* out = &record;
    CHECK(LookupCurrentDrawContractRecord(part, out) == ContractLookupStatus::MissingRecord);
    CHECK(out == nullptr);
  }
  for (unsigned mask = 0; mask < 16; ++mask) {
    auto out = record;
    CHECK(!QueryCurrentDrawGeometryContract(record.renderablePart,
        mask & 1 ? record.sceneNode : nullptr,
        mask & 2 ? record.worldObjectEntry : nullptr,
        mask & 4 ? record.unitPtr : nullptr,
        mask & 8 ? record.jHandle : 0, 7, out));
    CHECK(EqualRecord(out, CurrentDrawContractRecord{}));
  }
  for (unsigned fault = 0; fault < 6; ++fault) {
    auto invalid = record;
    if (fault == 0) invalid.renderablePart = nullptr;
    if (fault == 1) invalid.meshPayloadPtr = nullptr;
    if (fault == 2) invalid.producerStage = 12;
    if (fault == 3) invalid.producerFreshThisFrame = false;
    if (fault == 4) invalid.fromGrace = true;
    if (fault == 5) {
      invalid.jHandle = 0;
      invalid.sceneNode = invalid.unitPtr = invalid.worldObjectEntry = nullptr;
    }
    StoreCurrentDrawGeometryLedgerRecord(invalid);
    CHECK(!g_currentDrawGeometryLedger.peek());
  }
  StoreCurrentDrawGeometryLedgerAlias(record, 0);
  CHECK(allocations == before);
  CHECK(!g_currentDrawContractCache.peek() && !g_paletteSnapshotCache.peek());
}

static void GlobalFallbackKeepsLocalCold() {
  auto record = Record(Ptr(0x1000), 98);
  g_publishedCurrentDrawByPart[record.renderablePart] = record; // setup excluded
  globalEnabled = true;
  const auto before = allocations;
  const auto readsBefore = frameTagReads;
  const CurrentDrawContractRecord* out = nullptr;
  CHECK(LookupCurrentDrawContractRecord(record.renderablePart, out) == ContractLookupStatus::Hit);
  CHECK(out == &g_globalFallbackRecord && EqualRecord(*out, record));
  CHECK(frameTagReads == readsBefore); // Historical global path does not reread tag.
  CHECK(!g_currentDrawContractCache.peek());
  CHECK(LookupCurrentDrawContractRecord(Ptr(0x2000), out) == ContractLookupStatus::MissingRecord);
  CHECK(!out && allocations == before);
  globalEnabled = false;
  g_publishedCurrentDrawByPart.clear();
}

static void DirectMappedCaches() {
  auto record = Record(Ptr(0x1000), 100);
  const auto before = allocations;
  const auto slot = SelectLocalCurrentDrawContractSlot(record.renderablePart);
  CHECK(slot == (uintptr_t(record.renderablePart) >> 4) % 4096);
  CHECK(allocations == before + 1);
  CHECK(!g_paletteSnapshotCache.peek() && !g_currentDrawGeometryLedger.peek());
  auto* backing = g_currentDrawContractCache.peek();
  for (const auto& empty : *backing) CHECK(EqualRecord(empty, CurrentDrawContractRecord{}));
  g_currentDrawContractCache[slot] = record;
  CHECK(FindLocalCurrentDrawContract(record.renderablePart) == &(*backing)[slot]);
  const CurrentDrawContractRecord* out = nullptr;
  frameTagAvailable = true;
  testFrameTag = 18;
  CHECK(LookupCurrentDrawContractRecord(record.renderablePart, out) == ContractLookupStatus::FrameTagMismatch);
  CHECK(!out);
  testFrameTag = record.frameTag;
  CHECK(LookupCurrentDrawContractRecord(record.renderablePart, out) == ContractLookupStatus::Hit);
  CHECK(out == &(*backing)[slot]);
  (*backing)[slot].frameTag = 0;
  testFrameTag = 999;
  CHECK(LookupCurrentDrawContractRecord(record.renderablePart, out) == ContractLookupStatus::Hit);
  (*backing)[slot].frameTag = 1;
  frameTagAvailable = false;
  CHECK(LookupCurrentDrawContractRecord(record.renderablePart, out) == ContractLookupStatus::Hit);
  void* collision = Ptr(uintptr_t(record.renderablePart) + (4096u << 4));
  CHECK(SelectLocalCurrentDrawContractSlot(collision) == slot);
  CHECK(!FindLocalCurrentDrawContract(collision));
  CHECK(LookupCurrentDrawContractRecord(collision, out) == ContractLookupStatus::CacheCollision);
  CHECK(!out);
  (*backing)[slot] = Record(collision, 101);
  CHECK(!FindLocalCurrentDrawContract(record.renderablePart));
  CHECK(FindLocalCurrentDrawContract(collision) == &(*backing)[slot]);
  CHECK(allocations == before + 1);

  const auto paletteBefore = allocations;
  const auto paletteSlot = SelectLocalPaletteSnapshotSlot(record.renderablePart);
  CHECK(paletteSlot == (uintptr_t(record.renderablePart) >> 4) % 512);
  CHECK(allocations == paletteBefore + 1);
  CHECK(!g_currentDrawGeometryLedger.peek());
  auto* paletteBacking = g_paletteSnapshotCache.peek();
  for (const auto& empty : *paletteBacking) {
    CHECK(!empty.renderablePart && !empty.captureSerial && !empty.matrixCount);
    CHECK(empty.paletteProvenance == PaletteProvenance::Unknown);
    for (auto value : empty.bytes) CHECK(value == 0);
  }
  auto& palette = (*paletteBacking)[paletteSlot];
  palette.renderablePart = record.renderablePart;
  palette.frameTag = 123;
  palette.captureSerial = 456;
  palette.matrixCount = 2;
  palette.paletteProvenance = PaletteProvenance::TrustedBlendedWriter;
  palette.bytes[0] = 17;
  palette.bytes.back() = 251;
  CHECK(FindLocalPaletteSnapshot(record.renderablePart) == &palette);
  CHECK(palette.frameTag == 123 && palette.captureSerial == 456 && palette.matrixCount == 2);
  CHECK(palette.paletteProvenance == PaletteProvenance::TrustedBlendedWriter);
  CHECK(palette.bytes[0] == 17 && palette.bytes.back() == 251);
  void* paletteCollision = Ptr(uintptr_t(record.renderablePart) + (512u << 4));
  CHECK(SelectLocalPaletteSnapshotSlot(paletteCollision) == paletteSlot);
  palette.renderablePart = paletteCollision;
  CHECK(!FindLocalPaletteSnapshot(record.renderablePart));
  CHECK(FindLocalPaletteSnapshot(paletteCollision) == &palette);
  const auto warm = allocations;
  for (uintptr_t i = 1; i < 16385; ++i) {
    void* part = Ptr(0x1000u + ((i * 1664525u) & 0xfffffu) * 16u);
    const auto expected = (uintptr_t(part) >> 4) % 4096;
    CHECK(SelectLocalCurrentDrawContractSlot(part) == expected);
    (*backing)[expected] = Record(part, i);
    CHECK(EqualRecord(*FindLocalCurrentDrawContract(part), Record(part, i)));
    CHECK(SelectLocalPaletteSnapshotSlot(part) == (uintptr_t(part) >> 4) % 512);
  }
  CHECK(allocations == warm && backing == g_currentDrawContractCache.peek());
  CHECK(paletteBacking == g_paletteSnapshotCache.peek());
}

static void GeometryFourWayOrder() {
  const uint64_t alias = 0x5a55;
  std::array<void*, 6> parts{};
  parts[0] = Ptr(0x1000);
  const size_t base = CurrentDrawGeometryLedgerSetBase(parts[0], alias);
  unsigned count = 1;
  for (uintptr_t value = 0x1010; count < parts.size() && value < 0x1000000; value += 16)
    if (CurrentDrawGeometryLedgerSetBase(Ptr(value), alias) == base) parts[count++] = Ptr(value);
  CHECK(count == parts.size());
  if (count != parts.size()) return;
  const auto before = allocations;
  const uint64_t serials[] = {30, 10, 10, 40};
  for (unsigned way = 0; way < 4; ++way) {
    StoreCurrentDrawGeometryLedgerAlias(Record(parts[way], serials[way]), alias);
    CHECK(g_currentDrawGeometryLedger[base + way].renderablePart == parts[way]);
    CHECK(g_currentDrawGeometryLedger[base + way].record.known);
  }
  CHECK(allocations == before + 1);
  auto* backing = g_currentDrawGeometryLedger.peek();
  StoreCurrentDrawGeometryLedgerAlias(Record(parts[4], 50), alias);
  CHECK((*backing)[base + 1].renderablePart == parts[4]); // oldest tie: first way
  CHECK((*backing)[base + 2].renderablePart == parts[2]);
  (*backing)[base] = {}; // matching key wins even after an earlier empty slot
  StoreCurrentDrawGeometryLedgerAlias(Record(parts[2], 60), alias);
  CHECK(!(*backing)[base].renderablePart);
  CHECK((*backing)[base + 2].record.captureSerial == 60);
  StoreCurrentDrawGeometryLedgerAlias(Record(parts[5], 70), alias);
  CHECK((*backing)[base].renderablePart == parts[5]);
  CHECK(allocations == before + 1 && backing == g_currentDrawGeometryLedger.peek());

  // Independent reference selection, using the production hash only for the
  // set index. It does not copy the production selection loop.
  backing->fill({});
  auto reference = std::make_unique<std::array<CurrentDrawGeometryLedgerEntry, 8192>>();
  const auto warm = allocations;
  for (uint64_t n = 1; n <= 12000; ++n) {
    auto record = Record(parts[n % parts.size()], (n * 29) % 101);
    const auto first = reference->begin() + base;
    const auto last = first + 4;
    auto selected = std::find_if(first, last, [&](const auto& e) {
      return e.renderablePart == record.renderablePart && e.identityAlias == alias;
    });
    if (selected == last) selected = std::find_if(first, last, [](const auto& e) { return !e.renderablePart; });
    if (selected == last) selected = std::min_element(first, last, [](const auto& a, const auto& b) {
      return a.record.captureSerial < b.record.captureSerial;
    });
    *selected = {record.renderablePart, alias, record};
    selected->record.known = true;
    StoreCurrentDrawGeometryLedgerAlias(record, alias);
    for (unsigned way = 0; way < 4; ++way) {
      const auto& actual = (*backing)[base + way];
      const auto& expected = (*reference)[base + way];
      CHECK(actual.renderablePart == expected.renderablePart);
      CHECK(actual.identityAlias == expected.identityAlias);
      CHECK(EqualRecord(actual.record, expected.record));
    }
  }
  CHECK(allocations == warm);
}

static void GeometryQueryIdentityAndFreshness() {
  auto* backing = g_currentDrawGeometryLedger.peek();
  backing->fill({});
  const auto warm = allocations;
  auto record = Record(Ptr(0x7000), 100);
  StoreCurrentDrawGeometryLedgerRecord(record);
  auto expected = record;
  expected.known = true;
  for (unsigned mask = 1; mask < 16; ++mask) {
    CurrentDrawContractRecord out{};
    CHECK(QueryCurrentDrawGeometryContract(record.renderablePart,
        mask & 1 ? record.sceneNode : nullptr,
        mask & 2 ? record.worldObjectEntry : nullptr,
        mask & 4 ? record.unitPtr : nullptr,
        mask & 8 ? record.jHandle : 0, 7, out));
    CHECK(EqualRecord(out, expected));
  }
  for (unsigned fault = 0; fault < 7; ++fault) {
    auto query = record;
    if (fault == 0) query.sceneNode = Ptr(0xdead);
    if (fault == 1) query.worldObjectEntry = Ptr(0xdead);
    if (fault == 2) query.unitPtr = Ptr(0xdead);
    if (fault == 3) query.jHandle ^= 0x100;
    if (fault == 4) query.renderablePart = nullptr;
    if (fault == 5) query.renderablePart = Ptr(0xbeef);
    if (fault == 6) { query.sceneNode = query.worldObjectEntry = query.unitPtr = nullptr; query.jHandle = 0; }
    ExpectGeometryMiss(query);
  }
  ExpectGeometryMiss(record, 8);
  const auto alias = CurrentDrawGeometryIdentityAlias(1, record.jHandle);
  for (unsigned fault = 0; fault < 6; ++fault) {
    backing->fill({});
    auto invalid = record;
    if (fault == 0) invalid.producerStage = 12;
    if (fault == 1) invalid.producerFreshThisFrame = false;
    if (fault == 2) invalid.fromGrace = true;
    if (fault == 3) invalid.meshPayloadPtr = nullptr;
    if (fault == 4) invalid.renderFrameIndex = 8;
    StoreCurrentDrawGeometryLedgerAlias(invalid, alias); // bypass writer precheck intentionally
    if (fault == 5) (*backing)[CurrentDrawGeometryLedgerSetBase(record.renderablePart, alias)].record.known = false;
    ExpectGeometryMiss(record);
  }
  backing->fill({});
  auto newer = record;
  newer.captureSerial = 200;
  newer.payloadWordF0 = 200;
  const auto unitAlias = CurrentDrawGeometryIdentityAlias(2, uintptr_t(record.unitPtr));
  StoreCurrentDrawGeometryLedgerAlias(record, alias);
  StoreCurrentDrawGeometryLedgerAlias(newer, unitAlias);
  CurrentDrawContractRecord out{};
  CHECK(QueryCurrentDrawGeometryContract(record.renderablePart, record.sceneNode,
      record.worldObjectEntry, record.unitPtr, record.jHandle, 7, out));
  newer.known = true;
  CHECK(EqualRecord(out, newer));
  newer.captureSerial = record.captureSerial;
  StoreCurrentDrawGeometryLedgerAlias(newer, unitAlias);
  CHECK(QueryCurrentDrawGeometryContract(record.renderablePart, record.sceneNode,
      record.worldObjectEntry, record.unitPtr, record.jHandle, 7, out));
  CHECK(EqualRecord(out, expected)); // equal serial keeps jHandle's earlier alias
  backing->fill({});
  record.renderFrameIndex = 0;
  StoreCurrentDrawGeometryLedgerRecord(record);
  CHECK(QueryCurrentDrawGeometryContract(record.renderablePart, nullptr, nullptr,
      nullptr, record.jHandle, 0, out)); // frame zero is valid, not a sentinel
  CHECK(out.renderFrameIndex == 0 && out.known);
  CHECK(allocations == warm && backing == g_currentDrawGeometryLedger.peek());
}
} // namespace dxvk::war3::render
int main() {
  using namespace dxvk::war3::render;
  ColdReadsAndRejectedStores();
  GlobalFallbackKeepsLocalCold();
  DirectMappedCaches();
  GeometryFourWayOrder();
  GeometryQueryIdentityAndFreshness();
  std::printf("CURRENT_DRAW_LAZY_CPU checks=%u failures=%u\n", checks, failures);
  return failures ? 1 : 0;
}
'''


def fixture() -> tuple[str, dict]:
    cpp = (ROOT / CPP).read_text(encoding="utf-8")
    header = (ROOT / HEADER).read_text(encoding="utf-8")
    extracted: dict[str, str] = {}
    for name in CONSTANTS:
        extracted[name] = statement(cpp, rf"^static constexpr (?:size_t|uint32_t)\s+{name}\s*=[^;]+;")
    for name, text, prefix in (
        ("PaletteProvenance", header, "enum class PaletteProvenance : uint32_t"),
        ("CurrentDrawContractRecord", header, "struct CurrentDrawContractRecord"),
        ("ContractLookupStatus", cpp, "enum class ContractLookupStatus : uint32_t"),
        ("PaletteSnapshotEntry", cpp, "struct PaletteSnapshotEntry"),
        ("CurrentDrawGeometryLedgerEntry", cpp, "struct CurrentDrawGeometryLedgerEntry"),
    ):
        extracted[name] = block(text, prefix, semicolon=True)
    for name in OWNERS:
        extracted[name] = statement(cpp, rf"^thread_local\s+memory::ThreadLocalLazyArray<[^;]+?\s+{name}\s*=\s*\{{\}};")
    for prefix in FUNCTIONS:
        extracted[prefix] = block(cpp, prefix)
    record = mask_noncode(extracted["CurrentDrawContractRecord"])
    fields = re.findall(r"^\s+[\w:]+\*?\s+(\w+)\s*=", record, re.MULTILINE)
    if len(fields) < 35 or len(fields) != len(set(fields)):
        raise ValueError("unexpected record fields: full-field comparator cannot be certified")
    equal = "static bool EqualRecord(const CurrentDrawContractRecord& a, const CurrentDrawContractRecord& b) {\n  return "
    equal += " &&\n      ".join(f"a.{name} == b.{name}" for name in fields) + ";\n}\n"
    # Static maintenance checks are separately labelled; the executable does
    # not pretend to execute the much larger Reset/Retire/Snapshot functions.
    maintenance = {}
    for prefix in ("void ResetCurrentDrawContractCache(",
                   "CurrentDrawRetireResult RetireCurrentDrawContracts("):
        text = mask_noncode(block(cpp, prefix))
        for name in OWNERS:
            if not re.search(rf"{name}\.peek\(\)", text):
                raise ValueError(f"maintenance would miss cold peek: {prefix} {name}")
            if re.search(rf"{name}\s*\[|:\s*{name}\s*\)|{name}\.(?:get|fill|begin|end)\(", text):
                raise ValueError(f"maintenance could warm cold table: {prefix} {name}")
        maintenance[prefix] = "STATIC_PEEK_ONLY"
    chunks = [PRELUDE]
    chunks += [extracted[name] for name in CONSTANTS]
    chunks += [extracted[name] for name in ("PaletteProvenance", "CurrentDrawContractRecord",
               "ContractLookupStatus", "PaletteSnapshotEntry", "CurrentDrawGeometryLedgerEntry")]
    chunks += [extracted[name] for name in OWNERS]
    chunks += [PROVIDERS]
    chunks += [extracted[prefix] for prefix in FUNCTIONS]
    chunks += [equal, TESTS]
    generated = "\n\n".join(chunks)
    receipt = {
        "scope": "CPU full extracted functions; no game ABI/GPU/visual or full maintenance execution",
        "functionCount": len(FUNCTIONS), "recordFieldCount": len(fields),
        "functions": {key: hashlib.sha256(value.encode()).hexdigest() for key, value in extracted.items()},
        "productionFiles": {path: hashlib.sha256((ROOT / path).read_bytes()).hexdigest()
                            for path in (CPP, HEADER, LAZY, "src/util/util_bit.h")},
        "generatedSha256": hashlib.sha256(generated.encode()).hexdigest(),
        "maintenance": maintenance,
        "stubs": ["ObjectKind/War3BatchTag/ShadowProducerKind default enums",
                  "GlobalCurrentDrawPublishEnabled", "TryReadCurrentPaletteFrameTag", "WARVK_DATA_SCOPE"],
    }
    return generated, receipt


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, help="explicit C++17 compiler under caller's build lease")
    parser.add_argument("--output", type=Path, help="fresh directory for fixture, executable and receipt")
    parser.add_argument("--emit-only", action="store_true", help="no compiler or executable invocation")
    args = parser.parse_args()
    if not args.emit_only and not args.compiler:
        parser.error("--compiler is required unless --emit-only is used")
    output = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="warvk-lazy-currentdraw-"))
    output.mkdir(parents=True, exist_ok=True)
    cpp_path = output / "current_draw_lazy_fixture.cpp"
    receipt_path = output / "receipt.json"
    if cpp_path.exists() or receipt_path.exists():
        parser.error("output already contains a fixture/receipt; choose a fresh directory")
    generated, receipt = fixture()
    cpp_path.write_text(generated, encoding="utf-8")
    receipt["fixture"] = str(cpp_path)
    receipt["status"] = "EMITTED_NOT_COMPILED"
    priority = subprocess.BELOW_NORMAL_PRIORITY_CLASS if os.name == "nt" else 0
    try:
        if not args.emit_only:
            executable = output / ("current_draw_lazy_fixture.exe" if os.name == "nt" else "current_draw_lazy_fixture")
            command = [str(args.compiler.resolve()), "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
                       "-static", "-I", str(ROOT), str(cpp_path), "-o", str(executable)]
            receipt["compileCommand"] = command
            result = subprocess.run(command, capture_output=True, text=True, timeout=120, creationflags=priority)
            (output / "compile.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            receipt["compileExit"] = result.returncode
            if result.returncode != 0:
                receipt["status"] = "COMPILE_FAILED"
                return 1
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30, creationflags=priority)
            (output / "run.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            receipt["runExit"] = result.returncode
            receipt["runOutput"] = result.stdout.strip()
            receipt["executableSha256"] = hashlib.sha256(executable.read_bytes()).hexdigest()
            passed = re.fullmatch(r"CURRENT_DRAW_LAZY_CPU checks=([1-9][0-9]*) failures=0", result.stdout.strip())
            receipt["status"] = "CPU_PASS" if result.returncode == 0 and passed else "CPU_FAILED"
            if receipt["status"] != "CPU_PASS":
                return 1
        return 0
    except (OSError, subprocess.TimeoutExpired) as error:
        receipt["status"] = "RUNNER_FAILED"
        receipt["error"] = str(error)
        return 1
    finally:
        receipt_path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({"status": receipt["status"], "receipt": str(receipt_path),
                          "runOutput": receipt.get("runOutput")}, ensure_ascii=False))


if __name__ == "__main__":
    sys.exit(main())
