"""Strict reader for one schema-3 input draw's ``renderInputEvidence`` block.

This is an observation contract for PreparedShadowReplay, not the shared-core
A2 protocol. An available native draw state does not prove native ownership,
texture pixels, complete scene coverage, or that a GPU draw executed. Its frame
is a ShadowProducerSerial; a CPU event/batch frame belongs to another domain.
Diffuse material words do not certify the final shader color or evaluated GEOA.
"""

from __future__ import annotations

import re
import struct


NATIVE_SCHEMA = 1
NATIVE_FRAME_DOMAIN = "ShadowProducerSerial"
NATIVE_CAPTURE_STAGE = "PreparedShadowReplay"
NATIVE_UINT32_MAX = (1 << 32) - 1
NATIVE_UINT64_MAX = (1 << 64) - 1

# Fixed wire orders from war3_native_draw_material_facts.h. These are state IDs,
# not the state values carried in the JSON arrays.
NATIVE_RENDER_STATES = (
    15, 25, 24, 27, 19, 20, 171, 206, 207, 208, 209, 7, 14, 23, 22, 168, 60,
)
NATIVE_STAGE_STATES = (1, 2, 3, 4, 5, 6, 11, 24, 26, 27)
NATIVE_SAMPLER_STATES = (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13)
NATIVE_SAMPLER_MIPMAP_LOD_BIAS_INDEX = 7
NATIVE_MATERIAL_FIELDS = (
    "DiffuseR", "DiffuseG", "DiffuseB", "DiffuseA",
    "AmbientR", "AmbientG", "AmbientB", "AmbientA",
    "SpecularR", "SpecularG", "SpecularB", "SpecularA",
    "EmissiveR", "EmissiveG", "EmissiveB", "EmissiveA", "Power",
)
NATIVE_TEXTURE_TRANSFORM_WORD_COUNT = 16
NATIVE_ARRAY_LENGTHS = (
    ("renderStates", len(NATIVE_RENDER_STATES)),
    ("textureStage0", len(NATIVE_STAGE_STATES)),
    ("sampler0", len(NATIVE_SAMPLER_STATES)),
    ("materialBits", len(NATIVE_MATERIAL_FIELDS)),
    ("textureTransform0Bits", NATIVE_TEXTURE_TRANSFORM_WORD_COUNT),
)
NATIVE_REQUIRED_SHARED_FIELDS_MISSING = (
    "nativeInstanceLifecycle",
    "originalMdxGeosetMapping",
    "evaluatedGeoaAlphaRgb",
    "animationClock",
    "texturePixels",
    "allScenePassCoverage",
)
NATIVE_RENDER_INPUT_KEYS = (
    "schema", "captureStage", "finalShadowMaterial",
    "nativeDrawMaterialObservation", "requiredSharedFieldsMissing",
)
NATIVE_FINAL_SHADOW_MATERIAL_KEYS = (
    "alphaRefBits", "alphaTest", "alphaBlend", "depthTest", "depthWrite",
    "additive", "uvBinding", "uvStride", "uvOffset", "uvFormat",
    "textureView", "sampler", "samplerIndex",
)
NATIVE_DRAW_MATERIAL_KEYS = (
    "schema", "available", "frameDomain", "sourceFrame", "mapEpoch",
    "deviceEpoch", "part", "layer", "shaderMask", "renderStates",
    "textureStage0", "sampler0", "materialBits", "textureTransform0Bits",
)

_DECIMAL_U64 = re.compile(r"(?:0|[1-9][0-9]*)\Z")
_IDENTITY_FIELDS = ("sourceFrame", "mapEpoch", "deviceEpoch", "part")
_FINAL_BOOL_FIELDS = ("alphaTest", "alphaBlend", "depthTest", "depthWrite", "additive")
_FINAL_UINT32_FIELDS = ("uvBinding", "uvStride", "uvOffset", "uvFormat", "samplerIndex")
_FLOAT_ARRAY_FIELDS = frozenset(("materialBits", "textureTransform0Bits"))


def _object(value: object, keys: tuple[str, ...], path: str) -> dict:
    if type(value) is not dict:
        raise ValueError(f"{path} must be an object")
    if any(type(key) is not str for key in value) or set(value) != set(keys):
        raise ValueError(f"{path} must have exactly the declared keys")
    return value


def _uint32(value: object, path: str) -> int:
    # bool is deliberately excluded even though Python treats it as an int.
    if type(value) is not int or not 0 <= value <= NATIVE_UINT32_MAX:
        raise ValueError(f"{path} must be a uint32 integer")
    return value


def _uint64_decimal(value: object, path: str) -> str:
    if (type(value) is not str or len(value) > 20
            or _DECIMAL_U64.fullmatch(value) is None
            or int(value) > NATIVE_UINT64_MAX):
        raise ValueError(f"{path} must be a canonical uint64 decimal string")
    return value


def _boolean(value: object, path: str) -> bool:
    if type(value) is not bool:
        raise ValueError(f"{path} must be a boolean")
    return value


def _fixed(value: object, expected: object, path: str) -> None:
    if type(value) is not type(expected) or value != expected:
        raise ValueError(f"{path} must be {expected!r}")


def _finite_float_bits(value: object, path: str) -> int:
    bits = _uint32(value, path)
    if bits & 0x7F800000 == 0x7F800000:
        raise ValueError(f"{path} must encode a finite binary32 value")
    # Retain the original word: decoding/re-encoding can lose signed zero or
    # other raw-bit evidence. No normalized float is returned to consumers.
    return bits


