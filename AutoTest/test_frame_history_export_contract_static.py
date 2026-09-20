"""Production wiring static checks for the frozen export terminal contract.

SCOPE: STATIC_TEXT_CONTRACT_ONLY. These checks read production source text
only; they are not runtime, CPU-commit, GPU or game evidence and they do not
invoke the compiler. They cannot substitute the real commit-boundary test
(AutoTest/test_frame_history_export_commit.cpp, owned by the other worker).

Scoping rules (revised after main-thread review):
- Function bodies are extracted by signature regex + literal/comment-aware
  brace matching (length-preserving mask), never by whole-file string
  counting.
- WIRING assertions run on comment- AND literal-masked bodies: a function or
  entry name inside a comment or string literal is not a call and never
  satisfies them.
- PRESERVATION assertions on code tokens run on the same masked bodies. The
  one exception is the wire schema field check (P1), whose payload IS literal
  text: it runs with comments masked but string literals kept, so a
  comment-only copy cannot satisfy it either.
- The packageReady non-touch check covers status, nextExport, the unique
  commit entry settleExportProgressLocked AND the shared production header.

Families:
- ExportWiringTargetState: the frozen target state (unique settle entry with
  real calls from both status and nextExport; previous duplicated terminal
  branches gone). They fail while the wiring is absent or violated.
- ExportContractPreserved: schema fields, wire state numbers and key
  ownership boundaries that must hold before AND after the change.
- StaticCheckSelfTests: limited negative self-tests running the REAL
  mask/extract helpers on synthetic text, proving comment/string pseudo-calls
  and a missing definition body cannot satisfy the wiring checks.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "src/d3d9/war3/tools/war3_frame_history.cpp"
CORE = ROOT / "src/d3d9/war3/tools/war3_frame_history_core.h"
HEADER = ROOT / "src/d3d9/war3/tools/war3_frame_history_export_core.h"
SCOPE = "STATIC_TEXT_CONTRACT_ONLY_NOT_RUNTIME_COMMIT_GPU_GAME_EVIDENCE"

STATUS_SIG = r"json\s+status\s*\(\s*\)\s*\{"
NEXT_EXPORT_SIG = r"FrameHistory::Export\s+FrameHistory::nextExport\s*\(\s*\)\s*\{"
FAIL_SIG = r"void\s+fail\s*\(\s*const\s+char\s*\*\s*reason\s*\)\s*\{"
SETTLE_SIG = r"\bsettleExportProgressLocked\s*\(\s*\)\s*\{"


def mask(text, blank_literals=True):
    """Length-preserving mask.

    Comments are always blanked: they are never code. String/char literal
    contents are blanked only when blank_literals is True; checks whose
    contract is literal text (wire schema field names) run with
    blank_literals=False so comments still cannot satisfy them.
    """
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
        elif ch == "/" and nxt == "*":
            out[i] = out[i + 1] = " "
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            if i + 1 < n:
                out[i] = out[i + 1] = " "
                i += 2
        elif (ch == '"' or ch == "'") and blank_literals:
            quote = ch
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\" and i + 1 < n:
                    out[i] = out[i + 1] = " "
                    i += 2
                else:
                    if text[i] != "\n":
                        out[i] = " "
                    i += 1
            if i < n:
                i += 1
        else:
            i += 1
    return "".join(out)


def extract_span(masked_text, signature_regex):
    """Return (start, end) covering signature plus balanced body, or None."""
    match = re.search(signature_regex, masked_text)
    if not match:
        return None
    opening = masked_text.find("{", match.end() - 1)
    if opening < 0:
        return None
    depth = 0
    for i in range(opening, len(masked_text)):
        if masked_text[i] == "{":
            depth += 1
        elif masked_text[i] == "}":
            depth -= 1
            if depth == 0:
                return (match.start(), i + 1)
    return (match.start(), None)


def has_call(masked_body):
    """True only when a REAL call appears outside comments and literals."""
    return masked_body is not None and bool(
        re.search(r"settleExportProgressLocked\s*\(", masked_body)
    )


class ExportContractBase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cpp_text = CPP.read_text(encoding="utf-8")
        cls.cpp_masked = mask(cls.cpp_text)
        cls.cpp_comments_only = mask(cls.cpp_text, blank_literals=False)
        cls.core_text = CORE.read_text(encoding="utf-8")
        cls.header_text = HEADER.read_text(encoding="utf-8")
        cls.header_masked = mask(cls.header_text)
        cls.status_span = extract_span(cls.cpp_masked, STATUS_SIG)
        cls.next_export_span = extract_span(cls.cpp_masked, NEXT_EXPORT_SIG)
        cls.fail_span = extract_span(cls.cpp_masked, FAIL_SIG)
        cls.settle_span = extract_span(cls.cpp_masked, SETTLE_SIG)

    @classmethod
    def body(cls, span):
        if span is None or span[1] is None:
            return None
        return cls.cpp_text[span[0] : span[1]]

    @classmethod
    def masked_body(cls, span):
        if span is None or span[1] is None:
            return None
        return cls.cpp_masked[span[0] : span[1]]

    @classmethod
    def comments_only_body(cls, span):
        if span is None or span[1] is None:
            return None
        return cls.cpp_comments_only[span[0] : span[1]]


class ExportWiringTargetState(ExportContractBase):
    """Frozen target state; a failure means absent or violated wiring."""

    def test_status_body_calls_unique_settle_entry(self):
        masked = self.masked_body(self.status_span)
        self.assertIsNotNone(
            masked, "Impl::status body not found; production shape changed")
        self.assertTrue(
            has_call(masked),
            "status must call the unique entry in real code; a name inside "
            "a comment or string literal does not count")

    def test_next_export_body_calls_unique_settle_entry(self):
        masked = self.masked_body(self.next_export_span)
        self.assertIsNotNone(
            masked, "FrameHistory::nextExport body not found; shape changed")
        self.assertTrue(
            has_call(masked),
            "nextExport must call the unique entry in real code; a name "
            "inside a comment or string literal does not count")

    def test_status_body_old_duplicate_terminal_branch_removed(self):
        masked = self.masked_body(self.status_span)
        self.assertIsNotNone(masked, "Impl::status body not found")
        self.assertNotRegex(
            masked, r"state\s*=\s*failed\s*\?\s*Fault\s*:\s*Complete",
            "the old status-side terminal write must be gone from real code")
        self.assertNotRegex(
            masked, r"done\s*==\s*orderCount\s*&&\s*queued\s*==\s*orderCount",
            "the old status-side terminal condition must be gone from real code")

    def test_next_export_body_old_inline_terminal_removed(self):
        masked = self.masked_body(self.next_export_span)
        self.assertIsNotNone(masked, "FrameHistory::nextExport body not found")
        self.assertNotRegex(
            masked, r"m\s*->\s*fail\s*\(",
            "the old nextExport-side fault submission must move behind the "
            "unique entry")
        self.assertNotRegex(
            masked, r"m\s*->\s*state\s*=\s*Impl\s*::\s*Complete",
            "nextExport must not write the Complete state directly")
        self.assertNotRegex(
            masked, r"hud\s*\(\s*\)\s*\.\s*state\s*=\s*Impl\s*::\s*Complete",
            "nextExport must not publish the HUD Complete state directly")

    def test_settle_entry_definition_and_real_calls_in_caller_bodies(self):
        # Body-scoped definition check: extract the actual
        # settleExportProgressLocked function body instead of counting the
        # identifier across the whole file. Both callers must hold REAL calls
        # inside their own masked bodies.
        self.assertIsNotNone(
            self.settle_span,
            "settleExportProgressLocked definition signature not found")
        self.assertIsNotNone(
            self.settle_span[1] if self.settle_span else None,
            "settleExportProgressLocked definition body is unbalanced")
        self.assertTrue(
            has_call(self.masked_body(self.status_span)),
            "status caller body must contain a real call")
        self.assertTrue(
            has_call(self.masked_body(self.next_export_span)),
            "nextExport caller body must contain a real call")


class ExportContractPreserved(ExportContractBase):
    """Boundaries that must hold before and after the wiring change."""

    def test_status_body_wire_schema_fields_preserved(self):
        # The wire schema field names ARE literal text, so this check runs
        # with comments masked but string literals kept.
        body = self.comments_only_body(self.status_span)
        self.assertIsNotNone(body, "Impl::status body not found")
        self.assertRegex(body, r'\{\s*"schema"\s*,\s*2\s*\}')
        self.assertRegex(
            body, r'\{\s*"state"\s*,\s*uint32_t\s*\(\s*state\.load\s*\(\s*\)\s*\)\s*\}')
        self.assertRegex(body, r'\{\s*"captureComplete"\s*,\s*false\s*\}')
        self.assertRegex(body, r'\{\s*"rootCauseReady"\s*,\s*false\s*\}')

    def test_capture_lifecycle_state_numbers_preserved(self):
        # Explicit values preserve the manifest/HUD/external analyzer contract.
        self.assertIn(
            "Idle=0, PendingArm=1, Armed=2, Triggered=3, Frozen=4,", self.core_text)
        self.assertIn(
            "Exporting=5, Complete=6, Fault=7, Discard=8", self.core_text)

    def test_next_export_body_ownership_gates_preserved(self):
        masked = self.masked_body(self.next_export_span)
        self.assertIsNotNone(masked, "FrameHistory::nextExport body not found")
        self.assertRegex(masked, r"std\s*::\s*try_to_lock")
        self.assertRegex(
            masked, r"if\s*\(\s*!\s*l\s*\.\s*owns_lock\s*\(\s*\)\s*\)\s*return\s*\{\s*\}\s*;")
        self.assertRegex(masked, r"m\s*->\s*cancelled\s*\.\s*load\s*\(")
        self.assertRegex(
            masked, r"m\s*->\s*state\s*!=\s*Impl\s*::\s*Exporting",
            "submission stays gated to Exporting (no Fault/Discard revival)")

    def test_fail_body_fault_ownership_preserved(self):
        masked = self.masked_body(self.fail_span)
        self.assertIsNotNone(
            masked, "Impl::fail body not found; production shape changed")
        collapsed = re.sub(r"\s+", "", masked)
        self.assertIn("state=Fault;fault=reason;recording=false;", collapsed)

    def test_export_terminal_bodies_do_not_touch_package_ready(self):
        # Scope per main-thread review: status, nextExport, the unique commit
        # entry AND the shared production header. Comments are masked first,
        # so mentioning the word in a comment is not touching it. Still
        # static text only, never runtime evidence.
        for name, text in (
            ("Impl::status", self.masked_body(self.status_span)),
            ("FrameHistory::nextExport",
             self.masked_body(self.next_export_span)),
            ("Impl::settleExportProgressLocked",
             self.masked_body(self.settle_span)),
            ("shared production header", self.header_masked),
        ):
            self.assertIsNotNone(text, "%s not found" % name)
            self.assertNotIn("packageReady", text, name)


class StaticCheckSelfTests(unittest.TestCase):
    """Limited negative self-tests for the scoping helpers themselves.

    They run the REAL mask/extract/has_call helpers on synthetic text only
    (no production files) and prove that comment/string pseudo-calls and a
    missing definition body cannot satisfy the wiring checks.
    """

    def test_comment_only_call_does_not_satisfy_wiring_check(self):
        text = "json status(){\n  // settleExportProgressLocked();\n}"
        self.assertIn("settleExportProgressLocked", text)
        self.assertFalse(
            has_call(mask(text)),
            "a comment-only pseudo-call must not count as a real call")

    def test_string_literal_call_does_not_satisfy_wiring_check(self):
        text = (
            'json status(){const char* n = "settleExportProgressLocked();";}'
        )
        self.assertIn("settleExportProgressLocked", text)
        self.assertFalse(
            has_call(mask(text)),
            "a string-literal pseudo-call must not count as a real call")

    def test_missing_definition_body_fails_definition_check(self):
        callers = (
            "json status(){settleExportProgressLocked();}"
            "FrameHistory::Export FrameHistory::nextExport()"
            "{m->settleExportProgressLocked();}"
        )
        masked = mask(callers)
        self.assertTrue(has_call(masked))
        # Both callers hold real calls, but no definition signature exists:
        # the definition check must come up empty, so the wiring test fails.
        self.assertIsNone(
            extract_span(masked, SETTLE_SIG),
            "a missing settleExportProgressLocked definition body must not "
            "pass the definition check")


if __name__ == "__main__":
    unittest.main()