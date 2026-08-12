from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(
    encoding="utf-8", errors="replace"
)

start = SOURCE.index("enterBuildEligibleLeasePhase(\"LeaseKeySort\")")
end = SOURCE.index("if (traceBuildEligible)", start)
body = SOURCE[start:end]

assert "static thread_local std::vector<Matrix4> s_leaseLivePaletteScratch" in body
assert "leaseLivePaletteScratch.clear();" in body
assert "const auto installLeaseLivePalette" in body
assert "auto previousPalette = std::move(leased.packet.runtimeGroupPalette)" in body
assert "leased.packet.runtimeGroupPalette = std::move(livePalette)" in body
assert "livePalette = std::move(previousPalette)" in body

# Both producer-fact and CModel refreshes share the same cleared scratch. The
# existing packet is replaced only after a successful, non-empty rebuild.
assert body.count("m_war3ShadowPersistentFrameSerial, leaseLivePaletteScratch") == 2
assert body.count("installLeaseLivePalette(leased, leaseLivePaletteScratch)") == 2
first_install = body.index("installLeaseLivePalette(leased, leaseLivePaletteScratch)")
first_fail = body.index("if (!rebuilt || leaseLivePaletteScratch.empty()")
assert first_fail < first_install
assert "std::vector<Matrix4> liveRuntimeGroupPalette" not in body

print("direct lease palette scratch static checks passed")
