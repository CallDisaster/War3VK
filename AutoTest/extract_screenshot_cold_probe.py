"""Extract complete, unchanged production functions; not a rewritten model."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8")
def between(start, end):
    return source[source.index(start):source.index(end, source.index(start))]
Path(sys.argv[2]).write_text(
    between("void AsyncScreenshot::reset()", "uint64_t AsyncScreenshot::presentOrdinal()") +
    between("void AsyncScreenshot::prepare(", "std::optional<AsyncScreenshot::Copy> AsyncScreenshot::take("),
    encoding="utf-8")
Path(sys.argv[3]).write_text(between("  void releaseRetiredSlots()", "  bool save("), encoding="utf-8")
