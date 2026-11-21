"""Pure CPU checks for the actual pinned upstream ownership transformation.

No build, device, graphics context or game is created. These tests exercise the
real transform and output guards; they are not COM/GPU lifetime acceptance.
"""

import hashlib
from pathlib import Path
import tempfile
import unittest

import prepare_warvk_imgui_dx9 as adapter


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "subprojects/imgui/backends/imgui_impl_dx9.cpp"
HEADER = ROOT / "src/d3d9/war3/ui/war3_imgui_dx9.h"
OWNER = ROOT / "src/d3d9/war3/ui/war3_imgui_dx9_ownership.h"


class ImGuiAdapterTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.original = SOURCE.read_bytes()
        cls.source = cls.original.decode("utf-8").replace("\r\n", "\n")
        cls.generated = adapter.transform(cls.original).decode("utf-8")

    def test_exact_upstream_and_line_ending_variants(self):
        self.assertIn(hashlib.sha256(self.original).hexdigest().upper(), adapter.UPSTREAM_SHA256)
        lf = self.source.encode("utf-8")
        crlf = self.source.replace("\n", "\r\n").encode("utf-8")
        self.assertEqual(adapter.transform(lf), adapter.transform(crlf))

    def test_unknown_source_rejected(self):
        for mutation in (
            self.original + b"\n", self.original.replace(b"CreateVertexBuffer", b"CreateVertexBuffers", 1),
            self.original.replace(b"SetTexture", b"SetTextures", 1), b"", b"\xef\xbb\xbf" + self.original,
        ):
            with self.subTest(size=len(mutation)):
                with self.assertRaises(ValueError):
                    adapter.transform(mutation)

    def test_each_missing_site_rejected_independently(self):
        for label, old, _new, _expected in adapter.EDITS:
            with self.subTest(site=label):
                with self.assertRaisesRegex(ValueError, label):
                    adapter.apply_lifetime_edits(self.source.replace(old, "/* removed */", 1))

    def test_each_duplicated_site_rejected_independently(self):
        for label, old, _new, _expected in adapter.EDITS:
            with self.subTest(site=label):
                with self.assertRaisesRegex(ValueError, label):
                    adapter.apply_lifetime_edits(self.source + "\n" + old)

    def test_each_already_adapted_site_rejected(self):
        for label, _old, new, _expected in adapter.EDITS:
            with self.subTest(site=label):
                with self.assertRaisesRegex(ValueError, label):
                    adapter.apply_lifetime_edits(self.source + "\n" + new)

    def test_all_non_lifetime_bytes_preserved(self):
        # Reverse only declared substitutions: all remaining upstream code and
        # comments must be byte-identical after newline canonicalization.
        adapted = adapter.apply_lifetime_edits(self.source)
        for _label, old, new, expected in reversed(adapter.EDITS):
            self.assertEqual(adapted.count(new), expected)
            adapted = adapted.replace(new, old)
        self.assertEqual(adapted, self.source)
        self.assertIn(adapter.apply_lifetime_edits(self.source), self.generated)
        self.assertEqual(sum(edit[3] for edit in adapter.EDITS), 10)

    def test_public_temporary_refs_and_user_textures_unchanged(self):
        for expression in (
            "state_block->Release();", "pd3d->Release();",
            "device->CreateStateBlock(D3DSBT_ALL, &state_block)",
            "device->SetTexture(0, texture);",
        ):
            self.assertGreater(self.source.count(expression), 0, expression)
            self.assertEqual(self.generated.count(expression), self.source.count(expression))
        self.assertNotIn("bd->pd3dDevice->AddRef();", self.generated)
        self.assertNotIn("bd->pd3dDevice->Release();", self.generated)
        self.assertNotIn("bd->pVB->Release();", self.generated)
        self.assertNotIn("bd->pIB->Release();", self.generated)
        self.assertNotIn("backend_tex->Release();", self.generated)

    def test_explicit_prefixed_entry_points(self):
        header = HEADER.read_text(encoding="utf-8")
        upstream_include = self.generated.index('#include "imgui_impl_dx9.h"')
        for name in adapter.ENTRY_POINTS:
            define = f"#define ImGui_ImplDX9_{name} War3ImGui_ImplDX9_{name}\n"
            self.assertEqual(self.generated.count(define), 1)
            self.assertLess(self.generated.index(define), upstream_include)
            self.assertEqual(self.generated.count(f"#undef ImGui_ImplDX9_{name}\n"), 1)
            self.assertIn(f"War3ImGui_ImplDX9_{name}(", header)
        self.assertEqual(len(adapter.ENTRY_POINTS), 7)

    def test_helper_public_to_private_order_and_failure_guard(self):
        owner = OWNER.read_text(encoding="utf-8")
        self.assertIn("if (SUCCEEDED(result) && output && *output)", owner)
        self.assertLess(owner.index("resource->AddRefPrivate();"), owner.index("resource->Release();"))
        self.assertEqual(owner.count("return result;"), 1)
        for concrete in ("D3D9VertexBuffer", "D3D9IndexBuffer", "D3D9Texture2D"):
            self.assertIn(f"RetainCreatedResource<{concrete}>(result, output)", owner)
            self.assertIn(f"static_cast<{concrete}*>(resource)->ReleasePrivate();", owner)

    def test_generated_output_only_and_incremental_repeat(self):
        original_hash = hashlib.sha256(SOURCE.read_bytes()).digest()
        with tempfile.TemporaryDirectory(prefix="warvk-imgui-adapter-") as directory:
            output = Path(directory) / "generated.cpp"
            first = adapter.generate(SOURCE, output)
            saved = output.read_bytes()
            self.assertEqual(adapter.generate(SOURCE, output), first)
            self.assertEqual(output.read_bytes(), saved)
            self.assertEqual(first["lifetimeSites"], 10)
            self.assertEqual(first["prefixedEntryPoints"], 7)
            self.assertEqual(first["outputSha256"], hashlib.sha256(saved).hexdigest().upper())
        self.assertEqual(hashlib.sha256(SOURCE.read_bytes()).digest(), original_hash)

    def test_same_source_and_upstream_directory_forbidden(self):
        before = SOURCE.read_bytes()
        with self.assertRaisesRegex(ValueError, "alias"):
            adapter.generate(SOURCE, SOURCE)
        with self.assertRaisesRegex(ValueError, "backend directory"):
            adapter.generate(SOURCE, SOURCE.parent / "unwanted-generated.cpp")
        self.assertEqual(SOURCE.read_bytes(), before)

    def test_unrelated_existing_output_preserved(self):
        with tempfile.TemporaryDirectory(prefix="warvk-imgui-adapter-") as directory:
            output = Path(directory) / "keep.cpp"
            output.write_bytes(b"unrelated user file\n")
            with self.assertRaisesRegex(ValueError, "non-generated"):
                adapter.generate(SOURCE, output)
            self.assertEqual(output.read_bytes(), b"unrelated user file\n")

    def test_device_output_is_the_pre_proxy_output(self):
        # Proxy mode's overlay added a second variant; the in-process output
        # stays byte for byte what the shipped 1.22.x builds compiled.
        self.assertEqual(hashlib.sha256(self.generated.encode("utf-8")).hexdigest().upper(),
                         "B545855C4509345F75168A4D41BF636DBE5BF25779D8ECB34DDB4D8210F657ED")
        self.assertEqual(adapter.transform(self.original, "device"), self.generated.encode("utf-8"))
        self.assertNotIn("War3ProxyImGui_ImplDX9_", self.generated)
        self.assertNotIn("imgui_proxy", self.generated)

    def test_proxy_variant_same_sites_own_prefix_and_no_abort(self):
        proxy = adapter.transform(self.original, "proxy").decode("utf-8")
        config = adapter.VARIANTS["proxy"]
        self.assertTrue(proxy.startswith(config["banner"]))
        self.assertNotEqual(config["banner"], adapter.GENERATED_BANNER)
        # The same ten lifetime sites, routed to the proxy helpers, plus one.
        lifetime = adapter.lifetime_edits("dxvk::war3::imgui_proxy::")
        self.assertEqual([e[:2] + e[3:] for e in lifetime], [e[:2] + e[3:] for e in adapter.EDITS])
        self.assertEqual(config["edits"], lifetime + adapter.PROXY_EDITS)
        self.assertEqual(sum(edit[3] for edit in config["edits"]), 11)
        for _label, _old, new, expected in config["edits"]:
            self.assertEqual(proxy.count(new), expected)
        self.assertNotIn('IM_ASSERT(hr >= 0 && "Backend failed to create texture!");', proxy)
        self.assertNotIn("imgui_owned", proxy)
        self.assertNotIn("War3ImGui_ImplDX9_", proxy)
        header = (ROOT / "src/d3d9/war3/ui/war3_imgui_dx9_proxy.h").read_text(encoding="utf-8")
        for name in adapter.ENTRY_POINTS:
            self.assertEqual(proxy.count(f"#define ImGui_ImplDX9_{name} War3ProxyImGui_ImplDX9_{name}\n"), 1)
            self.assertIn(f"War3ProxyImGui_ImplDX9_{name}(", header)
        # Each refused site fails the proxy variant independently too.
        for label, old, _new, _expected in config["edits"]:
            with self.subTest(site=label):
                with self.assertRaisesRegex(ValueError, label):
                    adapter.apply_lifetime_edits(self.source.replace(old, "/* removed */", 1), config["edits"])

    def test_proxy_helpers_managed_atlas_and_no_public_reference(self):
        owner = (ROOT / "src/d3d9/war3/ui/war3_imgui_dx9_proxy_ownership.h").read_text(encoding="utf-8")
        texture = owner[owner.index("inline HRESULT CreateTexture("):owner.index("inline void ReleaseVertexBuffer(")]
        self.assertIn("width, height, levels, 0, format, D3DPOOL_MANAGED, output, sharedHandle", texture)
        retain = owner[owner.index("inline HRESULT RetainCreatedResource("):owner.index("inline HRESULT CreateVertexBuffer(")]
        self.assertLess(retain.index("proxy::RetainPrivate(*output)"), retain.index("(*output)->Release();"))
        self.assertIn("*output = nullptr;", retain)
        self.assertEqual(owner.count("proxy::ReleasePrivate(resource);"), 3)
        self.assertNotIn("AddRefPrivate", owner)

    def test_generate_proxy_variant_into_own_output(self):
        with tempfile.TemporaryDirectory(prefix="warvk-imgui-adapter-") as directory:
            device = Path(directory) / "device.cpp"
            proxy = Path(directory) / "proxy.cpp"
            adapter.generate(SOURCE, device)
            result = adapter.generate(SOURCE, proxy, "proxy")
            self.assertEqual(result["lifetimeSites"], 11)
            self.assertEqual(device.read_bytes(), self.generated.encode("utf-8"))
            # Neither variant overwrites the other's output.
            with self.assertRaisesRegex(ValueError, "non-generated"):
                adapter.generate(SOURCE, device, "proxy")
            with self.assertRaisesRegex(ValueError, "non-generated"):
                adapter.generate(SOURCE, proxy)

    def test_bad_input_does_not_touch_prior_generated_output(self):
        with tempfile.TemporaryDirectory(prefix="warvk-imgui-adapter-") as directory:
            root = Path(directory)
            source_dir = root / "source"
            output_dir = root / "build"
            source_dir.mkdir()
            output_dir.mkdir()
            source = source_dir / "upstream.cpp"
            source.write_bytes(self.original + b"changed")
            output = output_dir / "generated.cpp"
            sentinel = adapter.GENERATED_BANNER.encode("utf-8") + b"previous successful generation"
            output.write_bytes(sentinel)
            with self.assertRaisesRegex(ValueError, "unreviewed"):
                adapter.generate(source, output)
            self.assertEqual(output.read_bytes(), sentinel)


if __name__ == "__main__":
    unittest.main()
