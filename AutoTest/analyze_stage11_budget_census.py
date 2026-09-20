"""Strict Stage11 census reader. Cache accounting, NOT GPU residency/recovery proof."""
import argparse
import json
from pathlib import Path

OWNERS = ["touched", "failed", "recentCache", "coldCache", "retired"]
REASONS = ["gpuAuthored", "nonHostCached", "missingIdentity", "missingGeneration", "missingSpan"]
TAG_DIMENSIONS = [
    "staticTagOnly",
    "notStaticTagOnly",
    "mixedTagOverlap",
    "failedRetainedUnion",
    "touchedUnion",
    "touchedAndFailedUnion",
    "touchedOrFailedUnion",
    "tagUnknownUnion",
]

V1_TOP = {
    "schema", "enabled", "scope", "physicalBackingComplete",
    "gpuCompletionKnown", "ownerCategories", "unknownReasons", "samples",
}
V2_TOP = V1_TOP | {"tagDimensions"}
V1_PAGE = {
    "id", "active", "capacity", "used", "notReferencedByCacheBytes", "tail",
    "references", "staticReferences", "pageReferences", "knownAgeReferences",
    "minAccessAge", "maxAccessAge", "owned",
}
V2_PAGE = V1_PAGE | {
    "tagUnknownReferences", "staticTaggedUnionBytes", "notStaticTaggedUnionBytes",
    "staticTagOnlyBytes", "notStaticTagOnlyBytes", "mixedTagOverlapBytes",
    "failedRetainedUnionBytes", "touchedUnionBytes", "touchedAndFailedUnionBytes",
    "touchedOrFailedUnionBytes", "tagUnknownUnionBytes",
}
SAMPLE_KEYS = {
    "deviceIdentity", "mapEpoch", "deviceEpoch", "frame", "stage", "errors", "cap",
    "activeResident", "capacityRejects", "uploadRangeHits", "sampleCpuUs",
    "entryCount", "sliceCount", "complete", "unknownCounts",
    "unknownPositionBytes", "pages",
}
TAG_AGG_KEYS = (
    "owned", "staticOnly", "notStaticOnly", "mixed", "staticUnion",
    "notStaticUnion", "failed", "touched", "touchedFailed",
    "touchedOrFailed", "tagUnknown",
)


def unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate key: {key}")
        result[key] = value
    return result


def uint(value):
    if type(value) is not int or not 0 <= value <= (1 << 64) - 1:
        raise ValueError("invalid unsigned integer")
    return value


def _require_keys(mapping, expected, label):
    if not isinstance(mapping, dict) or set(mapping) != expected:
        raise ValueError(f"{label} keys")


def _zero_tag_aggregate():
    return {key: 0 for key in TAG_AGG_KEYS}


def _tag_bytes(aggregate):
    return {
        "staticTagOnly": aggregate["staticOnly"],
        "notStaticTagOnly": aggregate["notStaticOnly"],
        "mixedTagOverlap": aggregate["mixed"],
        "staticTaggedUnion": aggregate["staticUnion"],
        "notStaticTaggedUnion": aggregate["notStaticUnion"],
        "failedRetainedUnion": aggregate["failed"],
        "touchedUnion": aggregate["touched"],
        "touchedAndFailedUnion": aggregate["touchedFailed"],
        "touchedOrFailedUnion": aggregate["touchedOrFailed"],
        "tagUnknownUnion": aggregate["tagUnknown"],
    }


def _ratio(part, total):
    return (part / total) if total else None


def _validate_scalars(sample):
    scalars = {
        "deviceIdentity", "mapEpoch", "deviceEpoch", "frame", "stage", "errors",
        "cap", "activeResident", "capacityRejects", "uploadRangeHits",
        "sampleCpuUs", "entryCount", "sliceCount",
    }
    if set(sample) != SAMPLE_KEYS:
        raise ValueError("sample keys")
    for key in scalars:
        uint(sample[key])
    if type(sample["complete"]) is not bool or sample["complete"] != (sample["errors"] == 0):
        raise ValueError("incomplete hidden")
    for key in ("unknownCounts", "unknownPositionBytes"):
        if not isinstance(sample[key], list) or len(sample[key]) != len(REASONS):
            raise ValueError("reason bucket shape")
        for value in sample[key]:
            uint(value)


