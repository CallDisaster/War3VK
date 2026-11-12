// Host-only proof for the saved Ctrl+F1 panel settings (WarVK\settings.ini):
// value codec, ranges, file text, atomic file I/O and the process store.
// No game, Vulkan device or window is involved; files go to a fresh %TEMP% dir.
#include "../src/d3d9/war3/platform/war3_player_settings.h"
#include "../src/d3d9/d3d9_war3_settings.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

using namespace dxvk;
using namespace dxvk::war3::player;

static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static bool FileExists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static std::wstring MakeTempRoot() {
  wchar_t base[MAX_PATH] = {};
  CHECK(GetTempPathW(MAX_PATH, base) != 0);
  wchar_t name[64] = {};
  std::swprintf(name, 64, L"warvk-player-settings-%lu-%llu", GetCurrentProcessId(),
                static_cast<unsigned long long>(GetTickCount64()));
  std::wstring root = std::wstring(base) + name;
  CHECK(CreateDirectoryW(root.c_str(), nullptr));
  return root;
}

static void TestKeys() {
  const auto keys = KnownKeys();
  CHECK(keys.size() == KnownKeyCount());
  std::set<std::string> unique(keys.begin(), keys.end());
  CHECK(unique.size() == keys.size());
  for (const auto& key : keys)
    CHECK(IsKnownKey(key));
  CHECK(IsKnownKey("General.FpsUnlocked"));
  CHECK(IsKnownKey("Shadows.NativeMode"));
  CHECK(!IsKnownKey("Shadows.DebugMode"));
  CHECK(!IsKnownKey("Future.Thing"));
  CHECK(KnownKeyCount() > 100u);
}

static void TestDecimal() {
  CHECK(FormatDecimal(0.0) == "0");
  CHECK(FormatDecimal(1.0) == "1");
  CHECK(FormatDecimal(-2.0) == "-2");
  CHECK(FormatDecimal(1.25) == "1.25");
  CHECK(FormatDecimal(double(0.004f)) == "0.004");
  CHECK(FormatDecimal(double(1.18f)) == "1.18");
  CHECK(FormatDecimal(1400.0) == "1400");
  CHECK(FormatDecimal(-0.0000001) == "0");
  CHECK(FormatDecimal(NAN) == "0");

  double v = 0.0;
  CHECK(ParseDecimal("1.25", v) && v == 1.25);
  CHECK(ParseDecimal(" +3 ", v) && v == 3.0);
  CHECK(ParseDecimal("-0.5", v) && v == -0.5);
  CHECK(ParseDecimal("2E2", v) && v == 200.0);
  CHECK(ParseDecimal("5.", v) && v == 5.0);
  CHECK(ParseDecimal(".5", v) && v == 0.5);
  CHECK(!ParseDecimal("", v));
  CHECK(!ParseDecimal(".", v));
  CHECK(!ParseDecimal("1.2.3", v));
  CHECK(!ParseDecimal("1e", v));
  CHECK(!ParseDecimal("1e9999", v));
  CHECK(!ParseDecimal("abc", v));
  CHECK(!ParseDecimal("1,5", v));
  CHECK(!ParseDecimal("nan", v));

  // Panel-typical floats survive the text round trip bit-exactly.
  const float samples[] = {0.004f, 0.0833f, 0.166f, 1.18f, 0.75f, 0.005f,
                           1400.0f, 0.22f, 2.1f, 0.12f, -2.0f, 4000.0f, 0.03f};
  for (const float sample : samples) {
    double parsed = 0.0;
    CHECK(ParseDecimal(FormatDecimal(double(sample)), parsed));
    CHECK(float(parsed) == sample);
  }
}

