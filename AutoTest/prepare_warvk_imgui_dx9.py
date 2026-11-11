"""Generate the device-owned WarVK adapter from one pinned Dear ImGui backend.

This is a build input transformer, not a second checked-in renderer. It changes
only the explicit lifetime sites below. The shader/state/draw/texture-update
algorithm remains in subprojects/imgui/backends/imgui_impl_dx9.cpp. A backend
upgrade must audit this table and update the two exact upstream hashes; unknown
input fails before the output is touched. CRLF and LF checkouts of the same
audited source are accepted explicitly, not arbitrary whitespace changes.

Usage: python -B AutoTest/prepare_warvk_imgui_dx9.py --source <upstream.cpp>
       --output <build-directory/war3_imgui_dx9_backend.cpp>
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


UPSTREAM_SHA256 = frozenset((
    "D7943379E999CBC0BF02DE416FA55CDB7C8F9742F30D3611D2B585139B7A69C5",  # CRLF
    "ED1745EBFF7AD893F15B002D491CD005D368932859F98C20863A65B7CAED4180",  # LF
))
ENTRY_POINTS = (
    "Init", "Shutdown", "NewFrame", "RenderDrawData", "CreateDeviceObjects",
    "InvalidateDeviceObjects", "UpdateTexture",
)
GENERATED_BANNER = "// GENERATED WarVK DX9 ownership adapter; do not edit.\n"
NAMESPACE = "dxvk::war3::imgui_owned::"

# No regex/global AddRef or Release substitution: temporary state blocks,
# GetDirect3D results and application texture IDs retain upstream semantics.
# Each expected occurrence count is part of the reviewed contract.
EDITS = (
    (
        "borrow-device",
        "    bd->pd3dDevice->AddRef();",
        "    // WarVK: borrowed from the exact device owner; shutdown precedes owner destruction.",
        1,
    ),
    (
        "forget-borrowed-device",
        "    if (bd->pd3dDevice) { bd->pd3dDevice->Release(); }",
        "    bd->pd3dDevice = nullptr; // WarVK: no public or private device reference to release.",
        1,
    ),
    (
        "create-vertex-buffer",
        "device->CreateVertexBuffer(bd->VertexBufferSize * sizeof(CUSTOMVERTEX), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_CUSTOMVERTEX, D3DPOOL_DEFAULT, &bd->pVB, nullptr)",
        NAMESPACE + "CreateVertexBuffer(device, bd->VertexBufferSize * sizeof(CUSTOMVERTEX), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_CUSTOMVERTEX, D3DPOOL_DEFAULT, &bd->pVB, nullptr)",
        1,
    ),
    (
        "create-index-buffer",
        "device->CreateIndexBuffer(bd->IndexBufferSize * sizeof(ImDrawIdx), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, sizeof(ImDrawIdx) == 2 ? D3DFMT_INDEX16 : D3DFMT_INDEX32, D3DPOOL_DEFAULT, &bd->pIB, nullptr)",
        NAMESPACE + "CreateIndexBuffer(device, bd->IndexBufferSize * sizeof(ImDrawIdx), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, sizeof(ImDrawIdx) == 2 ? D3DFMT_INDEX16 : D3DFMT_INDEX32, D3DPOOL_DEFAULT, &bd->pIB, nullptr)",
        1,
    ),
    (
        "create-backend-texture",
        "bd->pd3dDevice->CreateTexture(tex->Width, tex->Height, 1, D3DUSAGE_DYNAMIC, bd->HasRgbaSupport ? D3DFMT_A8B8G8R8 : D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dx_tex, nullptr)",
        NAMESPACE + "CreateTexture(bd->pd3dDevice, tex->Width, tex->Height, 1, D3DUSAGE_DYNAMIC, bd->HasRgbaSupport ? D3DFMT_A8B8G8R8 : D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dx_tex, nullptr)",
        1,
    ),
    (
        "release-vertex-buffer",
        "if (bd->pVB) { bd->pVB->Release(); bd->pVB = nullptr; }",
        "if (bd->pVB) { " + NAMESPACE + "ReleaseVertexBuffer(bd->pVB); bd->pVB = nullptr; }",
        2,  # capacity growth and device invalidation
    ),
    (
        "release-index-buffer",
        "if (bd->pIB) { bd->pIB->Release(); bd->pIB = nullptr; }",
        "if (bd->pIB) { " + NAMESPACE + "ReleaseIndexBuffer(bd->pIB); bd->pIB = nullptr; }",
        2,
    ),
    (
        "release-backend-texture",
        "            backend_tex->Release();",
        "            " + NAMESPACE + "ReleaseTexture(backend_tex);",
        1,  # ImTextureData WantDestroy, not user-borrowed DrawCmd TexID
    ),
)


def apply_lifetime_edits(source: str) -> str:
    """Apply only reviewed sites; called only after transform's hash check.

    Kept separate so counterexamples can exercise the site guards as well as
    the outer hash guard. It does not accept or write paths.
    """
    for label, old, new, expected in EDITS:
        found = source.count(old)
        if found != expected:
            raise ValueError(f"{label}: expected {expected} exact sites, got {found}")
        if new in source:
            raise ValueError(f"{label}: replacement already present")
        source = source.replace(old, new)
    return source


def transform(source: bytes) -> bytes:
    digest = hashlib.sha256(source).hexdigest().upper()
    if digest not in UPSTREAM_SHA256:
        raise ValueError(f"unreviewed ImGui DX9 backend SHA-256: {digest}")
    text = source.decode("utf-8").replace("\r\n", "\n")
    body = apply_lifetime_edits(text)
    prologue = (
        GENERATED_BANNER
        + "// Source: subprojects/imgui/backends/imgui_impl_dx9.cpp (upstream license retained below).\n"
        + "// Only explicit ownership sites differ; re-audit before changing the pinned source.\n"
        + '#include "war3/ui/war3_imgui_dx9_ownership.h"\n'
        + '#include "war3/ui/war3_imgui_dx9.h"\n'
    )
    # Prefix both upstream declarations and definitions/internal calls. Product
    # code calls these explicit names; it must not depend on archive extraction
    # order deciding whether the public upstream backend is linked instead.
    prologue += "".join(
        f"#define ImGui_ImplDX9_{name} War3ImGui_ImplDX9_{name}\n"
        for name in ENTRY_POINTS
    )
    epilogue = "\n" + "".join(
        f"#undef ImGui_ImplDX9_{name}\n" for name in ENTRY_POINTS
    )
    return (prologue + "\n" + body + epilogue).encode("utf-8")


def generate(source: Path, output: Path) -> dict[str, object]:
    source = source.resolve(strict=True)
    output = output.resolve()
    if source == output or (output.exists() and output.samefile(source)):
        raise ValueError("output must not alias the upstream source")
    if output.parent == source.parent:
        raise ValueError("output must be outside the upstream backend directory")
    # Guard accidental clobber of an unrelated file. Incremental builds may
    # rewrite only this generator's own previous output.
    if output.exists() and not output.read_bytes().startswith(GENERATED_BANNER.encode("utf-8")):
        raise ValueError("refusing to overwrite a non-generated output")
    original = source.read_bytes()
    adapted = transform(original)
    output.write_bytes(adapted)
    return {
        "sourceSha256": hashlib.sha256(original).hexdigest().upper(),
        "outputSha256": hashlib.sha256(adapted).hexdigest().upper(),
        "sourceBytes": len(original),
        "outputBytes": len(adapted),
        "lifetimeSites": sum(edit[3] for edit in EDITS),
        "prefixedEntryPoints": len(ENTRY_POINTS),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(generate(args.source, args.output), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
