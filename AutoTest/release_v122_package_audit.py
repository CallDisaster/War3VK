"""Strict read-only audit for WarVK v1.22 offline package manifests.

One supported manifest shape is accepted.  The tool never builds, copies,
deploys, launches, writes evidence, or mutates the worktree.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct
import sys
import zipfile


SUPPORTED_SCHEMA = 1
SUPPORTED_KIND = "warvk-offline-candidate"
SUPPORTED_SCOPES = {"player", "author"}
PLAYER_ROOT_FILES = {
    "CHANGELOG.md",
    "CHANGELOG.txt",
    "COPYING",
    "COPYING.txt",
    "LICENSE",
    "LICENSE.txt",
    "README.md",
    "README.txt",
    "VERSION",
    "d3d9.dll",
    "pack.json",
}
AUTHOR_ROOT_FILES = set(PLAYER_ROOT_FILES)
AUTHOR_PREFIXES = (
    "WarVK/",
    "assets/",
    "docs/",
    "subprojects/war3fx/",
    "water/",
)
SUPPORTED_TARGET_PATH = "d3d9.dll"
AUTHOR_ALLOWED_SUFFIXES = {
    ".csv",
    ".gif",
    ".j",
    ".jpeg",
    ".jpg",
    ".json",
    ".md",
    ".png",
    ".svg",
    ".txt",
}
EVIDENCE_PATH_PARTS = {
    "crashdumps",
    "dumps",
    "evidence",
    "log",
    "logs",
    "raw",
    "screenshots",
}
WINDOWS_RESERVED = {
    "AUX",
    "COM1",
    "COM2",
    "COM3",
    "COM4",
    "COM5",
    "COM6",
    "COM7",
    "COM8",
    "COM9",
    "CON",
    "LPT1",
    "LPT2",
    "LPT3",
    "LPT4",
    "LPT5",
    "LPT6",
    "LPT7",
    "LPT8",
    "LPT9",
    "NUL",
    "PRN",
}
REQUIRED_FLAGS = (
    "releaseReady",
    "productAccepted",
    "gameplayValidated",
    "gpuAccepted",
    "visualAccepted",
    "visualRecoveryProven",
    "fissureFixed",
    "stable",
    "rootCauseReady",
)
BUILD_CONFIG_EXPECTED = {
    "architecture": "win32-i386",
    "waterExperimental": False,
    "nativeAutoLights": False,
    "x64Product": False,
}
TOP_LEVEL_KEYS = (
    "schema",
    "kind",
    "scope",
    "target",
    "files",
    "flags",
    "buildConfig",
    "limits",
)
TARGET_KEYS = ("path", "sha256", "size")
FILE_KEYS = ("sha256", "size")
HEX64 = re.compile(r"^[0-9A-Fa-f]{64}$")
PE_MACHINE_I386 = 0x14C
PE_MAGIC_PE32 = 0x10B
PE_MACHINE_AMD64 = 0x8664
PE_MAGIC_PE32PLUS = 0x20B


class AuditFailure(RuntimeError):
    pass


def _fail(message: str) -> None:
    raise AuditFailure(message)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest().upper()


def sha256_path(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def path_identity(path: Path) -> dict:
    return {"bytes": path.stat().st_size, "sha256": sha256_path(path)}


def _reject_constant(value: str) -> None:
    _fail("manifest contains non-finite JSON constant: " + value)


def _object_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            _fail("manifest contains duplicate JSON key: " + str(key))
        result[key] = value
    return result


def load_manifest(path: Path) -> dict:
    try:
        text = path.read_text(encoding="utf-8-sig")
    except OSError as exc:
        _fail("manifest unreadable: " + str(exc))
    try:
        value = json.loads(
            text,
            object_pairs_hook=_object_pairs,
            parse_constant=_reject_constant,
        )
    except AuditFailure:
        raise
    except json.JSONDecodeError as exc:
        _fail("manifest JSON invalid: " + str(exc))
    if not isinstance(value, dict):
        _fail("manifest root must be a JSON object")
    return value


def _require_exact_keys(value, expected, label: str) -> None:
    if not isinstance(value, dict):
        _fail(label + " must be a JSON object")
    actual = set(value)
    required = set(expected)
    if actual != required:
        extra = sorted(actual - required)
        missing = sorted(required - actual)
        _fail(label + " keys mismatch; extra=" + repr(extra) + ", missing=" + repr(missing))


def _parse_identity(value, label: str) -> dict:
    _require_exact_keys(value, FILE_KEYS, label)
    sha = value["sha256"]
    if not isinstance(sha, str) or HEX64.fullmatch(sha) is None:
        _fail(label + ".sha256 must be exactly 64 hex characters")
    size = value["size"]
    if type(size) is not int or size <= 0:
        _fail(label + ".size must be a positive JSON integer")
    return {"sha256": sha.upper(), "size": size}


def _parse_target(value) -> dict:
    _require_exact_keys(value, TARGET_KEYS, "manifest target")
    path = _safe_relpath(value["path"], "manifest target.path")
    if path != SUPPORTED_TARGET_PATH:
        _fail("manifest target.path must be exactly " + SUPPORTED_TARGET_PATH)
    sha = value["sha256"]
    if not isinstance(sha, str) or HEX64.fullmatch(sha) is None:
        _fail("manifest target.sha256 must be exactly 64 hex characters")
    size = value["size"]
    if type(size) is not int or size <= 0:
        _fail("manifest target.size must be a positive JSON integer")
    return {"path": path, "sha256": sha.upper(), "size": size}


def _parse_flags(value) -> dict:
    _require_exact_keys(value, REQUIRED_FLAGS, "manifest flags")
    result = {}
    for key in REQUIRED_FLAGS:
        flag = value[key]
        if type(flag) is not bool:
            _fail("manifest flags." + key + " must be a JSON boolean")
        if flag is not False:
            _fail("manifest flags." + key + " must remain false for an offline candidate")
        result[key] = False
    return result


def _parse_build_config(value) -> dict:
    _require_exact_keys(value, tuple(BUILD_CONFIG_EXPECTED), "manifest buildConfig")
    architecture = value["architecture"]
    if architecture != BUILD_CONFIG_EXPECTED["architecture"]:
        _fail("manifest buildConfig.architecture must be " + BUILD_CONFIG_EXPECTED["architecture"])
    result = {"architecture": architecture}
    for key in ("waterExperimental", "nativeAutoLights", "x64Product"):
        flag = value[key]
        if type(flag) is not bool:
            _fail("manifest buildConfig." + key + " must be a JSON boolean")
        if flag is not False:
            _fail(
                "manifest buildConfig." + key
                + " must be false; binary feature exclusion requires config provenance"
            )
        result[key] = False
    return result


def _parse_limits(value) -> list[str]:
    if not isinstance(value, list) or not value:
        _fail("manifest limits must be a non-empty JSON array")
    for item in value:
        if not isinstance(item, str) or not item.strip():
            _fail("manifest limits entries must be non-empty strings")
    return list(value)


def _safe_relpath(value, label: str) -> str:
    if not isinstance(value, str) or not value:
        _fail(label + " must be a non-empty string")
    if "\\" in value:
        _fail(label + " must use forward-slash relative paths")
    if ":" in value:
        _fail(label + " must not contain a colon, drive, or Windows ADS alias")
    path = PurePosixPath(value)
    if path.is_absolute() or str(path) != value:
        _fail(label + " must be an exact normalized relative path")
    for part in path.parts:
        if part in ("", ".", ".."):
            _fail(label + " must not contain empty, dot, or parent path components")
        if part.endswith((" ", ".")):
            _fail(label + " must not end a path component with space or dot")
        if part.split(".", 1)[0].upper() in WINDOWS_RESERVED:
            _fail(label + " must not use a Windows reserved device name")
    return value


def _check_scope(scope: str, relative: str) -> None:
    if scope == "player":
        allowed = relative in PLAYER_ROOT_FILES
    else:
        allowed = relative in AUTHOR_ROOT_FILES
        if not allowed and any(relative.startswith(prefix) for prefix in AUTHOR_PREFIXES):
            parts = {part.lower() for part in PurePosixPath(relative).parts}
            if parts & EVIDENCE_PATH_PARTS:
                _fail("author package contains an evidence path: " + relative)
            basename = PurePosixPath(relative).name
            suffix = PurePosixPath(relative).suffix.lower()
            if basename in AUTHOR_ROOT_FILES and basename != SUPPORTED_TARGET_PATH:
                allowed = True
            elif suffix in AUTHOR_ALLOWED_SUFFIXES:
                allowed = True
    if not allowed:
        _fail("file is outside the reviewed " + scope + " package scope: " + relative)


def _package_files(package_dir: Path) -> list[Path]:
    return sorted(path for path in package_dir.rglob("*") if path.is_file())


def _relative(package_dir: Path, path: Path) -> str:
    return path.relative_to(package_dir).as_posix()


def _resolve_package_file(package_dir: Path, relative: str) -> Path:
    _safe_relpath(relative, "package file path")
    candidate = (package_dir / relative).resolve()
    try:
        candidate.relative_to(package_dir)
    except ValueError:
        _fail("package file escapes the package root: " + relative)
    if not candidate.is_file():
        _fail("declared package file is missing: " + relative)
    return candidate


def _pe_identity(path: Path) -> dict:
    with path.open("rb") as stream:
        data = stream.read(4096)
    if len(data) < 0x40 or data[:2] != b"MZ":
        _fail("candidate is not a DOS/PE file: " + str(path))
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset + 26 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
        _fail("candidate has an invalid PE header: " + str(path))
    machine = struct.unpack_from("<H", data, pe_offset + 4)[0]
    characteristics = struct.unpack_from("<H", data, pe_offset + 22)[0]
    magic = struct.unpack_from("<H", data, pe_offset + 24)[0]
    return {
        "machine": machine,
        "machineHex": f"0x{machine:04X}",
        "magic": magic,
        "magicHex": f"0x{magic:04X}",
        "coffCharacteristicsHex": f"0x{characteristics:04X}",
        "largeAddressAware": bool(characteristics & 0x0020),
        "i386": machine == PE_MACHINE_I386 and magic == PE_MAGIC_PE32,
        "x64": machine == PE_MACHINE_AMD64 or magic == PE_MAGIC_PE32PLUS,
    }


def _check_membership(package_dir: Path, manifest_file: Path, declared: set[str]) -> None:
    try:
        manifest_relative = _relative(package_dir, manifest_file)
    except ValueError:
        _fail("manifest must live inside the package directory")
    actual = {_relative(package_dir, path) for path in _package_files(package_dir)}
    expected = set(declared) | {manifest_relative}
    if actual != expected:
        extra = sorted(actual - expected)
        missing = sorted(expected - actual)
        _fail("manifest membership mismatch; extra=" + repr(extra) + ", missing=" + repr(missing))


def _check_declared_files(package_dir: Path, files: dict) -> None:
    for relative, expected in files.items():
        path = _resolve_package_file(package_dir, relative)
        actual = path_identity(path)
        if actual["sha256"] != expected["sha256"] or actual["bytes"] != expected["size"]:
            _fail("declared file bytes do not match manifest identity: " + relative)


def _audit_archive(archive_path: Path, package_dir: Path) -> None:
    try:
        zip_file = zipfile.ZipFile(archive_path)
    except (OSError, zipfile.BadZipFile) as exc:
        _fail("archive unreadable: " + str(exc))
    with zip_file:
        bad = zip_file.testzip()
        if bad is not None:
            _fail("archive CRC failure: " + bad)
        names = sorted(name for name in zip_file.namelist() if not name.endswith("/"))
        disk_names = sorted(_relative(package_dir, path) for path in _package_files(package_dir))
        if names != disk_names:
            _fail("archive member set differs from package directory")
        for name in names:
            data = zip_file.read(name)
            disk_path = _resolve_package_file(package_dir, name)
            actual = path_identity(disk_path)
            if len(data) != actual["bytes"] or sha256_bytes(data) != actual["sha256"]:
                _fail("archive member differs from package directory: " + name)


def audit_package(
    package_dir: Path,
    manifest_path: Path | None = None,
    archive_path: Path | None = None,
    expect_sha256: str | None = None,
    expect_size: int | None = None,
) -> dict:
    package_dir = Path(package_dir).resolve()
    manifest_file = Path(manifest_path).resolve() if manifest_path else package_dir / "manifest.json"
    result = {
        "ok": False,
        "errors": [],
        "warnings": [],
        "packageDir": str(package_dir),
        "manifest": str(manifest_file),
        "manifestSha256": None,
        "fileCount": 0,
        "scope": None,
        "target": None,
        "flags": {},
        "buildConfig": {},
        "buildConfigProvenance": None,
        "buildEvidenceVerified": False,
        "claims": [],
    }
    try:
        if not package_dir.is_dir():
            _fail("package directory missing: " + str(package_dir))
        if not manifest_file.is_file():
            _fail("manifest missing: " + str(manifest_file))
        manifest = load_manifest(manifest_file)
        _require_exact_keys(manifest, TOP_LEVEL_KEYS, "manifest")
        if type(manifest["schema"]) is not int or manifest["schema"] != SUPPORTED_SCHEMA:
            _fail("manifest schema must be integer " + str(SUPPORTED_SCHEMA))
        if manifest["kind"] != SUPPORTED_KIND:
            _fail("manifest kind must be " + SUPPORTED_KIND)
        scope = manifest["scope"]
        if not isinstance(scope, str) or scope not in SUPPORTED_SCOPES:
            _fail("manifest scope must be player or author")
        target = _parse_target(manifest["target"])
        raw_files = manifest["files"]
        if not isinstance(raw_files, dict) or not raw_files:
            _fail("manifest files must be a non-empty JSON object")
        files = {}
        for relative, value in raw_files.items():
            relative = _safe_relpath(relative, "manifest files key")
            _check_scope(scope, relative)
            files[relative] = _parse_identity(value, "manifest files." + relative)
        if target["path"] not in files:
            _fail("manifest target.path must be listed in manifest files")
        if files[target["path"]] != {"sha256": target["sha256"], "size": target["size"]}:
            _fail("manifest target identity must exactly match its manifest files entry")
        flags = _parse_flags(manifest["flags"])
        build_config = _parse_build_config(manifest["buildConfig"])
        limits = _parse_limits(manifest["limits"])
        _check_membership(package_dir, manifest_file, set(files))
        _check_declared_files(package_dir, files)
        result["manifestSha256"] = path_identity(manifest_file)["sha256"]
        result["fileCount"] = len(_package_files(package_dir))
        target_path = _resolve_package_file(package_dir, target["path"])
        actual = path_identity(target_path)
        if actual["sha256"] != target["sha256"] or actual["bytes"] != target["size"]:
            _fail("target file bytes do not match manifest identity")
        expected_sha = str(expect_sha256 or target["sha256"]).upper()
        expected_size = expect_size if expect_size is not None else target["size"]
        if HEX64.fullmatch(expected_sha) is None:
            _fail("--expect-sha256 must be exactly 64 hex characters")
        if type(expected_size) is not int or expected_size <= 0:
            _fail("--expect-size must be a positive integer")
        if actual["sha256"] != expected_sha or actual["bytes"] != expected_size:
            _fail("target does not match the explicit expected identity")
        pe = _pe_identity(target_path)
        if not pe["i386"]:
            suffix = "x64/PE32+" if pe["x64"] else "non-PE32/i386"
            _fail("candidate must be PE32/i386, got " + suffix + " (" + pe["machineHex"] + "/" + pe["magicHex"] + ")")
        if archive_path is not None:
            _audit_archive(Path(archive_path).resolve(), package_dir)
        result["scope"] = scope
        result["flags"] = flags
        result["buildConfig"] = build_config
        result["buildConfigProvenance"] = "self-declared-not-independent"
        result["buildEvidenceVerified"] = False
        result["target"] = {
            "path": target["path"],
            "bytes": actual["bytes"],
            "sha256": actual["sha256"],
            "machine": "i386",
            "machineHex": pe["machineHex"],
            "pe": "PE32",
            "magicHex": pe["magicHex"],
            "coffCharacteristicsHex": pe["coffCharacteristicsHex"],
            "largeAddressAware": pe["largeAddressAware"],
            "x64": False,
            "declaredFilesChecked": len(files),
            "limits": limits,
        }
        result["ok"] = True
    except (AuditFailure, OSError, TypeError, ValueError) as exc:
        result["errors"].append(str(exc))
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-dir", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, default=None)
    parser.add_argument("--archive", type=Path, default=None)
    parser.add_argument("--expect-sha256", default=None)
    parser.add_argument("--expect-size", type=int, default=None)
    args = parser.parse_args(argv)
    result = audit_package(
        args.package_dir,
        manifest_path=args.manifest,
        archive_path=args.archive,
        expect_sha256=args.expect_sha256,
        expect_size=args.expect_size,
    )
    print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