def _validate_page_common(page, schema):
    expected = V1_PAGE if schema == 1 else V2_PAGE
    if set(page) != expected or type(page["active"]) is not bool:
        raise ValueError("page keys")
    for key in expected:
        if key != "active" and key != "owned":
            uint(page[key])
    if page["id"] == 0 or page["capacity"] == 0:
        raise ValueError("page identity/capacity")
    if not isinstance(page["owned"], list) or len(page["owned"]) != len(OWNERS):
        raise ValueError("owner buckets")
    owned = sum(uint(value) for value in page["owned"])
    if (
        page["used"] > page["capacity"]
        or page["capacity"] > 512 * 1024**2
        or page["used"] % 256
        or page["capacity"] % 256
        or owned + page["notReferencedByCacheBytes"] != page["used"]
        or page["used"] + page["tail"] != page["capacity"]
    ):
        raise ValueError("page byte closure")
    if (
        page["staticReferences"] > page["references"]
        or page["knownAgeReferences"] > page["references"]
        or page["minAccessAge"] > page["maxAccessAge"]
    ):
        raise ValueError("reference/age bounds")
    if schema == 2 and page["tagUnknownReferences"] > page["references"]:
        raise ValueError("tag unknown reference bounds")
    return owned


def _validate_tag_page(page, owned):
    static_only = uint(page["staticTagOnlyBytes"])
    not_static_only = uint(page["notStaticTagOnlyBytes"])
    mixed = uint(page["mixedTagOverlapBytes"])
    static_union = uint(page["staticTaggedUnionBytes"])
    not_static_union = uint(page["notStaticTaggedUnionBytes"])
    failed = uint(page["failedRetainedUnionBytes"])
    touched = uint(page["touchedUnionBytes"])
    touched_failed = uint(page["touchedAndFailedUnionBytes"])
    touched_or_failed = uint(page["touchedOrFailedUnionBytes"])
    tag_unknown = uint(page["tagUnknownUnionBytes"])
    if static_only + not_static_only + mixed != owned:
        raise ValueError("tag partition closure")
    if static_union != static_only + mixed or not_static_union != not_static_only + mixed:
        raise ValueError("tag union derivation")
    if (
        failed > owned
        or touched > owned
        or touched_failed > failed
        or touched_failed > touched
        or touched_or_failed > owned
        or touched_or_failed + touched_failed != failed + touched
        or touched_or_failed < max(failed, touched)
        or tag_unknown > owned
    ):
        raise ValueError("tag cross-stat closure")
    if (tag_unknown == 0) != (page["tagUnknownReferences"] == 0):
        raise ValueError("tag unknown reference/union consistency")
    return {
        "staticOnly": static_only,
        "notStaticOnly": not_static_only,
        "mixed": mixed,
        "staticUnion": static_union,
        "notStaticUnion": not_static_union,
        "failed": failed,
        "touched": touched,
        "touchedFailed": touched_failed,
        "touchedOrFailed": touched_or_failed,
        "tagUnknown": tag_unknown,
    }


def _scope_dimensions(all_agg, active_agg, available):
    retired_agg = {key: all_agg[key] - active_agg[key] for key in TAG_AGG_KEYS}
    if not available:
        return {
            "tagDimensionsAvailable": False,
            "tagBytesActive": None,
            "tagBytesRetired": None,
            "tagBytesAll": None,
            "tagUnknownRatioActive": None,
            "tagUnknownRatioRetired": None,
            "tagUnknownRatioAll": None,
        }
    return {
        "tagDimensionsAvailable": True,
        "tagBytesActive": _tag_bytes(active_agg),
        "tagBytesRetired": _tag_bytes(retired_agg),
        "tagBytesAll": _tag_bytes(all_agg),
        "tagUnknownRatioActive": _ratio(active_agg["tagUnknown"], active_agg["owned"]),
        "tagUnknownRatioRetired": _ratio(retired_agg["tagUnknown"], retired_agg["owned"]),
        "tagUnknownRatioAll": _ratio(all_agg["tagUnknown"], all_agg["owned"]),
    }


