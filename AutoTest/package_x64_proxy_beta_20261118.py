"""CreateNew-only player beta package of the x64 render proxy: unpack into the game directory and play.

Never deploys, never touches a game directory, never uploads. The binaries are
stripped copies of the frozen builds named below; the unstripped originals stay
in their build directories and are recorded in the receipt, so a player's crash
address can still be resolved.

usage: package_x64_proxy_beta_20261118.py --round b1
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import zipfile

AUTOTEST = Path(__file__).resolve().parent
ROOT = AUTOTEST.parent
TEMPLATE = AUTOTEST / "x64_proxy_beta"
NAME = "WarVK-x64-Beta-20261118b"
TOOLS = Path("E:/Dev/Toolchains/warvk-rh0-llvm-mingw-20260616/portable/"
             "llvm-mingw-20260616-ucrt-x86_64/bin")

X86_RECEIPT = AUTOTEST / "artifacts/x86-f10-build-20261118-r1/receipt.json"
X86_DLL = ROOT / "build32_device_owner_f10_20261118_a/src/d3d9/d3d9.dll"
FRONT = AUTOTEST / "artifacts/x64-author-trial-kit-20261118/warvk-d3d9-front.dll"
FRONT_SHA256 = "14e995b67b7435abb5535f0019e855077d5f50eebd602885ca0d0c0600609ab4"
HOST = ROOT / "build64_render_host_f8_20261118_a/tools/render_host/dxvk_core/warvk-d3d9-host-dev.exe"
HOST_SHA256 = "8c88c9d8d3daacfedda0b5b5a708c55016d880d1fc9f147ba4429c7c500cadf2"
KIT = "WarVK/x64"

LICENSES = {
    "COPYING": "WarVK-COPYING.txt",
    "LICENSE": "DXVK-LICENSE.txt",
    "src/minhook/LICENSE.txt": "MinHook-LICENSE.txt",
    "subprojects/imgui/LICENSE.txt": "imgui-LICENSE.txt",
    "subprojects/dxbc-spirv/LICENSE": "dxbc-spirv-LICENSE.txt",
    "subprojects/libdisplay-info/LICENSE": "libdisplay-info-LICENSE.txt",
    "smaa/LICENSE.txt": "SMAA-LICENSE.txt",
    "include/vulkan/LICENSE.md": "Vulkan-Headers-LICENSE.md",
    "include/spirv/LICENSE": "SPIRV-Headers-LICENSE.txt",
    "subprojects/StormBreaker/LICENSE.txt": "StormBreaker-LICENSE.txt",
}
# Only DLLs every supported Windows has. A toolchain runtime here would mean a
# player's machine cannot start the file.
SYSTEM_IMPORTS = re.compile(
    r"^(api-ms-win-[a-z0-9-]+|kernel32|user32|gdi32|advapi32|shell32|ole32|oleaut32|"
    r"dbghelp|dwmapi|bcrypt|setupapi|ws2_32|winmm|imm32|version|psapi|ntdll|shlwapi|"
    r"crypt32|secur32|uxtheme|comdlg32)\.dll$", re.I)
I386, AMD64 = 0x14C, 0x8664


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def identity(path: Path) -> dict:
    return {"size": path.stat().st_size, "sha256": sha256(path)}


def require(value, message: str) -> None:
    if not value:
        raise RuntimeError(message)


def create_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(data)


def crlf(text: str) -> bytes:
    return text.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8")


def machine(path: Path) -> int:
    data = path.read_bytes()[:4096]
    require(data[:2] == b"MZ", f"not a PE file: {path.name}")
    offset = struct.unpack_from("<I", data, 0x3C)[0]
    require(data[offset:offset + 4] == b"PE\0\0", f"bad PE header: {path.name}")
    return struct.unpack_from("<H", data, offset + 4)[0]


def imports(path: Path) -> list[str]:
    text = subprocess.run([str(TOOLS / "llvm-objdump.exe"), "-p", str(path)], check=True,
                          capture_output=True, text=True, errors="replace").stdout
    return sorted(set(re.findall(r"DLL Name:\s*(\S+)", text)))


def strip_copy(source: Path, target: Path, expected_machine: int) -> dict:
    target.parent.mkdir(parents=True, exist_ok=True)
    require(not target.exists(), f"target exists: {target}")
    subprocess.run([str(TOOLS / "llvm-strip.exe"), "--strip-unneeded", "-o", str(target), str(source)],
                   check=True)
    require(machine(target) == expected_machine, f"wrong machine: {target.name}")
    names = imports(target)
    foreign = [name for name in names if not SYSTEM_IMPORTS.match(name)]
    require(not foreign, f"{target.name} needs non-system DLLs: {foreign}")
    return dict(source=str(source.relative_to(ROOT)), unstripped=identity(source),
                packaged=identity(target), imports=names)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--round", required=True)
    args = parser.parse_args()
    output = AUTOTEST / "artifacts" / f"x64-proxy-beta-20261118-{args.round}"
    package = output / NAME
    archive = output / f"{NAME}.zip"
    output.mkdir(parents=True)

    build = json.loads(X86_RECEIPT.read_text(encoding="utf-8"))
    require(build.get("ok") is True and build.get("deployed") is False, "x86 build receipt is not clean")
    require(build["dll"]["sha256"].lower() == sha256(X86_DLL), "x86 DLL differs from its build receipt")
    require(sha256(FRONT) == FRONT_SHA256, "front end differs from the frozen f8 file")
    require(sha256(HOST) == HOST_SHA256, "host differs from the frozen f8 file")

    binaries = {
        "d3d9.dll": strip_copy(X86_DLL, package / "d3d9.dll", I386),
        f"{KIT}/warvk-d3d9-front.dll": strip_copy(FRONT, package / KIT / "warvk-d3d9-front.dll", I386),
        f"{KIT}/warvk-d3d9-host.exe": strip_copy(HOST, package / KIT / "warvk-d3d9-host.exe", AMD64),
    }
    create_bytes(package / KIT / "dxvk.conf", (TEMPLATE / "dxvk.conf").read_bytes())
    readme = (TEMPLATE / "README.txt").read_text(encoding="utf-8").replace(
        "@@STATUS@@", (TEMPLATE / "status.txt").read_text(encoding="utf-8").strip())
    require("@@" not in readme, "unfilled placeholder in the player notes")
    create_bytes(package / "WarVK内测版说明.txt", b"\xef\xbb\xbf" + crlf(readme))
    for source, name in LICENSES.items():
        create_bytes(package / KIT / "licenses" / name, (ROOT / source).read_bytes())

    files = sorted(p for p in package.rglob("*") if p.is_file())
    sums = "".join(f"{sha256(p)}  {p.relative_to(package).as_posix()}\n" for p in files)
    create_bytes(package / KIT / "SHA256SUMS.txt", sums.encode("utf-8"))
    files = sorted(p for p in package.rglob("*") if p.is_file())

    # The archive root is the game directory: no enclosing folder.
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as stream:
        for path in files:
            stream.write(path, path.relative_to(package).as_posix())
    with zipfile.ZipFile(archive) as stream:
        require(stream.testzip() is None, "archive CRC failure")
        members = sorted(n for n in stream.namelist() if not n.endswith("/"))
        require(members == sorted(p.relative_to(package).as_posix() for p in files),
                "archive members differ from the package directory")
        for path in files:
            data = stream.read(path.relative_to(package).as_posix())
            require(hashlib.sha256(data).hexdigest() == sha256(path), f"archive differs: {path.name}")

    receipt = dict(schema="warvk-x64-proxy-beta/v2", deployed=False, uploaded=False, name=NAME,
                   binaries=binaries, x86BuildReceipt=str(X86_RECEIPT.relative_to(ROOT)),
                   files=len(files), archive=identity(archive))
    create_bytes(output / "receipt.json",
                 json.dumps(receipt, ensure_ascii=False, indent=1).encode("utf-8"))
    print(json.dumps(dict(package=str(package), archive=str(archive), files=len(files),
                          archiveMiB=round(archive.stat().st_size / 1048576, 1)), ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
