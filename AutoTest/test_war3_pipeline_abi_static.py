#!/usr/bin/env python3
"""Prevent mixed-object War3RenderPipeline allocation ABI regressions."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


PIPELINE_H = read("src/d3d9/d3d9_war3_pipeline.h")
PIPELINE_CPP = read("src/d3d9/d3d9_war3_pipeline.cpp")
DEVICE_CPP = read("src/d3d9/d3d9_device.cpp")
SHADER_API_CPP = read("src/d3d9/war3_shader_api.cpp")


class War3PipelineAbiStaticTests(unittest.TestCase):
    def test_device_never_allocates_or_deletes_pipeline_directly(self) -> None:
        self.assertNotIn("new War3RenderPipeline", DEVICE_CPP)
        self.assertNotIn("delete m_war3Pipeline", DEVICE_CPP)
        self.assertIn("CreateWar3RenderPipeline(", DEVICE_CPP)
        self.assertIn("DestroyWar3RenderPipeline(", DEVICE_CPP)

    def test_factory_owns_allocation_and_destruction(self) -> None:
        self.assertIn("return new War3RenderPipeline(device);", PIPELINE_CPP)
        self.assertIn("delete pipeline;", PIPELINE_CPP)

    def test_factory_symbol_encodes_pipeline_and_settings_layout(self) -> None:
        for token in (
            "War3RenderPipelineAbiTag<",
            "sizeof(War3RenderPipeline)",
            "alignof(War3RenderPipeline)",
            "sizeof(War3RenderSettings)",
            "alignof(War3RenderSettings)",
            "War3RenderPipelineAbi",
        ):
            self.assertIn(token, PIPELINE_H)
        self.assertGreaterEqual(DEVICE_CPP.count("War3RenderPipelineAbi { }"), 2)

    def test_shader_context_symbol_encodes_all_walked_scene_layouts(self) -> None:
        for token in (
            "War3ShaderContextAbiTag<",
            "sizeof(War3PipelineInput)",
            "alignof(War3PipelineInput)",
            "sizeof(War3FrameScene)",
            "alignof(War3FrameScene)",
            "sizeof(War3ShadowCasterDraw)",
            "alignof(War3ShadowCasterDraw)",
            "sizeof(War3ShadowReplayBufferBinding)",
            "alignof(War3ShadowReplayBufferBinding)",
        ):
            self.assertIn(token, PIPELINE_H)

        abi_call = "ValidateWar3ShaderContextAbi("
        context_call = "UpdateRenderContext(input)"
        self.assertIn(abi_call, PIPELINE_CPP)
        self.assertIn(context_call, PIPELINE_CPP)
        self.assertLess(PIPELINE_CPP.index(abi_call), PIPELINE_CPP.index(context_call))
        self.assertIn("::dxvk::War3ShaderContextAbi) noexcept", SHADER_API_CPP)


if __name__ == "__main__":
    unittest.main()