static void TestRecordAndApply() {
  const War3RenderSettings defaults;
  Overrides none;
  CHECK(RecordChanges(defaults, defaults, none) == 0u);
  CHECK(none.empty());

  War3RenderSettings edited = defaults;
  edited.shadows.enabled = false;
  edited.postFx.aa.mode = War3AAMode::FXAA;
  edited.shadows.csm.shadowResolution = 2048u;
  edited.postFx.exposure = 1.25f;
  edited.postFx.volumetricLight.sampleCount = 8;
  edited.postFx.volumetricLight.quality = War3VolumetricQuality::FroxelMedium;
  edited.ambient.color.x = 0.5f;
  edited.dayNight.dayTransitionStartDeg = -3.5f;

  Overrides saved;
  CHECK(RecordChanges(defaults, edited, saved) == 8u);
  CHECK(saved.size() == 8u);
  CHECK(saved["Shadows.Enabled"] == "0");
  CHECK(saved["AA.Mode"] == "1");
  CHECK(saved["Shadows.CsmResolution"] == "2048");
  CHECK(saved["PostFx.Exposure"] == "1.25");
  CHECK(saved["Volumetric.Samples"] == "8");
  CHECK(saved["Volumetric.Quality"] == "1");
  CHECK(saved["Ambient.ColorR"] == "0.5");
  CHECK(saved["DayNight.TransitionStartDeg"] == "-3.5");

  // A second edit of the same field replaces the value, not appends.
  War3RenderSettings edited2 = edited;
  edited2.postFx.exposure = 2.0f;
  CHECK(RecordChanges(edited, edited2, saved) == 1u);
  CHECK(saved["PostFx.Exposure"] == "2");
  CHECK(saved.size() == 8u);

  // Restart: apply onto fresh defaults reproduces exactly the edited state.
  War3RenderSettings restarted;
  Extras extras;
  Overrides copy = saved;
  const ApplyReport report = ApplyOverrides(copy, restarted, extras);
  CHECK(report.applied == 8u && report.unknown == 0u && report.rejected.empty());
  Overrides diff;
  CHECK(RecordChanges(edited2, restarted, diff) == 0u);
  CHECK(!restarted.shadows.enabled);
  CHECK(restarted.postFx.aa.mode == War3AAMode::FXAA);
  CHECK(restarted.shadows.csm.shadowResolution == 2048u);
  CHECK(restarted.postFx.volumetricLight.quality == War3VolumetricQuality::FroxelMedium);
  CHECK(!extras.fpsUnlocked && extras.nativeShadowMode == 0u);
}

static void TestClampRejectUnknown() {
  Overrides input = {
      {"Shadows.Strength", "5"},           // clamp to 1
      {"Shadows.CsmResolution", "3000"},   // nearest power of two: 2048
      {"PointShadow.Resolution", "99999"}, // clamp then snap: 2048
      {"AA.Mode", "9"},                    // clamp to 5
      {"Shadows.CsmFitMode", "2"},         // development mode is not a player value
      {"AA.SmaaSteps", "-4"},              // clamp to 4
      {"Shadows.Enabled", "maybe"},        // rejected
      {"Sun.Intensity", "abc"},            // rejected
      {"Sun.Enabled", "1e999"},            // rejected (not a bool)
      {"Bloom.Intensity", "1e999"},        // rejected (overflow)
      {"Future.Thing", "7"},               // unknown: kept, not applied
      {"General.FpsUnlocked", "true"},
      {"Shadows.NativeMode", "5"},         // clamp to 2
  };
  War3RenderSettings settings;
  Extras extras;
  const ApplyReport report = ApplyOverrides(input, settings, extras);
  CHECK(report.applied == 8u);
  CHECK(report.unknown == 1u);
  CHECK(report.rejected.size() == 4u);
  CHECK(settings.shadows.strength == 1.0f);
  CHECK(settings.shadows.csm.shadowResolution == 2048u);
  CHECK(settings.shadows.pointShadowResolution == 2048u);
  CHECK(settings.postFx.aa.mode == War3AAMode::SMAA_Ultra);
  CHECK(settings.shadows.csm.fitMode == War3CsmFitMode::TightAabb);
  CHECK(settings.postFx.aa.smaaMaxSearchSteps == 4);
  CHECK(settings.shadows.enabled);  // rejected value left the default alone
  CHECK(extras.fpsUnlocked);
  CHECK(extras.nativeShadowMode == 2u);
  CHECK(input.count("Shadows.Enabled") == 0u);  // rejected keys are dropped
  CHECK(input.count("Future.Thing") == 1u);     // unknown keys survive

  Overrides pow2 = {{"Shadows.CsmResolution", "1500"}};
  War3RenderSettings s2;
  ApplyOverrides(pow2, s2, extras);
  CHECK(s2.shadows.csm.shadowResolution == 1024u);
  pow2 = {{"Shadows.CsmResolution", "3072"}};  // tie: prefer the larger
  ApplyOverrides(pow2, s2, extras);
  CHECK(s2.shadows.csm.shadowResolution == 4096u);

  // Every known key accepts plain 0 and 1 (clamped into its range).
  for (const auto& key : KnownKeys()) {
    for (const char* value : {"0", "1"}) {
      Overrides one = {{key, value}};
      War3RenderSettings s;
      Extras e;
      const ApplyReport r = ApplyOverrides(one, s, e);
      CHECK(r.applied == 1u && r.rejected.empty());
    }
  }
}

