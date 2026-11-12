#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dxvk {
struct War3RenderSettings;
}

namespace dxvk::war3::player {

// Player choices made in the Ctrl+F1 panel, saved to <game>\WarVK\settings.ini.
//
// Only keys the player actually changed are stored. Everything else keeps the
// build default, so a later release can still improve untouched defaults.
// Keys this build does not know (written by a newer build) are kept verbatim
// and written back, never applied.
//
// Order at device creation: build defaults -> VRAM tier -> these saved choices
// -> diagnostic DXVK_WAR3_* environment overrides. Map scripts (JAPI) keep
// writing through the settings mailbox afterwards and are never recorded here.
using Overrides = std::map<std::string, std::string>;

// Panel state that lives outside War3RenderSettings.
struct Extras {
  bool fpsUnlocked = false;
  uint32_t nativeShadowMode = 0;
};

struct ApplyReport {
  size_t applied = 0;
  size_t unknown = 0;                 // kept in the file, not applied
  std::vector<std::string> rejected;  // known key, unusable value: dropped
};

// Applies every known key, clamping to the panel's own ranges. Known keys with
// an unusable value are reported and erased from `overrides`.
ApplyReport ApplyOverrides(Overrides& overrides, War3RenderSettings& settings,
                           Extras& extras);

// Records every persisted field whose value differs between the two states.
// Returns the number of keys written.
size_t RecordChanges(const War3RenderSettings& before,
                     const War3RenderSettings& after, Overrides& overrides);
size_t RecordExtraChanges(const Extras& before, const Extras& after,
                          Overrides& overrides);

// Puts every overridden known field back to its baseline value and erases the
// known keys; unknown keys stay.
void RestoreBaseline(Overrides& overrides,
                     const War3RenderSettings& baselineSettings,
                     const Extras& baselineExtras,
                     War3RenderSettings& settings, Extras& extras);

bool IsKnownKey(const std::string& key);
size_t KnownKeyCount();
std::vector<std::string> KnownKeys();

// settings.ini text: UTF-8 with BOM, comment lines, one [PlayerSettings]
// section of key=value lines. Malformed lines are skipped and counted.
std::string SerializeFile(const Overrides& overrides);
Overrides ParseFile(const std::string& text, size_t* ignoredLines = nullptr);

// Locale-independent decimal text used for float values.
std::string FormatDecimal(double value);
bool ParseDecimal(const std::string& text, double& value);

// File I/O. A missing file reads as empty text and succeeds. Writes go to a
// temporary file first and then replace the target, so a crash mid-write never
// leaves a half-written settings.ini.
bool ReadTextFile(const std::wstring& path, std::string& text);
bool WriteTextFileAtomically(const std::wstring& path, const std::string& text);
bool DeleteFileIfPresent(const std::wstring& path);

// <directory of war3.exe>\WarVK\settings.ini
std::wstring DefaultSettingsPath();

// ---------------------------------------------------------------------------
// Process-wide store shared by the render pipeline and the panel. All entry
// points are thread-safe; file I/O happens only in ApplyAtStartup (read) and
// FlushIfDue (write/delete).
// ---------------------------------------------------------------------------

// The first call reads the file. Every call applies the saved choices to the
// given state; the first call also remembers the state it was given as the
// baseline used by ResetToBaseline.
ApplyReport ApplyAtStartup(War3RenderSettings& settings, Extras& extras);

void RecordPanelEdit(const War3RenderSettings& before,
                     const War3RenderSettings& after);
void RecordExtrasEdit(const Extras& before, const Extras& after);

// Restores the baseline for every saved key and schedules the file removal.
// Returns false when nothing was saved or no baseline exists yet.
bool ResetToBaseline(War3RenderSettings& settings, Extras& extras);

// Saves pending edits once nothing has changed for kSaveDelayMs, or right away
// when `force` is set (panel closed, device teardown).
constexpr uint64_t kSaveDelayMs = 1500u;
void FlushIfDue(bool force);

struct StoreStatus {
  size_t savedKeys = 0;
  bool pending = false;
  bool lastSaveFailed = false;
};
StoreStatus GetStoreStatus();

// Test hooks: use another file and forget all loaded state.
void ResetStoreForTest(const std::wstring& path);

} // namespace dxvk::war3::player
