#!/usr/bin/env python3
"""Compile/run exact production 401/405 routing bodies with bounded CPU stubs.

This is NOT a second implementation of the router. Selected, the enabled-domain
predicate, both complete Hooked_SMemAlloc/Hooked_SMemReAlloc definitions and their
constants are extracted verbatim from the current production files. Only their
external dependencies are stubbed. This proves dispatch/argument/return behavior,
not TLSF ownership, native Storm allocation, hooks, GPU, gameplay or performance.

The integration owner runs this serially under its build lease. --emit-only
creates the reviewable fixture/manifest without compiling or claiming a pass.
The runner never loads Game/Storm/product DLLs, deploys, or changes configuration.
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

ROOT = Path(__file__).resolve().parents[1]
HOOK = ROOT / "src/d3d9/war3/memory/war3_storm_hook.cpp"
DOMAIN = ROOT / "src/d3d9/war3/memory/war3_storm_low_domain.cpp"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def identity(path: Path) -> dict:
    raw = path.read_bytes()
    return dict(path=str(path.resolve()), bytes=len(raw),
                sha256=hashlib.sha256(raw).hexdigest().upper())


def mask_cpp(source: str) -> str:
    """Mask comments/literals without changing offsets; fail on unsupported raw strings."""
    chars = list(source)
    at = 0
    while at < len(source):
        end = at
        if source.startswith("//", at):
            end = source.find("\n", at)
            if end < 0:
                end = len(source)
        elif source.startswith("/*", at):
            close = source.find("*/", at + 2)
            require(close >= 0, "unterminated C++ block comment")
            end = close + 2
        elif source.startswith('R"', at):
            raise ValueError("raw C++ literal requires explicit extractor review")
        elif source[at] in ('"', "'"):
            quote = source[at]
            end = at + 1
            closed = False
            while end < len(source):
                if source[end] == "\\":
                    end += 2
                elif source[end] == quote:
                    end += 1
                    closed = True
                    break
                else:
                    end += 1
            require(closed, "unterminated C++ literal")
        if end > at:
            for index in range(at, min(end, len(source))):
                if chars[index] not in "\r\n":
                    chars[index] = " "
            at = end
        else:
            at += 1
    return "".join(chars)


def extract(source: str, path: Path, pattern: str, *, function: bool) -> dict:
    masked = mask_cpp(source)
    matches = list(re.finditer(pattern, masked, re.MULTILINE))
    require(len(matches) == 1, f"expected one complete definition: {path.name}: {pattern}")
    start = matches[0].start()
    if function:
        opening = masked.find("{", matches[0].end())
        require(opening >= 0, "missing function body")
        require(";" not in masked[matches[0].end():opening], "matched a declaration, not a body")
        depth = 0
        end = None
        for index in range(opening, len(masked)):
            if masked[index] == "{":
                depth += 1
            elif masked[index] == "}":
                depth -= 1
                if depth == 0:
                    end = index + 1
                    break
        require(end is not None, "unbalanced function body")
    else:
        closing = masked.find(";", matches[0].end())
        require(closing >= 0, "missing declaration terminator")
        end = closing + 1
    exact = source[start:end]
    require(exact and source.count(exact) == 1, "extracted definition is not unique")
    return dict(path=path.relative_to(ROOT).as_posix(), firstLine=source.count("\n", 0, start) + 1,
                bytes=len(exact.encode("utf-8")),
                sha256=hashlib.sha256(exact.encode("utf-8")).hexdigest().upper(), text=exact)


PRELUDE = r'''
// GENERATED fixture: exact production definitions follow below, without substitutions.
// External dependencies are deterministic CPU-only stubs; no native allocator is loaded.
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
static_assert(sizeof(void*) == 4, "must exercise the Win32 ABI");
namespace route_fixture {
struct State {
  bool pool = true, oom = false;
  unsigned nativeCalls = 0, managedCalls = 0, poolQueries = 0;
  unsigned zeroCalls = 0, warnings = 0, unexpected = 0, nativeReallocCalls = 0;
  unsigned managedQueries = 0, rejectedQueries = 0;
  int ecx = 0, edx = 0;
  size_t size = 0, zeroBegin = 0, zeroEnd = 0;
  const char* name = nullptr;
  DWORD line = 0, flags = 0;
  void* old = nullptr;
  void* zeroPointer = nullptr;
  void* nativeResult = nullptr;
  void* managedResult = nullptr;
} state;
unsigned checks = 0, failures = 0, routeCases = 0, nativeReallocCases = 0;
char nativeMarker, managedMarker, oldMarker;
void check(bool ok, const char* expression, int line) {
  ++checks;
  if (!ok) {
    if (failures < 16) std::fprintf(stderr, "FAIL line=%d expr=%s\n", line, expression);
    ++failures;
  }
}
#define CHECK(x) check(bool(x), #x, __LINE__)
namespace hooks {
enum class War3HotHookId { StormAlloc, StormReAlloc };
struct War3HotHookCallTiming { War3HotHookCallTiming(War3HotHookId, unsigned) {} };
struct War3HotHookNativeScope { explicit War3HotHookNativeScope(War3HotHookCallTiming&) {} };
}
namespace war3dbg { void Print(const char*, ...) { ++state.warnings; } }
std::atomic<bool> g_enabled{false}, g_redirectEnabled{true};
std::atomic<size_t> g_largeBlockThreshold{0};
bool TlsfPool_IsInitialized() { ++state.poolQueries; return state.pool; }
void* CallOriginalAlloc(int ecx, int edx, size_t size, const char* name,
                       DWORD line, DWORD flags) {
  ++state.nativeCalls;
  state.ecx = ecx; state.edx = edx; state.size = size;
  state.name = name; state.line = line; state.flags = flags;
  return state.nativeResult;
}
void* AllocManagedBlock(size_t size) {
  ++state.managedCalls; state.size = size;
  return state.oom ? nullptr : state.managedResult;
}
void ZeroRequestedRange(void* pointer, size_t begin, size_t end) {
  ++state.zeroCalls; state.zeroPointer = pointer;
  state.zeroBegin = begin; state.zeroEnd = end;
}
// 405 tests below deliberately model an ORIGINAL-owned block, never a managed one.
// Unreachable managed mutation dependencies report a failure if a regression reaches them.
struct alignas(16) StormTlsfHeader { unsigned char bytes[16]; };
bool QueryManagedBlock(void*, StormTlsfHeader**, size_t*, bool) {
  ++state.managedQueries; return false;
}
bool IsRejectedManagedPointer(void*) { ++state.rejectedQueries; return false; }
void NoteRejectedManagedPointer(void*, const char*) { ++state.unexpected; }
void RestoreManagedHeaderClaim(StormTlsfHeader*) { ++state.unexpected; }
bool ReleaseClaimedManagedBlock(void*, StormTlsfHeader*, size_t) { ++state.unexpected; return false; }
void* TlsfPool_ReallocInPlace(void*, size_t) { ++state.unexpected; return nullptr; }
void SetupManagedHeader(void*, size_t) { ++state.unexpected; }
void NoteManagedResize(size_t, size_t) { ++state.unexpected; }
bool CopyIntoNewBlock(void*, const void*, size_t, size_t) { ++state.unexpected; return false; }
bool DiscardManagedBlock(void*) { ++state.unexpected; return false; }
int CallOriginalFree(void*, const char*, int, DWORD) { ++state.unexpected; return 0; }
void* CallOriginalReAlloc(int ecx, int edx, void* old, size_t size,
                         const char* name, DWORD line, DWORD flags) {
  ++state.nativeReallocCalls; state.old = old;
  state.ecx = ecx; state.edx = edx; state.size = size;
  state.name = name; state.line = line; state.flags = flags;
  return state.nativeResult;
}
'''

HARNESS = r'''
#line 1 "generated-routing-harness"
void reset(bool enabled, bool redirect, bool pool, bool oom, bool nullResult, size_t threshold) {
  state = {};
  state.pool = pool; state.oom = oom;
  state.nativeResult = nullResult ? nullptr : &nativeMarker;
  state.managedResult = &managedMarker;
  g_enabled.store(enabled); g_redirectEnabled.store(redirect);
  g_largeBlockThreshold.store(threshold);
}
struct SourceCase { const char* name; DWORD line; bool exact; };
int run() {
  // This independent expected identity is intentionally not read from kSource.
  constexpr char expected[] = ".?AUMDLDATAHash@@";
  char copied[] = ".?AUMDLDATAHash@@";
  CHECK(sizeof(kSource) == 18);
  CHECK(std::strcmp(kSource, expected) == 0);
  CHECK(kHeap == 0x1E955348u);
  CHECK(kStormNativeLargeBlockThreshold == 0xFE7Cu);
  const SourceCase sources[] = {
    {expected, DWORD(-2), true}, {copied, DWORD(-2), true},
    {expected, 0, false}, {".?AUMDLDATAHash@@extra", DWORD(-2), false},
    {".?AUMDLDATAHASH@@", DWORD(-2), false}, {"other-owner", DWORD(-2), false},
    {nullptr, DWORD(-2), false}
  };
  const size_t sizes[] = {0, 1, 0x5784u, 0xFE7Bu, 0xFE7Cu, 0xFE7Du, 0x10000u, 0x20010u};
  const size_t thresholds[] = {0xFE7Cu, 0x20000u};
  const DWORD flagCases[] = {0, 8, 0x10u, 0x18u, 0xA5A50008u};
  constexpr int ecx = 0x13572468, edx = -12345;
  for (const auto& source : sources) for (size_t size : sizes)
  for (size_t threshold : thresholds) for (bool enabled : {false, true})
  for (bool redirect : {false, true}) for (bool pool : {false, true})
  for (bool oom : {false, true}) for (bool nullResult : {false, true})
  for (DWORD flags : flagCases) for (bool viaNullRealloc : {false, true}) {
    reset(enabled, redirect, pool, oom, nullResult, threshold);
    CHECK(StormLowDomain_RequiresNativeAllocation(source.name, int32_t(source.line)) ==
          (enabled && source.exact));
    const bool forced = enabled && source.exact;
    const bool tryManaged = !forced && redirect && size >= threshold && pool;
    const bool expectNative = !tryManaged || oom;
    void* result = viaNullRealloc
      ? Hooked_SMemReAlloc(ecx, edx, nullptr, size, source.name, source.line, flags)
      : Hooked_SMemAlloc(ecx, edx, size, source.name, source.line, flags);
    ++routeCases;
    CHECK(state.nativeCalls == unsigned(expectNative));
    CHECK(state.managedCalls == unsigned(tryManaged));
    CHECK(state.poolQueries == unsigned(!forced && redirect && size >= threshold));
    CHECK(state.nativeReallocCalls == 0);
    CHECK(state.managedQueries == 0 && state.rejectedQueries == 0);
    CHECK(state.unexpected == 0);
    CHECK(state.warnings == unsigned(tryManaged && oom));
    CHECK(result == (expectNative ? state.nativeResult : state.managedResult));
    CHECK(state.size == size);
    if (expectNative) {
      CHECK(state.ecx == ecx && state.edx == edx);
      CHECK(state.name == source.name && state.line == source.line && state.flags == flags);
      CHECK(state.zeroCalls == 0);
    } else {
      CHECK(state.zeroCalls == unsigned((flags & 8u) != 0));
      if (flags & 8u)
        CHECK(state.zeroPointer == state.managedResult && state.zeroBegin == 0 && state.zeroEnd == size);
    }
  }
  // Native ownership must dominate caller metadata/size. No upgrade to managed
  // storage, including size zero or a different source name on realloc.
  for (const auto& source : sources) for (size_t size : sizes)
  for (bool enabled : {false, true}) for (bool redirect : {false, true})
  for (bool nullResult : {false, true}) for (DWORD flags : flagCases) {
    reset(enabled, redirect, true, false, nullResult, 0xFE7Cu);
    void* result = Hooked_SMemReAlloc(ecx, edx, &oldMarker, size,
                                    source.name, source.line, flags);
    ++nativeReallocCases;
    CHECK(state.nativeReallocCalls == 1 && state.nativeCalls == 0);
    CHECK(state.managedCalls == 0 && state.poolQueries == 0);
    CHECK(state.old == &oldMarker && state.size == size);
    CHECK(state.ecx == ecx && state.edx == edx);
    CHECK(state.name == source.name && state.line == source.line && state.flags == flags);
    CHECK(state.managedQueries == 1 && state.rejectedQueries == 1);
    CHECK(result == state.nativeResult);
    CHECK(state.zeroCalls == 0 && state.warnings == 0 && state.unexpected == 0);
  }
  std::printf("RESULT {\"checks\":%u,\"routeCases\":%u,\"nativeReallocCases\":%u,"
              "\"failures\":%u,\"pointerBits\":32}\n", checks, routeCases, nativeReallocCases, failures);
  return failures ? 1 : 0;
}
} // namespace route_fixture
int main() { return route_fixture::run(); }
'''


def fixture() -> tuple[str, list[dict]]:
    # read_bytes preserves CRLF/LF exactly; no substitution is applied to copied definitions.
    hook = HOOK.read_bytes().decode("utf-8")
    domain = DOMAIN.read_bytes().decode("utf-8")
    specs = [
        (domain, DOMAIN, r"^[ \t]*constexpr[ \t]+char[ \t]+kSource\[\][ \t]*=", False),
        (domain, DOMAIN, r"^[ \t]*constexpr[ \t]+uint32_t[ \t]+kHeap[ \t]*=", False),
        (hook, HOOK, r"^[ \t]*constexpr[ \t]+size_t[ \t]+kStormNativeLargeBlockThreshold[ \t]*=", False),
        (domain, DOMAIN, r"^[ \t]*bool[ \t]+Selected\(", True),
        (domain, DOMAIN, r"^[ \t]*bool[ \t]+StormLowDomain_RequiresNativeAllocation\(", True),
        (hook, HOOK, r"^[ \t]*void[ \t]*\*[ \t]*__fastcall[ \t]+Hooked_SMemAlloc\(", True),
        (hook, HOOK, r"^[ \t]*void[ \t]*\*[ \t]*__fastcall[ \t]+Hooked_SMemReAlloc\(", True),
    ]
    fragments = [extract(text, path, pattern, function=is_function)
                 for text, path, pattern, is_function in specs]
    pieces = [PRELUDE]
    for fragment in fragments:
        pieces += [f'\n#line {fragment["firstLine"]} "{fragment["path"]}"\n', fragment["text"], "\n"]
    pieces.append(HARNESS)
    generated = "".join(pieces)
    for fragment in fragments:
        require(generated.count(fragment["text"]) == 1, "generated source changed/duplicated a definition")
    return generated, [{key: value for key, value in fragment.items() if key != "text"}
                       for fragment in fragments]


def write_new(path: Path, raw: bytes) -> None:
    with path.open("xb") as stream:
        stream.write(raw)


def execute(command: list[str], cwd: Path, environment: dict, timeout: int) -> dict:
    flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS
    result = subprocess.run(command, cwd=cwd, env=environment, timeout=timeout,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            creationflags=flags, check=False)
    return dict(command=command, exitCode=result.returncode,
                stdout=result.stdout.decode("utf-8", "replace"),
                stderr=result.stderr.decode("utf-8", "replace"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="fresh direct AutoTest/artifacts child")
    parser.add_argument("--compiler", type=Path, help="explicit i686-w64-mingw32-clang++.exe")
    parser.add_argument("--emit-only", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    require(output.parent == (ROOT / "AutoTest/artifacts").resolve(), "output must be a direct artifact child")
    output.mkdir(exist_ok=False)
    receipt = dict(schema="warvk-storm-low-route/v1", ok=False, emitted=False, compiled=False,
                   executed=False, cpuStubOnly=True, productionBodiesUnmodified=True,
                   nativeStormLoaded=False, gameLaunched=False, productBuilt=False,
                   deploymentPerformed=False, allocatorOwnershipCertified=False, steps=[])
    try:
        sources = [HOOK, DOMAIN, Path(__file__).resolve()]
        before = [identity(path) for path in sources]
        receipt["inputs"] = before
        generated, fragments = fixture()
        source_path = output / "production-routing-fixture.cpp"
        write_new(source_path, generated.encode("utf-8"))
        receipt.update(emitted=True, extractedDefinitions=fragments, fixture=identity(source_path))
        if args.emit_only:
            receipt["status"] = "EMITTED_NOT_TESTED"
            require(before == [identity(path) for path in sources], "source changed during extraction")
            return 0
        require(os.name == "nt", "Win32 CPU fixture must run on Windows")
        require(args.compiler is not None, "--compiler is required unless --emit-only")
        compiler = args.compiler.resolve()
        require(compiler.is_file() and compiler.name.lower() == "i686-w64-mingw32-clang++.exe",
                "explicit LLVM32 compiler required")
        receipt["compiler"] = identity(compiler)
        binary = output / "production-routing-fixture.exe"
        environment = dict(os.environ)
        environment["PATH"] = str(compiler.parent) + os.pathsep + environment.get("PATH", "")
        command = [str(compiler), "-std=c++17", "-O3", "-Wall", "-Wextra",
                   "-D_WIN32_WINNT=0xA00", "-mstack-alignment=4", str(source_path), "-o", str(binary)]
        compile_step = execute(command, ROOT, environment, 60)
        receipt["steps"].append(compile_step)
        write_new(output / "compile.stdout.txt", compile_step["stdout"].encode("utf-8"))
        write_new(output / "compile.stderr.txt", compile_step["stderr"].encode("utf-8"))
        require(compile_step["exitCode"] == 0, "routing fixture compile failed")
        receipt.update(compiled=True, executable=identity(binary))
        pe = binary.read_bytes()
        require(pe[:2] == b"MZ", "fixture is not PE")
        offset = int.from_bytes(pe[0x3C:0x40], "little")
        require(pe[offset:offset + 4] == b"PE\0\0" and
                int.from_bytes(pe[offset + 4:offset + 6], "little") == 0x14C,
                "fixture is not Win32 i386")
        require(before == [identity(path) for path in sources], "source changed before execution")
        run_step = execute([str(binary)], output, environment, 15)
        receipt["steps"].append(run_step)
        receipt["executed"] = True
        write_new(output / "run.stdout.txt", run_step["stdout"].encode("utf-8"))
        write_new(output / "run.stderr.txt", run_step["stderr"].encode("utf-8"))
        require(run_step["exitCode"] == 0, "routing fixture execution failed")
        match = re.fullmatch(r"RESULT (\{[^\r\n]+\})\s*", run_step["stdout"])
        require(match is not None, "missing/extra fixture output")
        result = json.loads(match.group(1))
        require(set(result) == {"checks", "routeCases", "nativeReallocCases", "failures", "pointerBits"},
                "fixture result shape mismatch")
        require(all(type(value) is int for value in result.values()), "fixture counts must be integers")
        # Exact matrix cardinality prevents empty/reduced loops from silently passing.
        require(result["routeCases"] == 35840 and result["nativeReallocCases"] == 2240 and
                result["checks"] > 450000 and result["failures"] == 0 and result["pointerBits"] == 32,
                "fixture matrix/result mismatch")
        receipt.update(result=result, status="EXACT_PRODUCTION_ROUTE_CPU_STUB_PASS")
        require(before == [identity(path) for path in sources], "source changed during run")
        receipt["ok"] = True
        return 0
    except Exception as error:
        receipt.update(status="FAILED", error=f"{type(error).__name__}: {error}")
        print(receipt["error"], file=sys.stderr)
        return 1
    finally:
        write_new(output / "receipt.json", (json.dumps(receipt, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))
        print(json.dumps({key: receipt.get(key) for key in ("status", "ok", "emitted", "compiled", "executed", "result")},
                         ensure_ascii=False))


if __name__ == "__main__":
    raise SystemExit(main())