def _validate(data, schema):
    top = V1_TOP if schema == 1 else V2_TOP
    _require_keys(data, top, "census schema")
    if type(data["schema"]) is not int or data["schema"] != schema:
        raise ValueError("census schema")
    if (
        data["scope"] != "retained-page-and-cache-slices"
        or data["physicalBackingComplete"] is not False
        or data["gpuCompletionKnown"] is not False
        or data["ownerCategories"] != OWNERS
        or data["unknownReasons"] != REASONS
    ):
        raise ValueError("unsupported evidence strength or categories")
    if schema == 2 and data["tagDimensions"] != TAG_DIMENSIONS:
        raise ValueError("tag dimensions")
    samples = data["samples"]
    if not isinstance(samples, list) or len(samples) > 4 or (not data["enabled"] and samples):
        raise ValueError("sample count or disabled producer")
    summaries = []
    identity = last_stage = last_frame = None
    for sample in samples:
        _validate_scalars(sample)
        if not 0 <= sample["stage"] < 4:
            raise ValueError("stage range")
        if schema == 2 and sample["deviceIdentity"] == 0:
            raise ValueError("missing device identity")
        current_identity = tuple(sample[key] for key in ("deviceIdentity", "mapEpoch", "deviceEpoch"))
        if identity is not None and identity != current_identity:
            raise ValueError("cross epoch cut")
        identity = current_identity
        if last_stage is not None:
            if not last_stage < sample["stage"] or sample["frame"] <= last_frame:
                raise ValueError("sample order")
        last_stage, last_frame = sample["stage"], sample["frame"]
        if sample["sliceCount"] > 32768 or sample["entryCount"] > 16384 or len(sample["pages"]) > 128:
            raise ValueError("producer limit")
        seen_pages = set()
        reference_sum = static_reference_sum = known_age_sum = tag_unknown_reference_sum = 0
        resident = all_retained = active_retained = 0
        all_cache_bytes = active_cache_bytes = 0
        all_unattributed = active_unattributed = 0
        all_tails = active_tails = 0
        all_agg = _zero_tag_aggregate()
        active_agg = _zero_tag_aggregate()
        for page in sample["pages"]:
            owned = _validate_page_common(page, schema)
            if page["id"] in seen_pages:
                raise ValueError("page identity alias")
            seen_pages.add(page["id"])
            if schema == 2:
                dims = _validate_tag_page(page, owned)
                for key in TAG_AGG_KEYS:
                    if key != "owned":
                        all_agg[key] += dims[key]
                        if page["active"]:
                            active_agg[key] += dims[key]
            elif page["staticReferences"] > page["references"]:
                raise ValueError("old static reference bounds")
            all_agg["owned"] += owned
            if page["active"]:
                active_agg["owned"] += owned
            reference_sum += page["references"]
            static_reference_sum += page["staticReferences"]
            known_age_sum += page["knownAgeReferences"]
            if schema == 2:
                tag_unknown_reference_sum += page["tagUnknownReferences"]
            all_retained += page["capacity"]
            all_cache_bytes += owned
            all_unattributed += page["notReferencedByCacheBytes"]
            all_tails += page["tail"]
            if page["active"]:
                resident += page["capacity"]
                active_retained += page["capacity"]
                active_cache_bytes += owned
                active_unattributed += page["notReferencedByCacheBytes"]
                active_tails += page["tail"]
        if resident != sample["activeResident"]:
            raise ValueError("resident rows mismatch")
        if reference_sum != sample["sliceCount"]:
            raise ValueError("slice reference closure")
        if static_reference_sum > sample["sliceCount"]:
            raise ValueError("static reference closure")
        if known_age_sum > sample["sliceCount"]:
            raise ValueError("known age reference closure")
        if schema == 2 and tag_unknown_reference_sum > sample["sliceCount"]:
            raise ValueError("tag unknown reference closure")
        retired_retained = all_retained - active_retained
        retired_cache_bytes = all_cache_bytes - active_cache_bytes
        retired_unattributed = all_unattributed - active_unattributed
        retired_tails = all_tails - active_tails
        summary = dict(
            stage=sample["stage"],
            frame=sample["frame"],
            complete=sample["complete"],
            activeResident=resident,
            cacheReferencedCapacity=active_cache_bytes,
            cacheReferencedCapacityActive=active_cache_bytes,
            cacheReferencedCapacityRetired=retired_cache_bytes,
            cacheReferencedCapacityAll=all_cache_bytes,
            cacheUnattributedUsed=active_unattributed,
            cacheUnattributedUsedActive=active_unattributed,
            cacheUnattributedUsedRetired=retired_unattributed,
            cacheUnattributedUsedAll=all_unattributed,
            tail=active_tails,
            tailActive=active_tails,
            tailRetired=retired_tails,
            tailAll=all_tails,
            retainedPageCapacity=all_retained,
            retainedPageCapacityActive=active_retained,
            retainedPageCapacityRetired=retired_retained,
            retainedPageCapacityAll=all_retained,
            gpuSafeReclaimableBytes=None,
            physicalBackingBytes=None,
            interpretation=(
                "accounted-cache-ranges"
                if sample["complete"]
                else "incomplete-do-not-infer-occupancy"
            ),
        )
        summary.update(_scope_dimensions(all_agg, active_agg, schema == 2))
        summaries.append(summary)
    covered = bool(samples) and all(sample["complete"] for sample in summaries)
    return {
        "schema": schema,
        "samples": summaries,
        "covered": covered,
        "tagDimensionsAvailable": schema == 2,
        "shadowRecoveryProven": False,
        "physicalBackingComplete": False,
    }


def validate(data):
    if not isinstance(data, dict):
        raise ValueError("census root")
    schema = data.get("schema")
    if type(schema) is not int:
        raise ValueError("census schema")
    if schema == 1:
        return _validate(data, 1)
    if schema == 2:
        return _validate(data, 2)
    raise ValueError("census schema")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="JSON report or isolated stage11BudgetCensus block")
    args = parser.parse_args()
    data = json.loads(
        args.input.read_text(encoding="utf-8"),
        object_pairs_hook=unique_pairs,
        parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)),
    )
    print(json.dumps(validate(data.get("stage11BudgetCensus", data)), indent=2))


if __name__ == "__main__":
    main()
