"""CreateNew-only author weather preview; reuse the existing strict ZIP/PE audit.

Never deploy, overwrite a package, rebuild, change git, or claim stable/release.
Raw game evidence stays in the repository artifacts, not in the author bundle.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

from release_v122_package_audit import BUILD_CONFIG_EXPECTED, REQUIRED_FLAGS, audit_package

ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = ROOT / "AutoTest/artifacts/weather-night-20261010"
DLL = ROOT / "build32_weather_20261010/src/d3d9/d3d9.dll"
EXPECTED_DLL = {"size": 34_467_840,
                "sha256": "8A156518BD029D106EC009DC3C702D0005746E2E97E4589C1C18F8F4684090CE"}
AUTHOR_FILES = (
    "WarVK/WEATHER_API.md", "WarVK/action.txt", "WarVK/call.txt", "WarVK/define.txt",
    "WarVK/jass/warvk_api.j", "WarVK/jass/warvk_constant.j", "WarVK/jass/warvk_weather_demo.j",
)
LICENSE_FILES = (
    "COPYING", "LICENSE", "src/minhook/LICENSE.txt", "subprojects/imgui/LICENSE.txt",
    "subprojects/dxbc-spirv/LICENSE", "subprojects/libdisplay-info/LICENSE", "smaa/LICENSE.txt",
    "include/vulkan/LICENSE.md", "include/spirv/LICENSE", "subprojects/StormBreaker/LICENSE.txt",
)


def identity(path: Path) -> dict:
    return {"size": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest().upper()}


def create_text(path: Path, text: str) -> None:
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        stream.write(text)


def copy_new(source: Path, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    with source.open("rb") as reader, target.open("xb") as writer:
        shutil.copyfileobj(reader, writer)


def checked_evidence() -> dict:
    result = {}
    for name in ("canary-viewport-after", "stress-b"):
        path = ARTIFACTS / name / "run.json"
        value = json.loads(path.read_text(encoding="utf-8"))
        if (value.get("candidateDll", {}).get("sha256") != EXPECTED_DLL["sha256"]
                or value.get("runtimeAcceptance") is not True
                or value.get("protectedInputsUnchanged") is not True
                or value.get("globalInputUsed") is not False
                or value.get("videoRegistryChanged") is not False
                or value.get("stop", {}).get("ok") is not True):
            raise RuntimeError("actual game gate missing or different: " + name)
        result[name] = {"receipt": str(path), **identity(path),
                        "scope": "isolated supported runtime assertions, not full GPU/visual acceptance"}
    static_path = ARTIFACTS / "all-static-final-03/receipt.json"
    static = json.loads(static_path.read_text(encoding="utf-8"))
    if not (static.get("completed") is True and static.get("discoveredScripts") == 282
            and static.get("passedScripts") == 282 and static.get("failedScripts") == 0
            and static.get("rejectedScripts") == 0):
        raise RuntimeError("final full static gate is not closed")
    result["static"] = {"receipt": str(static_path), **identity(static_path)}
    meson_path = ARTIFACTS / "meson-final-04-testlog.json"
    tests = [json.loads(line) for line in meson_path.read_text(encoding="utf-8").splitlines() if line.strip()]
    if len(tests) != 114 or any(test.get("result") != "OK" or test.get("returncode") != 0 for test in tests):
        raise RuntimeError("final full Meson gate is not closed")
    result["meson"] = {"receipt": str(meson_path), **identity(meson_path), "tests": 114}
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--destination", type=Path, required=True)
    args = parser.parse_args()
    destination = args.destination.resolve()
    archive = destination.parent / (destination.name + ".zip")
    if destination.exists() or archive.exists():
        raise FileExistsError("package destination already exists; preserve it")
    if identity(DLL) != EXPECTED_DLL:
        raise RuntimeError("final DLL identity changed")
    evidence = checked_evidence()
    report = ROOT / "docs/research/2026-10-10-weather-morning-report.md"
    sources = {relative: identity(ROOT / relative) for relative in AUTHOR_FILES}
    options_path = ROOT / "build32_weather_20261010/meson-info/intro-buildoptions.json"
    options = {entry["name"]: entry["value"] for entry in json.loads(options_path.read_text(encoding="utf-8"))}
    for name in ("warvk_internal_frame_recorder", "warvk_shadow_observers_dev"):
        if options.get(name) is not False:
            raise RuntimeError("product development gate differs: " + name)
    source_head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    source_status = subprocess.check_output(["git", "status", "--porcelain=v1"], cwd=ROOT, text=True)
    destination.mkdir(parents=True, exist_ok=False)
    copy_new(DLL, destination / "d3d9.dll")
    for relative in AUTHOR_FILES:
        copy_new(ROOT / relative, destination / relative)
    copy_new(report, destination / "docs/WEATHER_MORNING_REPORT.md")
    copy_new(report, destination / "README.md")
    create_text(destination / "VERSION", "1.22.02-weather-preview-20261010\n")
    create_text(destination / "COPYING.txt", "\n\n".join(
        "===== " + relative + " =====\n" + (ROOT / relative).read_text(encoding="utf-8")
        for relative in LICENSE_FILES))
    create_text(destination / "pack.json", json.dumps({
        "candidate": "1.22.02-weather-preview-20261010", "sourceHead": source_head,
        "sourceIsDirty": bool(source_status), "releaseCleanSourceMirror": False,
        "sourceStatusAtPackaging": source_status.splitlines(),
        "authorInputs": sources, "crossFile": identity(ROOT / "build-win32-weather-llvm.txt"),
        "buildType": options.get("buildtype"), "assertions": options.get("b_ndebug"),
        "compiler": "LLVM 22.1.8 Win32 UCRT, separate fresh weather build",
        "evidence": evidence, "published": False, "releaseAuthorized": False,
        "uncovered": ["foreground FPS", "full player/artistic acceptance", "roof snow accumulation",
                      "GPU OOM/device-lost injection", "weather across physical Reset/resize/map transition",
                      "all driver and in-flight retired VRAM peak", "audio-device clock synchronization"],
    }, ensure_ascii=False, indent=2))
    files = {path.relative_to(destination).as_posix(): identity(path)
             for path in sorted(destination.rglob("*")) if path.is_file()}
    create_text(destination / "manifest.json", json.dumps({
        "schema": 1, "kind": "warvk-offline-candidate", "scope": "author",
        "target": {"path": "d3d9.dll", **EXPECTED_DLL}, "files": files,
        "flags": dict.fromkeys(REQUIRED_FLAGS, False), "buildConfig": BUILD_CONFIG_EXPECTED,
        "limits": ["Development preview, not a stable/public release.",
                   "Manual backup/install/rollback only; no player directory was modified.",
                   "Author bundle is not a reproducible release-clean source mirror."],
    }, ensure_ascii=False, indent=2))
    first = audit_package(destination, expect_sha256=EXPECTED_DLL["sha256"], expect_size=EXPECTED_DLL["size"])
    if not first["ok"]:
        raise RuntimeError("directory package audit failed: " + repr(first["errors"]))
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as packed:
        for path in sorted(destination.rglob("*")):
            if path.is_file():
                packed.write(path, path.relative_to(destination).as_posix())
    final = audit_package(destination, archive_path=archive,
                          expect_sha256=EXPECTED_DLL["sha256"], expect_size=EXPECTED_DLL["size"])
    if not final["ok"] or identity(DLL) != EXPECTED_DLL:
        raise RuntimeError("ZIP package audit failed or DLL drift: " + repr(final["errors"]))
    for relative, before in sources.items():
        if identity(ROOT / relative) != before:
            raise RuntimeError("author input changed during package: " + relative)
    print(json.dumps({"ok": True, "dll": {"path": str(destination / "d3d9.dll"), **EXPECTED_DLL},
                      "archive": {"path": str(archive), **identity(archive)},
                      "fileCount": final["fileCount"], "published": False}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