static void TestDerivedValues() {
  Overrides taa = {{"Shadows.TaaMode", "2"}};
  War3RenderSettings settings;
  Extras extras;
  const uint64_t revision = settings.shadows.shadowTaaSettingsRevision;
  ApplyOverrides(taa, settings, extras);
  CHECK(settings.shadows.shadowTaaMode == War3ShadowTaaMode::Temporal);
  CHECK(settings.shadows.shadowTaaEnabled);
  CHECK(settings.shadows.shadowTaaSettingsRevision == revision + 1u);

  Overrides pcf = {{"PointShadow.PcfNear", "2.5"}};
  War3RenderSettings point;
  ApplyOverrides(pcf, point, extras);
  CHECK(point.shadows.pointShadowPcfRadiusNear == 2.5f);
  CHECK(point.shadows.pointShadowPcfRadiusFar == 2.5f);

  // Untouched derived values stay exactly as built.
  Overrides other = {{"Shadows.Strength", "0.5"}};
  War3RenderSettings plain;
  ApplyOverrides(other, plain, extras);
  CHECK(plain.shadows.shadowTaaSettingsRevision == revision);
  CHECK(!plain.shadows.shadowTaaEnabled);
}

static void TestRestoreBaseline() {
  War3RenderSettings baseline;
  baseline.shadows.csm.shadowResolution = 2048u;  // e.g. low VRAM tier
  Extras baselineExtras;
  baselineExtras.nativeShadowMode = 1u;

  War3RenderSettings settings = baseline;
  Extras extras = baselineExtras;
  Overrides saved = {{"Shadows.CsmResolution", "4096"},
                     {"Shadows.TaaMode", "2"},
                     {"General.FpsUnlocked", "1"},
                     {"Shadows.NativeMode", "2"},
                     {"Future.Thing", "x"}};
  ApplyOverrides(saved, settings, extras);
  CHECK(settings.shadows.csm.shadowResolution == 4096u);
  CHECK(extras.fpsUnlocked && extras.nativeShadowMode == 2u);
  const uint64_t revision = settings.shadows.shadowTaaSettingsRevision;

  RestoreBaseline(saved, baseline, baselineExtras, settings, extras);
  CHECK(settings.shadows.csm.shadowResolution == 2048u);
  CHECK(settings.shadows.shadowTaaMode == War3ShadowTaaMode::DirectInline);
  CHECK(!settings.shadows.shadowTaaEnabled);
  CHECK(settings.shadows.shadowTaaSettingsRevision == revision + 1u);
  CHECK(!extras.fpsUnlocked && extras.nativeShadowMode == 1u);
  CHECK(saved.size() == 1u && saved.count("Future.Thing") == 1u);
}

static void TestFileText() {
  Overrides saved = {{"Shadows.Enabled", "0"}, {"PostFx.Exposure", "1.25"},
                     {"Future.Thing", "7"}};
  const std::string text = SerializeFile(saved);
  CHECK(text.compare(0, 3, "\xEF\xBB\xBF") == 0);
  CHECK(text.find("[PlayerSettings]\r\n") != std::string::npos);
  CHECK(text.find("PostFx.Exposure=1.25\r\n") != std::string::npos);
  size_t ignored = 99;
  CHECK(ParseFile(text, &ignored) == saved);
  CHECK(ignored == 0u);

  const std::string messy =
      "Shadows.Enabled=1\n"                 // before any section: ignored
      "[Graphics]\nVramPreset=low\n"        // other section: ignored
      "  [PlayerSettings]  \n"
      "; comment\n# comment\n\n"
      "  Sun.Intensity =  1.5  \r\n"
      "no equals sign\n"
      "Bad Key=1\n"
      "Sun.Enabled=\xE4\xBD\x8E\n"           // non-ASCII value
      "Sun.Intensity=2\n"                   // last duplicate wins
      "Empty.Value=\n"
      "[Other]\nAA.Mode=1";
  const Overrides parsed = ParseFile(messy, &ignored);
  CHECK(parsed.size() == 1u);
  CHECK(parsed.at("Sun.Intensity") == "2");
  CHECK(ignored == 7u);
  CHECK(ParseFile("", &ignored).empty() && ignored == 0u);
}

