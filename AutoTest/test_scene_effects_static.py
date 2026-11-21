#!/usr/bin/env python3
"""Source rules of the shared effect executors (x64 migration phases 4a, 4b).
The shadow pass (phase 4c) has its own rules in test_shadow_scene_static.py.

An effect that runs at an insertion point has one implementation under
src/d3d9/war3/effects.  The in-process device reaches it through a thin
adapter pass; the x64 render host compiles the same sources and feeds them
from the frame's scene packet.  These rules keep both ends on one
implementation and one insertion order.  The pixel comparison of the two
modes is run_x64_scene_effects_gate.py.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EFFECTS = ROOT / "src/d3d9/war3/effects"
D3D9 = ROOT / "src/d3d9"
HOST = ROOT / "tools/render_host"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def code(text: str) -> str:
    """The text without comments (rules are about code, not prose)."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//.*", "", text)


def includes(text: str) -> list[str]:
    return re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', text, re.M)


class SceneEffectTemplate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scene = read(EFFECTS / "war3_effect_scene.h")
        cls.frame = read(EFFECTS / "war3_effect_frame.h")
        cls.executors = {
            path.name: read(path)
            for path in sorted(EFFECTS.glob("war3_*_executor.*"))
        }
        cls.device = read(D3D9 / "d3d9_device.cpp")
        cls.wire = read(HOST / "d3d9_annotation_wire.h")
        cls.host_main = read(HOST / "dxvk_core/d3d9_host_main.cpp")
        cls.backend = read(HOST / "dxvk_core/d3d9_backend.cpp")
        cls.before_ui = read(HOST / "dxvk_core/before_ui_effects.h")

    # --- one implementation, usable by both ends ---------------------------

    def test_scene_header_has_no_dependency(self):
        for name in includes(self.scene):
            self.assertRegex(name, r"^c?[a-z_]+$", name)

    def test_executors_depend_only_on_dxvk_and_execution_helpers(self):
        self.assertEqual(
            {"war3_aa_executor.cpp", "war3_aa_executor.h",
             "war3_bloom_executor.cpp", "war3_bloom_executor.h",
             "war3_snow_executor.cpp", "war3_snow_executor.h",
             "war3_ssao_executor.cpp", "war3_ssao_executor.h"},
            set(self.executors))
        allowed = re.compile(
            r"^(war3_[a-z_]+\.h"                       # this directory, shader headers
            r"|\.\./render/war3_(owned_image_layout|tracked_vk_pipeline|"
            r"weather_resource_retry)\.h"
            r"|\.\./\.\./smaa_(area|search)_tex\.inl"     # SMAA lookup data
            r"|\.\./\.\./\.\./(dxvk|util)/[a-z_]+\.h"
            r"|[a-z_]+)$")                             # standard library
        for name, text in {**self.executors,
                           "war3_effect_frame.h": self.frame}.items():
            for header in includes(text):
                self.assertRegex(header, allowed, f"{name} includes {header}")
            # No device, pipeline, settings, game or diagnostics types.
            for forbidden in ("D3D9", "War3PipelineInput", "War3RenderSettings",
                              "War3PerfMonitor", "WeatherRuntime", "Game.dll",
                              "GetModuleHandle", "getenv", "env::"):
                self.assertNotIn(forbidden, code(text), f"{name} uses {forbidden}")

    def test_executor_allocates_before_the_first_command(self):
        for name in ("war3_aa_executor.cpp", "war3_bloom_executor.cpp",
                     "war3_snow_executor.cpp", "war3_ssao_executor.cpp"):
            text = self.executors[name]
            run = text[text.rindex("Executor::run("):]
            first_command = run.index("ctx->cmd")
            self.assertRegex(run[:first_command], r"ensurePipelines?\(", name)
            # GPU timing opens with the first command, not before a refusal.
            self.assertNotIn("beginRecording(", run[first_command:], name)
            self.assertNotIn("catch (", run[first_command:], name)
            self.assertNotIn("createImage(", run[first_command:], name)
            # Layouts are read from and restored to the image, never assumed.
            self.assertIn("->queryLayout(", run[:first_command], name)
            self.assertIn("LayoutKnown(", run[:first_command], name)
            self.assertIn("trackLayout(colorRange, colorOriginal)",
                          run[first_command:], name)

    def test_in_process_passes_are_adapters_without_commands(self):
        for name, executor in (("d3d9_war3_ssao.cpp", "SsaoExecutor"),
                               ("d3d9_war3_weather.cpp", "SnowExecutor"),
                               ("d3d9_war3_aa.cpp", "AaExecutor"),
                               ("d3d9_war3_bloom.cpp", "BloomExecutor")):
            text = read(D3D9 / name)
            header = read(D3D9 / name.replace(".cpp", ".h"))
            self.assertIn(f"war3::effects::{executor} m_executor", header)
            self.assertEqual(1, text.count("m_executor.run("), name)
            if executor != "BloomExecutor":  # bloom is not a pipeline pass
                self.assertIn("War3EffectFrame(input)", text, name)
            for command in ("ctx->cmd", "createImage(", "createSampler(",
                            "createBuiltIn", "trackLayout(", "VkImageMemoryBarrier"):
                self.assertNotIn(command, text, f"{name} records {command}")

    # --- scene packet --------------------------------------------------------

    def test_wire_uses_the_shared_record_layouts(self):
        # The shadow records' header brings the effect records with it.
        self.assertIn('#include "../../src/d3d9/war3/effects/war3_shadow_scene.h"',
                      self.wire)
        self.assertIn('#include "war3_effect_scene.h"',
                      read(EFFECTS / "war3_shadow_scene.h"))
        self.assertIn("Ssao=5, WeatherSnow=6, Aa=7, Bloom=8", self.wire)
        self.assertIn("uint32_t(AnnotationSchema::Ssao)==scene::SsaoSchema", self.wire)
        self.assertIn("uint32_t(AnnotationSchema::WeatherSnow)==scene::SnowSchema",
                      self.wire)
        self.assertIn("uint32_t(AnnotationSchema::Aa)==scene::AaSchema", self.wire)
        self.assertIn("uint32_t(AnnotationSchema::Bloom)==scene::BloomSchema", self.wire)
        self.assertIn("FrameCameraBytes==scene::CameraBytes", self.wire)
        self.assertIn("return scene::ValidEffectRecord(schema,p,bytes);", self.wire)
        # One stage enumeration and one scene tracker for both ends.
        self.assertIn("using Stage=scene::Stage;", self.wire)
        self.assertIn("using SceneTracker=scene::SceneTracker;", self.wire)
        self.assertNotIn("enum class Stage", self.wire)
        self.assertNotIn("struct SceneTracker", self.wire)

    def test_records_are_values_with_fixed_sizes(self):
        for constant in ("CameraSchema = 2", "SsaoSchema = 5", "SnowSchema = 6",
                         "AaSchema = 7", "BloomSchema = 8",
                         "SsaoBytes = 6 * 4", "SnowBytes = 11 * 4 + 3 * 8 + 8",
                         "AaBytes = 7 * 4", "BloomBytes = 6 * 4"):
            self.assertIn(constant, self.scene)
        records = self.scene[self.scene.index("struct Camera"):
                             self.scene.index("namespace detail")]
        self.assertNotIn("*", records.replace("* 4", ""))  # no pointers
        self.assertIn("s.id == 0) return false", self.scene)

    # --- insertion point -----------------------------------------------------

    def test_host_inserts_once_after_a_completed_world_scene(self):
        main = self.host_main
        marker = main[main.index("case d9::Op::StageMarker:"):]
        marker = marker[:marker.index("annotationRecorder.stage(")]
        insert = marker.index("h.args[0]==uint32_t(d9::Stage::BeforeUi)"
                              "&&(scenePacket.effects()||shadowPacket.work())")
        refuse = marker.index("if(insert&&!sceneTracker.canInsertBeforeUi())")
        mark = marker.index("sceneTracker.mark(")
        run = marker.index("gpu->runBeforeUi(scenePacket")
        done = marker.index("sceneTracker.insertedBeforeUi()")
        self.assertLess(insert, refuse)
        self.assertLess(refuse, mark)
        self.assertLess(mark, run)
        self.assertLess(run, done)
        self.assertIn("return D3DERR_INVALIDCALL;",
                      marker[refuse:mark])
        self.assertIn("if(h.args[0]==uint32_t(d9::Stage::FrameEnd))clearScene();",
                      marker)

    def test_scene_packet_never_outlives_its_frame(self):
        main = self.host_main
        # One place ends the packet, and the shadow records and casters with it.
        self.assertIn("void clearScene() {scenePacket.clear();shadowPacket.clear();"
                      "gpu->endShadowFrame();}", main)
        self.assertEqual(1, main.count("scenePacket.clear()"))
        # Itself and four callers: FrameEnd, SessionReset, Present, and the
        # first soft refusal of a scene record (the half frame is abandoned
        # there, so that a WorldSceneExit ignored later cannot keep the
        # tracker active and refuse every SessionReset after it).
        self.assertEqual(5, main.count("clearScene()"))
        self.assertIn("session->clearScene();session->sceneTracker.active=false;"
                      "session->sceneTracker.frameEnd();", main)
        self.assertRegex(main, r"gpu->present\(\);sceneTracker\.frameEnd\(\);"
                               r"clearScene\(\);")
        self.assertRegex(main, r"annotationRecorder\.reset\([^;]*\);clearScene\(\);")
        scene_case = main[main.index("case d9::Op::FrameScene:"):
                          main.index("case d9::Op::StageMarker:")]
        self.assertLess(scene_case.index("d9::ValidFrameScene("),
                        scene_case.index("scenePacket.accept("))

    def test_host_records_effects_into_the_frame_command_stream(self):
        backend = self.backend
        run = backend[backend.index("D3D9Backend::BeforeUiOutcome D3D9Backend::runBeforeUi("):]
        run = run[:run.index("\n}\n")]
        self.assertIn("requireProfile(", run)
        self.assertIn("owner.context->beginExternalRendering()", run)
        self.assertIn("owner.beforeUi->run(list, owner.colorView, owner.depthView, packet",
                      run)
        self.assertIn("owner.targetGeneration", run)
        # Executors die with the device that created their objects.
        shutdown = backend.index("beforeUi.reset();")
        self.assertLess(shutdown, backend.index("context = nullptr", shutdown))

    def test_effect_order_is_the_same_everywhere(self):
        # In process: registration order of the passes.
        device = self.device
        self.assertLess(device.index('"SSAO", std::move(ssaoPass)'),
                        device.index('"Weather", std::move(weatherPass)'))
        self.assertLess(device.index('"Weather", std::move(weatherPass)'),
                        device.index('"AA", std::move(aaPass)'))
        # Post-processing follows the pipeline's BeforeUi passes.
        insert = device[device.index("void D3D9DeviceEx::War3MaybeInsertBeforeUi("):]
        self.assertLess(insert.index("EmitCs("),
                        insert.index("cBloom->Run(ctx->beginExternalRendering()"))
        # Fixture entry: the same passes, in that order.
        entry = device[device.index("HRESULT D3D9DeviceEx::War3RunSceneEffectsForFixture("):]
        entry = entry[:entry.index("\n}\n")]
        order = [entry.index(call) for call in (
            "cSsao->Run(cmd, cInput);", "cWeather->Run(cmd, cInput);",
            "cAa->Run(cmd, cInput);", "cBloom->Run(cmd, cInput.colorView, cBloomRecord);")]
        self.assertEqual(order, sorted(order))
        self.assertIn("ctx->beginExternalRendering()", entry)
        # Host.
        order = [self.before_ui.index(call) for call in (
            "m_ssao.run(", "m_snow.run(", "m_aa.run(", "m_bloom.run(")]
        self.assertEqual(order, sorted(order))
        self.assertIn("fx::CameraFromScene(", self.before_ui)

    def test_d3d9_post_processing_is_only_the_fixture_oracle(self):
        # The device runs the Vulkan bloom executor; the D3D9-API
        # implementation (d3dcompiler at run time) is reachable only from the
        # fixture's legacy switch, which only the gate sets.
        device = code(self.device)
        header = code(read(D3D9 / "d3d9_device.h"))
        for name in ("War3PostProcess", "war3_post_process.h", "ApplyFromSurface"):
            self.assertNotIn(name, device)
            self.assertNotIn(name, header)
        self.assertIn("std::unique_ptr<War3BloomPass> m_war3BloomPass;", header)
        fixture = read(D3D9 / "d3d9_war3_scene_effects_fixture.cpp")
        self.assertIn('env::getEnvVar("DXVK_WAR3_SCENE_EFFECTS_FIXTURE_LEGACY_BLOOM") == "1"',
                      fixture)
        mark = fixture[fixture.index("D3D9War3SceneEffectsFixture::MarkStage("):]
        self.assertIn("LegacyBloom() && m_packet.hasBloom", mark)
        self.assertIn("War3RunSceneEffectsForFixture(m_packet, m_frames + 1, !legacy,", mark)
        users = [path.name for path in D3D9.rglob("*.cpp")
                 if "War3PostProcess" in code(read(path))]
        self.assertEqual(sorted(users), ["d3d9_war3_scene_effects_fixture.cpp",
                                         "war3_post_process.cpp"])

    # --- two-mode comparison entry -------------------------------------------

    def test_fixture_entry_exists_only_behind_its_switch(self):
        device = self.device
        branch = device[device.index("if (riid == IID_WarVKSceneEffectsFixture"):]
        branch = branch[:branch.index("return S_OK;")]
        self.assertIn("D3D9War3SceneEffectsFixture::Enabled()", branch)
        fixture = read(D3D9 / "d3d9_war3_scene_effects_fixture.cpp")
        self.assertIn('env::getEnvVar("DXVK_WAR3_SCENE_EFFECTS_FIXTURE") == "1"',
                      fixture)
        # The fixture entry applies the host's insertion rule to the same types.
        self.assertIn("m_tracker.canInsertBeforeUi()", fixture)
        self.assertIn("m_packet.effects()", fixture)
        self.assertIn("m_tracker.insertedBeforeUi()", fixture)
        self.assertIn("War3RunSceneEffectsForFixture(m_packet", fixture)
        # No new export: the entry is reachable only through QueryInterface.
        exports = read(D3D9 / "d3d9.def")
        self.assertNotIn("SceneEffects", exports)
        self.assertNotIn("Fixture", exports)
        self.assertEqual(1, device.count("IID_WarVKSceneEffectsFixture"))

    # --- build lists and gates -----------------------------------------------

    def test_both_builds_compile_the_same_executor_sources(self):
        x86 = read(D3D9 / "meson.build")
        host = read(HOST / "dxvk_core/meson.build")
        for source in ("war3_snow_executor.cpp", "war3_ssao_executor.cpp",
                       "war3_aa_executor.cpp", "war3_bloom_executor.cpp"):
            self.assertIn(f"'war3/effects/{source}'", x86)
            self.assertIn(f"'../../../src/d3d9/war3/effects/{source}'", host)
        self.assertIn("'d3d9_war3_bloom.cpp'", x86)
        self.assertIn("'d3d9_war3_scene_effects_fixture.cpp'", x86)
        self.assertNotIn("scene_effects_fixture", host)
        # The host compiles the executor subset with its own generator: the
        # same glslang arguments plus the host's layer-receiver define.
        self.assertIn("host_effect_glsl = generator(glsl_compiler,", host)
        self.assertIn("arguments: glsl_args + ['-DWARVK_HOST_LAYER_RECEIVER=1']", host)
        self.assertIn("host_effect_glsl.process(war3fx_executor_shaders)", host)
        shaders = read(ROOT / "subprojects/war3fx/meson.build")
        subset = shaders[shaders.index("war3fx_executor_shaders = files(["):]
        subset = subset[:subset.index("])")]
        # Comments in the list may contain apostrophes ("the water's").
        subset = "\n".join(line.split("#", 1)[0] for line in subset.splitlines())
        names = re.findall(r"'([^']+)'", subset)
        self.assertIn("shaders/war3_water_surface.frag", names)
        for shader in names:
            self.assertEqual(2, shaders.count(f"'{shader}'"),
                             f"{shader} must also be in war3fx_shaders")

    def test_gates_know_the_effects_directory(self):
        cut = read(ROOT / "AutoTest/run_x86_data_layer_cut_gate.py")
        self.assertIn("if sub == 'effects':", cut)
        core = read(ROOT / "AutoTest/run_render_host_core_gate.py")
        self.assertEqual(2, core.count("(ROOT/'src/d3d9/war3/effects').glob('*.cpp')"))
        self.assertIn("'src/d3d9/war3/render/war3_tracked_vk_pipeline.cpp'", core)


if __name__ == "__main__":
    unittest.main()
