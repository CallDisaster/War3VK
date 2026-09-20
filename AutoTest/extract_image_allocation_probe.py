"""Generate an unchanged production method for a CPU-only fault-injection TU."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8")
start = source.index("  Rc<DxvkResourceAllocation> DxvkMemoryAllocator::createImageResource(")
end = source.index("\n\n", source.index("    return allocation;", start))
body = source[start:end]
assert body.rstrip().endswith("}")
Path(sys.argv[2]).write_text(body + "\n", encoding="utf-8")
