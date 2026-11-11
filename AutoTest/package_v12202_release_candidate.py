"""Package the exact v1.22.02 RC1 DLL for player testing, never for release.

The package is CreateNew-only.  It keeps every acceptance flag false and uses
the existing strict offline-package auditor, including ZIP CRC/member hashes.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import zipfile

from release_v122_package_audit import (
    BUILD_CONFIG_EXPECTED,
    REQUIRED_FLAGS,
    audit_package,
)


ROOT = Path(__file__).resolve().parents[1]
BUILD_ROOT = Path("D:/WarVK-Builds/v1.22.02-rc1-20260927")
SOURCE = BUILD_ROOT / "source"
CANDIDATE = BUILD_ROOT / "build32-product/src/d3d9/d3d9.dll"
DESTINATION = Path("C:/Users/Administrator/Desktop/WarVK-v1.22.02-RC1-20260928")
EXPECTED_CANDIDATE = {
    "size": 36_149_331,
    "sha256": "07D6AE38F4969580F15C0E2C6B5966A955EA303ABF6201200C5BED8A92EBFC23",
}
EXPECTED_SOURCE_MANIFEST = "4C7707EB66D3FBA394DC414742A1983E25B8E44F715473EB73A7B94C1E0563BA"
LICENSE_PATHS = (
    "COPYING", "LICENSE", "src/minhook/LICENSE.txt",
    "subprojects/imgui/LICENSE.txt", "subprojects/dxbc-spirv/LICENSE",
    "subprojects/libdisplay-info/LICENSE", "smaa/LICENSE.txt",
    "include/vulkan/LICENSE.md", "include/spirv/LICENSE",
    "subprojects/StormBreaker/LICENSE.txt",
)


def identity(path: Path) -> dict[str, int | str]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
            size += len(block)
    return {"size": size, "sha256": digest.hexdigest().upper()}


def create_text(path: Path, value: str) -> None:
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        stream.write(value)


def copy_new(source: Path, target: Path) -> None:
    with source.open("rb") as reader, target.open("xb") as writer:
        shutil.copyfileobj(reader, writer, length=1024 * 1024)


def main() -> int:
    archive = DESTINATION.parent / (DESTINATION.name + ".zip")
    if DESTINATION.exists() or archive.exists():
        raise FileExistsError("candidate destination or archive already exists")
    if identity(CANDIDATE) != EXPECTED_CANDIDATE:
        raise RuntimeError("product DLL identity changed")
    source_manifest = BUILD_ROOT / "source-manifest.json"
    if identity(source_manifest)["sha256"] != EXPECTED_SOURCE_MANIFEST:
        raise RuntimeError("frozen source manifest changed")
    source_data = json.loads(source_manifest.read_text(encoding="utf-8"))
    for relative, expected in source_data["files"].items():
        if identity(SOURCE / relative) != expected:
            raise RuntimeError("frozen source file changed: " + relative)
    if (SOURCE / "RELEASE").read_text(encoding="utf-8").strip() != "1.22.02":
        raise RuntimeError("frozen product version differs")
    notes = (ROOT / "docs/RELEASE_1.22.02.md").read_text(encoding="utf-8")
    if "releaseAuthorized=false" not in notes or "尚未发布" not in notes:
        raise RuntimeError("candidate README omits release hold")

    DESTINATION.mkdir(parents=True, exist_ok=False)
    copy_new(CANDIDATE, DESTINATION / "d3d9.dll")
    create_text(DESTINATION / "README.md", notes)
    copy_new(ROOT / "CHANGELOG.md", DESTINATION / "CHANGELOG.md")
    create_text(DESTINATION / "VERSION", "1.22.02-RC1\n")
    notices = "\n\n".join(
        f"===== {relative} =====\n"
        + (SOURCE / relative).read_text(encoding="utf-8")
        for relative in LICENSE_PATHS
    )
    create_text(DESTINATION / "COPYING.txt", notices)
    create_text(
        DESTINATION / "pack.json",
        json.dumps(
            {
                "candidate": "1.22.02-RC1",
                "sourceHead": source_data["sourceHead"],
                "sourceManifestSha256": EXPECTED_SOURCE_MANIFEST,
                "unstrippedDll": EXPECTED_CANDIDATE,
                "published": False,
                "releaseAuthorized": False,
                "runtimeScope": [
                    "isolated ChangeLevel A-B-A at 2560x1440",
                    "Hamg Walk JASS probe",
                    "Life-and-Death 180.803s route at 1902x963; six single-sample incomplete shadows",
                ],
                "uncovered": [
                    "menu return and re-entry", "D3D9 Reset",
                    "2560x1440 high-pressure recovery", "full player visual acceptance",
                    "other-unit/death/handle-reuse Walk behaviour",
                ],
            },
            ensure_ascii=False,
            indent=2,
        ),
    )
    files = {
        path.name: identity(path)
        for path in sorted(DESTINATION.iterdir()) if path.is_file()
    }
    manifest = {
        "schema": 1,
        "kind": "warvk-offline-candidate",
        "scope": "player",
        "target": {"path": "d3d9.dll", **EXPECTED_CANDIDATE},
        "files": files,
        "flags": dict.fromkeys(REQUIRED_FLAGS, False),
        "buildConfig": BUILD_CONFIG_EXPECTED,
        "limits": [
            "Player RC only; not a stable or public release.",
            "No automatic deployment; keep and restore the original game DLL.",
            "High-pressure and full visual release gates remain open.",
        ],
    }
    create_text(DESTINATION / "manifest.json", json.dumps(manifest, ensure_ascii=False, indent=2))
    result = audit_package(
        DESTINATION, expect_sha256=EXPECTED_CANDIDATE["sha256"],
        expect_size=EXPECTED_CANDIDATE["size"],
    )
    if not result["ok"]:
        raise RuntimeError("candidate audit failed: " + repr(result["errors"]))
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED,
                         compresslevel=6) as packed:
        for path in sorted(DESTINATION.iterdir()):
            packed.write(path, path.name)
    result = audit_package(
        DESTINATION, archive_path=archive,
        expect_sha256=EXPECTED_CANDIDATE["sha256"],
        expect_size=EXPECTED_CANDIDATE["size"],
    )
    if not result["ok"] or identity(CANDIDATE) != EXPECTED_CANDIDATE:
        raise RuntimeError("archive audit or source recheck failed")
    print(json.dumps({
        "ok": True, "dll": identity(DESTINATION / "d3d9.dll"),
        "archive": {"path": str(archive), **identity(archive)},
        "manifestSha256": identity(DESTINATION / "manifest.json")["sha256"],
        "released": False,
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