static void TestFileIo(const std::wstring& root) {
  const std::wstring dir = root + L"\\WarVK";
  const std::wstring path = dir + L"\\settings.ini";
  std::string text = "x";
  CHECK(ReadTextFile(path, text) && text.empty());  // missing dir and file
  CHECK(WriteTextFileAtomically(path, "abc\r\n"));  // creates WarVK
  CHECK(ReadTextFile(path, text) && text == "abc\r\n");
  CHECK(WriteTextFileAtomically(path, "second"));   // replaces
  CHECK(ReadTextFile(path, text) && text == "second");
  CHECK(!FileExists(path + L".tmp"));
  CHECK(DeleteFileIfPresent(path));
  CHECK(!FileExists(path));
  CHECK(DeleteFileIfPresent(path));

  // Oversized files are refused instead of half-read.
  CHECK(WriteTextFileAtomically(path, std::string(70 * 1024, 'a')));
  CHECK(!ReadTextFile(path, text) && text.empty());
  CHECK(DeleteFileIfPresent(path));
  CHECK(RemoveDirectoryW(dir.c_str()));
}

static void TestStore(const std::wstring& root) {
  const std::wstring path = root + L"\\WarVK\\settings.ini";

  // First run: no file. The panel turns shadows off and unlocks FPS.
  ResetStoreForTest(path);
  War3RenderSettings live;
  Extras extras;
  ApplyReport report = ApplyAtStartup(live, extras);
  CHECK(report.applied == 0u && report.unknown == 0u);
  CHECK(GetStoreStatus().savedKeys == 0u && !GetStoreStatus().pending);

  War3RenderSettings before = live;
  live.shadows.enabled = false;
  RecordPanelEdit(before, live);
  Extras extrasAfter = extras;
  extrasAfter.fpsUnlocked = true;
  RecordExtrasEdit(extras, extrasAfter);
  StoreStatus status = GetStoreStatus();
  CHECK(status.savedKeys == 2u && status.pending);
  FlushIfDue(false);  // still inside the settle delay
  CHECK(!FileExists(path) && GetStoreStatus().pending);
  FlushIfDue(true);   // panel closed
  CHECK(FileExists(path) && !GetStoreStatus().pending && !GetStoreStatus().lastSaveFailed);

  // A frame without changes records nothing.
  before = live;
  RecordPanelEdit(before, live);
  CHECK(!GetStoreStatus().pending);

  // Second run: the saved choices come back on top of fresh defaults, and the
  // baseline is what the pipeline had before they were applied.
  ResetStoreForTest(path);
  War3RenderSettings restarted;
  restarted.shadows.csm.shadowResolution = 2048u;  // VRAM tier ran first
  Extras restartedExtras;
  report = ApplyAtStartup(restarted, restartedExtras);
  CHECK(report.applied == 2u);
  CHECK(!restarted.shadows.enabled && restartedExtras.fpsUnlocked);
  CHECK(GetStoreStatus().savedKeys == 2u && !GetStoreStatus().pending);

  // A second device in the same process re-applies but keeps the first baseline.
  War3RenderSettings secondDevice;
  Extras secondExtras;
  secondExtras.fpsUnlocked = true;
  ApplyAtStartup(secondDevice, secondExtras);
  CHECK(!secondDevice.shadows.enabled);

  // Reset puts the baseline back and removes the file on the next flush.
  CHECK(ResetToBaseline(restarted, restartedExtras));
  CHECK(restarted.shadows.enabled && !restartedExtras.fpsUnlocked);
  CHECK(restarted.shadows.csm.shadowResolution == 2048u);
  CHECK(GetStoreStatus().savedKeys == 0u && GetStoreStatus().pending);
  FlushIfDue(false);
  CHECK(!FileExists(path) && !GetStoreStatus().pending);
  CHECK(!ResetToBaseline(restarted, restartedExtras));  // nothing left to reset

  // An unwritable location reports failure once and does not retry every frame.
  const std::wstring blocker = root + L"\\blocker";
  CHECK(WriteTextFileAtomically(blocker, "file, not a directory"));
  ResetStoreForTest(blocker + L"\\settings.ini");
  War3RenderSettings blocked;
  ApplyAtStartup(blocked, extras);
  before = blocked;
  blocked.postFx.exposure = 1.5f;
  RecordPanelEdit(before, blocked);
  FlushIfDue(true);
  status = GetStoreStatus();
  CHECK(status.lastSaveFailed && !status.pending && status.savedKeys == 1u);
  CHECK(DeleteFileIfPresent(blocker));
  ResetStoreForTest(L"");
}

int main() {
  TestKeys();
  TestDecimal();
  TestRecordAndApply();
  TestClampRejectUnknown();
  TestDerivedValues();
  TestRestoreBaseline();
  TestFileText();
  const std::wstring root = MakeTempRoot();
  TestFileIo(root);
  TestStore(root);
  CHECK(RemoveDirectoryW((root + L"\\WarVK").c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND);
  CHECK(RemoveDirectoryW(root.c_str()));
  std::printf("player_settings: %u checks passed\n", checks);
  return 0;
}
