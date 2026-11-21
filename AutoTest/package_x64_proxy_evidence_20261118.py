"""CreateNew-only player package of the x64 render proxy with frame evidence and the performance log.

The same layout and checks as package_x64_proxy_beta_20261118.py, with the f13
builds and the evidence notes. Never deploys, never touches a game directory,
never uploads.

usage: package_x64_proxy_evidence_20261118.py --round e1
"""
from __future__ import annotations

import argparse
import hashlib
import json
import zipfile

import package_x64_proxy_beta_20261118 as base

NAME = "WarVK-x64-Evidence-20261118"
TEMPLATE = base.AUTOTEST / "x64_proxy_evidence"
X86_DLL = base.ROOT / "build32_device_owner_f16_20261119_a/src/d3d9/d3d9.dll"
HOST = base.ROOT / "build64_render_host_f16_20261119_a/tools/render_host/dxvk_core/warvk-d3d9-host-dev.exe"
KIT = base.KIT


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--round", required=True)
    args = parser.parse_args()
    output = base.AUTOTEST / "artifacts" / f"x64-proxy-evidence-20261118-{args.round}"
    package = output / NAME
    archive = output / f"{NAME}.zip"
    output.mkdir(parents=True)
    base.require(base.sha256(base.FRONT) == base.FRONT_SHA256, "front end differs from the frozen f8 file")

    binaries = {
        "d3d9.dll": base.strip_copy(X86_DLL, package / "d3d9.dll", base.I386),
        f"{KIT}/warvk-d3d9-front.dll": base.strip_copy(base.FRONT, package / KIT / "warvk-d3d9-front.dll", base.I386),
        f"{KIT}/warvk-d3d9-host.exe": base.strip_copy(HOST, package / KIT / "warvk-d3d9-host.exe", base.AMD64),
    }
    base.create_bytes(package / KIT / "dxvk.conf", (base.TEMPLATE / "dxvk.conf").read_bytes())
    readme = (TEMPLATE / "README.txt").read_text(encoding="utf-8")
    base.require("@@" not in readme, "unfilled placeholder in the player notes")
    base.create_bytes(package / "WarVK取证版说明.txt", b"\xef\xbb\xbf" + base.crlf(readme))
    for source, name in base.LICENSES.items():
        base.create_bytes(package / KIT / "licenses" / name, (base.ROOT / source).read_bytes())

    files = sorted(p for p in package.rglob("*") if p.is_file())
    sums = "".join(f"{base.sha256(p)}  {p.relative_to(package).as_posix()}\n" for p in files)
    base.create_bytes(package / KIT / "SHA256SUMS.txt", sums.encode("utf-8"))
    files = sorted(p for p in package.rglob("*") if p.is_file())

    # The archive root is the game directory: no enclosing folder.
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as stream:
        for path in files:
            stream.write(path, path.relative_to(package).as_posix())
    with zipfile.ZipFile(archive) as stream:
        base.require(stream.testzip() is None, "archive CRC failure")
        members = sorted(n for n in stream.namelist() if not n.endswith("/"))
        base.require(members == sorted(p.relative_to(package).as_posix() for p in files),
                     "archive members differ from the package directory")
        for path in files:
            data = stream.read(path.relative_to(package).as_posix())
            base.require(hashlib.sha256(data).hexdigest() == base.sha256(path), f"archive differs: {path.name}")

    receipt = dict(schema="warvk-x64-proxy-evidence/v1", deployed=False, uploaded=False, name=NAME,
                   binaries=binaries, files=len(files), archive=base.identity(archive))
    base.create_bytes(output / "receipt.json", json.dumps(receipt, ensure_ascii=False, indent=1).encode("utf-8"))
    print(json.dumps(dict(package=str(package), archive=str(archive), files=len(files),
                          archiveMiB=round(archive.stat().st_size / 1048576, 1)), ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
