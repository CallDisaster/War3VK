import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class WarVkReleaseVersionStaticTests(unittest.TestCase):
    def read(self, relative_path: str) -> str:
        return (ROOT / relative_path).read_text(encoding="utf-8")

    def test_product_version_sources_are_1_2_0_release(self):
        self.assertIn("version : '1.2.0'", self.read("meson.build"))
        self.assertEqual(self.read("RELEASE").strip(), "1.2.0")
        self.assertIn(
            '#define DXVK_VERSION "1.2.0 Release"', self.read("version.h")
        )

        resource = self.read("src/d3d9/version.rc")
        self.assertIn("FILEVERSION        1,2,0,0", resource)
        self.assertIn('VALUE "ProductVersion",   "1.2.0 Release"', resource)

    def test_public_api_versions_are_1_2_0(self):
        shader_api = self.read("src/d3d9/war3_shader_api.h")
        values = dict(
            re.findall(r"API_VERSION_(MAJOR|MINOR|PATCH)\s*=\s*(\d+)", shader_api)
        )
        self.assertEqual(values, {"MAJOR": "1", "MINOR": "2", "PATCH": "0"})

        japi = self.read("src/d3d9/war3/japi/war3_japi_v1.cpp")
        self.assertIn('kApiVersion = "WarVK JAPI 1.2.0 Release"', japi)
        self.assertIn('kCanonicalVersion = "v1"', japi)

    def test_public_docs_name_the_release_and_vulkan_requirement(self):
        readme = self.read("README.md")
        readme_cn = self.read("README_CN.md")
        changelog = self.read("CHANGELOG.md")
        self.assertIn("# WarVK 1.2.0 Release", readme)
        self.assertIn("# WarVK 1.2.0 Release", readme_cn)
        self.assertIn("## v1.2.0 Release", changelog)
        self.assertIn("Vulkan 1.3", readme)
        self.assertIn("Vulkan 1.3", readme_cn)


if __name__ == "__main__":
    unittest.main()