def _final_shadow_material(value: object) -> dict:
    path = "renderInputEvidence.finalShadowMaterial"
    material = _object(value, NATIVE_FINAL_SHADOW_MATERIAL_KEYS, path)
    alpha_bits = _finite_float_bits(material["alphaRefBits"], f"{path}.alphaRefBits")
    alpha = struct.unpack("!f", struct.pack("!I", alpha_bits))[0]
    if not 0.0 <= alpha <= 1.0:
        raise ValueError(f"{path}.alphaRefBits must encode a value in [0, 1]")
    for name in _FINAL_BOOL_FIELDS:
        _boolean(material[name], f"{path}.{name}")
    for name in _FINAL_UINT32_FIELDS:
        _uint32(material[name], f"{path}.{name}")
    for name in ("textureView", "sampler"):
        _uint64_decimal(material[name], f"{path}.{name}")
    return material.copy()


def validate_render_input_evidence(
    block: object, *, part: str, layer: int, map_epoch: str, device_epoch: str,
) -> dict:
    """Validate a per-draw block and return a detached, conservative summary.

    Raises ValueError for malformed fields or an available observation whose
    part/layer/epochs differ from the enclosing draw. ``sourceFrame`` has its
    own producer domain and is never compared to a CPU event or batch frame.
    The caller supplies identity already associated with the enclosing draw;
    this function neither fetches native memory nor certifies object lifetime.
    """
    _uint64_decimal(part, "draw.part")
    _uint32(layer, "draw.layer")
    _uint64_decimal(map_epoch, "draw.mapEpoch")
    _uint64_decimal(device_epoch, "draw.deviceEpoch")

    path = "renderInputEvidence"
    evidence = _object(block, NATIVE_RENDER_INPUT_KEYS, path)
    _fixed(evidence["schema"], NATIVE_SCHEMA, f"{path}.schema")
    _fixed(evidence["captureStage"], NATIVE_CAPTURE_STAGE, f"{path}.captureStage")
    final_material = _final_shadow_material(evidence["finalShadowMaterial"])
    missing = evidence["requiredSharedFieldsMissing"]
    if (type(missing) is not list
            or any(type(item) is not str for item in missing)
            or missing != list(NATIVE_REQUIRED_SHARED_FIELDS_MISSING)):
        raise ValueError(f"{path}.requiredSharedFieldsMissing must be the ordered fixed list")

    native_path = f"{path}.nativeDrawMaterialObservation"
    native = _object(evidence["nativeDrawMaterialObservation"], NATIVE_DRAW_MATERIAL_KEYS,
                     native_path)
    _fixed(native["schema"], NATIVE_SCHEMA, f"{native_path}.schema")
    available = _boolean(native["available"], f"{native_path}.available")
    _fixed(native["frameDomain"], NATIVE_FRAME_DOMAIN, f"{native_path}.frameDomain")
    for name in _IDENTITY_FIELDS:
        _uint64_decimal(native[name], f"{native_path}.{name}")
    _uint32(native["layer"], f"{native_path}.layer")
    shader_mask = _uint32(native["shaderMask"], f"{native_path}.shaderMask")
    if shader_mask > 3:
        raise ValueError(f"{native_path}.shaderMask has unknown shader bits")
    for name, length in NATIVE_ARRAY_LENGTHS:
        words = native[name]
        if type(words) is not list or len(words) != length:
            raise ValueError(f"{native_path}.{name} must be an array of {length} words")
        validator = _finite_float_bits if name in _FLOAT_ARRAY_FIELDS else _uint32
        for index, word in enumerate(words):
            validator(word, f"{native_path}.{name}[{index}]")
    _finite_float_bits(native["sampler0"][NATIVE_SAMPLER_MIPMAP_LOD_BIAS_INDEX],
                       f"{native_path}.sampler0[{NATIVE_SAMPLER_MIPMAP_LOD_BIAS_INDEX}]")

    if available:
        for name in _IDENTITY_FIELDS:
            if native[name] == "0":
                raise ValueError(f"{native_path}.{name} must be nonzero when available")
        expected = {
            "part": part, "layer": layer,
            "mapEpoch": map_epoch, "deviceEpoch": device_epoch,
        }
        for name, identity in expected.items():
            if native[name] != identity:
                raise ValueError(f"{native_path}.{name} does not match the enclosing draw")
    else:
        # Unknown is represented only by a completely zeroed payload. Do not
        # backfill the draw's known identity into an unavailable observation.
        if (any(native[name] != "0" for name in _IDENTITY_FIELDS)
                or native["layer"] != 0 or shader_mask != 0
                or any(any(native[name]) for name, _ in NATIVE_ARRAY_LENGTHS)):
            raise ValueError(f"{native_path} must have an all-zero payload when unavailable")

    return {
        "nativeDrawStateAvailable": available,
        "sourceFrame": native["sourceFrame"],
        "sourceFrameDomain": NATIVE_FRAME_DOMAIN,
        "materialDiffuseBits": native["materialBits"][:4] if available else None,
        "finalShadowMaterial": final_material,
        "sharedRendererReady": False,
        "requiredSharedFieldsMissing": list(NATIVE_REQUIRED_SHARED_FIELDS_MISSING),
    }
