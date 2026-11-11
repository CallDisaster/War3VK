// Links the shipping strict parser, managed registry, weather dispatcher and
// WeatherRuntime. WARVK_JAPI_PROTOCOL_TEST only excludes the old GPU/native
// backends; it does not replace any weather implementation with a test stub.
#include "../src/d3d9/war3/japi/war3_japi_v1.h"
#include "../src/d3d9/war3/render/war3_weather_runtime.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace dxvk::war3::japi;
using dxvk::war3::render::WeatherRuntime;

namespace {
unsigned checks = 0;
unsigned failures = 0;

void check(bool value, const char* label) {
  ++checks;
  if (!value) {
    ++failures;
    std::printf("FAIL: %s\n", label);
  }
}

Reply send(Carrier carrier, const std::string& command,
           const std::string& arguments = {}) {
  return Dispatch(carrier, "warvk:v1;" + command + arguments);
}

std::string idArg(int32_t id) {
  return ";d:" + std::to_string(id);
}

bool near(float a, float b) {
  return std::fabs(a - b) < 1.0e-5f;
}

int32_t create(const char* seed = "123") {
  Reply reply = send(Carrier::Hotkey, "weather.createSnow", std::string(";i:") + seed);
  check(reply.ok() && reply.kind == ResultKind::Integer && reply.integer > 0,
        "create uses real CPU author path");
  return reply.integer;
}

void expectError(Carrier carrier, const char* name,
                 const std::string& arguments, ErrorCode expected) {
  Reply reply = send(carrier, name, arguments);
  check(reply.consumed && reply.error == expected, name);
}

void strictEnvelope(int32_t id) {
  struct Row { const char* command; Carrier carrier; const char* tail; };
  const std::array<Row, 12> rows = {{
    {"weather.createSnow", Carrier::Hotkey, ";i:1"},
    {"weather.destroy", Carrier::Preloader, ""},
    {"weather.isAlive", Carrier::Hotkey, ""},
    {"weather.setRegion", Carrier::Preloader, ";r:0;r:0;r:100;r:100;r:0;r:100;r:0.1"},
    {"weather.setEnabled", Carrier::Preloader, ";b:1"},
    {"weather.setSnowTarget", Carrier::Preloader, ";r:0.1;r:20;r:40;r:0.2;r:0.3;r:0.4;r:0"},
    {"weather.clearKeyframes", Carrier::Preloader, ""},
    {"weather.addSnowKeyframe", Carrier::Preloader, ";r:0;r:0.1;r:20;r:40;r:0.2;r:0.3;r:0.4"},
    {"weather.playTimeline", Carrier::Preloader, ";r:0"},
    {"weather.setPaused", Carrier::Preloader, ";b:1"},
    {"weather.setTime", Carrier::Preloader, ";r:5"},
    {"weather.getTime", Carrier::LocalizedString, ""},
  }};
  for (const Row& row : rows) {
    const bool creation = std::string(row.command) == "weather.createSnow";
    const std::string args = (creation ? "" : idArg(id)) + row.tail;
    const Carrier wrong = row.carrier == Carrier::Preloader
        ? Carrier::Hotkey : Carrier::Preloader;
    expectError(wrong, row.command, args, ErrorCode::CarrierMismatch);
    expectError(row.carrier, row.command, args + ";i:1", ErrorCode::ArgumentCountMismatch);
    const size_t finalToken = args.rfind(';');
    expectError(row.carrier, row.command, args.substr(0, finalToken),
                ErrorCode::ArgumentCountMismatch);
    if (!creation)
      expectError(row.carrier, row.command, ";i:" + std::to_string(id) + row.tail,
                  ErrorCode::InvalidId);
  }
  expectError(Carrier::Hotkey, "weather.createSnow", ";i:2147483648", ErrorCode::IntegerOverflow);
  expectError(Carrier::Hotkey, "weather.createSnow", ";r:1", ErrorCode::InvalidInteger);
  expectError(Carrier::Preloader, "weather.setEnabled", idArg(id) + ";b:true", ErrorCode::InvalidBoolean);
  expectError(Carrier::Preloader, "weather.setPaused", idArg(id) + ";i:1", ErrorCode::InvalidBoolean);
  expectError(Carrier::LocalizedString, "weather.getTime", ";d:0", ErrorCode::InvalidId);
  expectError(Carrier::LocalizedString, "weather.getTime", ";d:-1", ErrorCode::InvalidId);
  expectError(Carrier::Preloader, "weather.setTime", idArg(id) + ";r:nan", ErrorCode::InvalidReal);
  expectError(Carrier::Preloader, "weather.setTime", idArg(id) + ";r:inf", ErrorCode::InvalidReal);
  expectError(Carrier::Preloader, "weather.setTime", idArg(id) + ";r:1e100", ErrorCode::RealOutOfRange);
}
} // namespace

int main() {
  ResetAuthorState();
  check(!IsRuntimeReady(), "weather test has no active render pipeline");
  check(GetFeatureFlags() == (0x00020000u | 0x00000400u),
        "CPU weather and registry capabilities are visible before render ready");
  Reply flags = send(Carrier::Hotkey, "system.featureFlags");
  check(flags.ok() && flags.integer == static_cast<int32_t>(GetFeatureFlags()), "wire capability matches direct query");
  check(send(Carrier::Hotkey, "managedObject.count").integer == 0, "initial managed registry is empty");

  const int32_t first = create("-2147483648");
  auto snapshot = WeatherRuntime::instance().snapshot(100, 7, 9);
  check(snapshot.id > 0 && !snapshot.enabled && snapshot.seed == 0x80000000u,
        "signed JASS seed preserves all 32 bits and creation starts disabled");
  check(snapshot.frameSerial == 100 && snapshot.mapEpoch == 7 && snapshot.deviceEpoch == 9,
        "snapshot preserves render frame identity");
  Reply alive = send(Carrier::Hotkey, "weather.isAlive", idArg(first));
  check(alive.ok() && alive.integer == 1, "weather public id resolves");
  check(send(Carrier::Hotkey, "managedObject.isAlive", idArg(first)).integer == 1,
        "generic registry knows the weather object");
  check(send(Carrier::Hotkey, "managedObject.type", idArg(first)).integer == 4,
        "generic registry reports the public weather type");
  check(send(Carrier::Hotkey, "managedObject.count").integer == 1, "weather counts exactly once");
  expectError(Carrier::Hotkey, "weather.createSnow", ";i:5", ErrorCode::BackendRejected);
  strictEnvelope(first);

  const std::string id = idArg(first);
  check(send(Carrier::Preloader, "weather.setRegion", id + ";r:-100;r:-200;r:300;r:400;r:50;r:500;r:0.2").ok(), "region command accepted");
  check(send(Carrier::Preloader, "weather.setSnowTarget", id + ";r:0.1;r:20;r:40;r:0.2;r:0.3;r:0.4;r:0").ok(), "target command accepted");
  snapshot = WeatherRuntime::instance().snapshot(101, 7, 9);
  check(near(snapshot.region.minX, -100) && near(snapshot.region.minY, -200) &&
        near(snapshot.region.maxX, 300) && near(snapshot.region.maxY, 400) &&
        near(snapshot.region.bottomZ, 50) && near(snapshot.region.topZ, 500) && near(snapshot.region.edgeFeather, 0.2f),
        "all seven region wire fields land in their real snapshot slots");
  check(near(snapshot.params.snowfall, 0.1f) && near(snapshot.params.windAngle, 20) &&
        near(snapshot.params.windSpeed, 40) && near(snapshot.params.gust, 0.2f) &&
        near(snapshot.params.haze, 0.3f) && near(snapshot.params.lowSnow, 0.4f),
        "all six target wire fields land in their real snapshot slots");
  const uint64_t beforeInvalid = snapshot.revision;
  expectError(Carrier::Preloader, "weather.setRegion", id + ";r:0;r:0;r:0;r:100;r:0;r:100;r:0.1", ErrorCode::BackendRejected);
  expectError(Carrier::Preloader, "weather.setRegion", id + ";r:0;r:0;r:100;r:100;r:0;r:0;r:0.1", ErrorCode::BackendRejected);
  expectError(Carrier::Preloader, "weather.setRegion", id + ";r:0;r:0;r:100;r:100;r:0;r:100;r:0.6", ErrorCode::BackendRejected);
  expectError(Carrier::Preloader, "weather.setSnowTarget", id + ";r:1.1;r:20;r:40;r:0.2;r:0.3;r:0.4;r:0", ErrorCode::BackendRejected);
  expectError(Carrier::Preloader, "weather.setSnowTarget", id + ";r:0.1;r:20;r:2049;r:0.2;r:0.3;r:0.4;r:0", ErrorCode::BackendRejected);
  expectError(Carrier::Preloader, "weather.setSnowTarget", id + ";r:0.1;r:20;r:40;r:0.2;r:0.3;r:0.4;r:3601", ErrorCode::BackendRejected);
  expectError(Carrier::Preloader, "weather.setTime", id + ";r:86401", ErrorCode::BackendRejected);
  check(WeatherRuntime::instance().snapshot(102, 7, 9).revision == beforeInvalid,
        "invalid author commands do not change the revision");

  check(send(Carrier::Preloader, "weather.setEnabled", id + ";b:1").ok(), "enable command accepted");
  check(send(Carrier::Preloader, "weather.setPaused", id + ";b:1").ok(), "pause command accepted");
  check(send(Carrier::Preloader, "weather.setTime", id + ";r:123").ok(), "seek command accepted while paused");
  Reply time = send(Carrier::LocalizedString, "weather.getTime", id);
  check(time.ok() && time.kind == ResultKind::Text && time.text == "123", "legacy localized-string carrier formats real time correctly");
  snapshot = WeatherRuntime::instance().snapshot(103, 7, 9);
  check(snapshot.enabled && snapshot.paused && snapshot.timeSeconds == 123.0,
        "pause and seek reach real production state");

  // The production LocalizedString query is decoded by stock JASS S2R, not
  // an exponent-aware C++ parser. A tiny post-play time must stay tiny.
  const char* decimalTimes[] = {"0", "0.0000973", "0.0000001",
      "0.000000000000000000000000000000000000000000001", "0.001234567",
      "1.234567", "86400"};
  for (const char* decimal : decimalTimes) {
    check(send(Carrier::Preloader, "weather.setTime", id + ";r:" + decimal).ok(),
          "fixed-decimal real query setup accepted");
    const Reply queried = send(Carrier::LocalizedString, "weather.getTime", id);
    check(queried.ok() && queried.kind == ResultKind::Text && !queried.text.empty(),
          "fixed-decimal real query returns text");
    check(queried.text.find_first_of("eE") == std::string::npos,
          "real reply contains no S2R-incompatible exponent");
    float decoded = -1.0f;
    const auto parsed = std::from_chars(queried.text.data(),
        queried.text.data() + queried.text.size(), decoded, std::chars_format::fixed);
    const auto exact = WeatherRuntime::instance().snapshot(103, 7, 9);
    check(parsed.ec == std::errc{} && parsed.ptr == queried.text.data() + queried.text.size()
        && decoded == float(exact.timeSeconds),
          "shortest fixed real reply preserves the binary32 query value");
  }

  check(send(Carrier::Preloader, "weather.clearKeyframes", id).ok(), "clear keyframes command accepted");
  expectError(Carrier::Preloader, "weather.addSnowKeyframe", id + ";r:1;r:0;r:350;r:0;r:0;r:0;r:0", ErrorCode::BackendRejected);
  check(send(Carrier::Preloader, "weather.addSnowKeyframe", id + ";r:0;r:0;r:350;r:0;r:0;r:0;r:0").ok(), "first keyframe accepted");
  check(send(Carrier::Preloader, "weather.addSnowKeyframe", id + ";r:10;r:1;r:10;r:200;r:1;r:0.8;r:0.6").ok(), "second keyframe accepted");
  check(send(Carrier::Preloader, "weather.playTimeline", id + ";r:5").ok(), "timeline command accepted");
  check(send(Carrier::Preloader, "weather.setPaused", id + ";b:1").ok(), "timeline pause accepted");
  check(send(Carrier::Preloader, "weather.setTime", id + ";r:5").ok(), "exact midpoint seek accepted");
  snapshot = WeatherRuntime::instance().snapshot(104, 7, 9);
  check(snapshot.enabled && snapshot.paused && near(snapshot.params.snowfall, 0.5f) &&
        near(snapshot.params.windAngle, 0) && near(snapshot.params.windSpeed, 100) &&
        near(snapshot.params.gust, 0.5f) && near(snapshot.params.haze, 0.4f) && near(snapshot.params.lowSnow, 0.3f),
        "keyframe wire values and shortest-angle interpolation reach the snapshot");
  expectError(Carrier::Preloader, "weather.clearKeyframes", id, ErrorCode::BackendRejected);
  expectError(Carrier::Preloader, "weather.addSnowKeyframe", id + ";r:20;r:1;r:0;r:0;r:0;r:0;r:0", ErrorCode::BackendRejected);
  check(send(Carrier::Preloader, "weather.setEnabled", id + ";b:0").ok(), "disable command accepted");
  check(!WeatherRuntime::instance().snapshot(105, 7, 9).enabled, "disabled state published");
  check(send(Carrier::Preloader, "weather.destroy", id).ok(), "destroy command accepted");
  check(send(Carrier::Hotkey, "weather.isAlive", id).integer == 0, "destroyed public id no longer resolves");
  check(send(Carrier::Hotkey, "managedObject.count").integer == 0, "destroy removed the managed entry");

  const int32_t oldMapId = create();
  ResetAuthorState();
  check(send(Carrier::Hotkey, "weather.isAlive", idArg(oldMapId)).integer == 0, "map reset rejects old weather handle");
  const int32_t newMapId = create("2147483647");
  check(newMapId != oldMapId && newMapId != first, "public ids are not reused across destroy or reset");
  expectError(Carrier::Preloader, "weather.setEnabled", idArg(oldMapId) + ";b:1", ErrorCode::BackendRejected);
  check(!WeatherRuntime::instance().snapshot(1, 8, 9).enabled, "late old-map command does not enable new weather");
  check(send(Carrier::Hotkey, "managedObject.type", idArg(newMapId)).integer == 4, "new-map object type survives generic query");
  check(send(Carrier::Hotkey, "managedObject.type", idArg(oldMapId)).integer == 0, "generic query rejects old-map handle");

  for (unsigned index = 0; index < WeatherRuntime::kMaxKeyframes; ++index) {
    check(send(Carrier::Preloader, "weather.addSnowKeyframe", idArg(newMapId) +
          ";r:" + std::to_string(index) + ";r:0.2;r:0;r:10;r:0;r:0;r:0").ok(),
          "bounded keyframe accepted through production parser");
  }
  expectError(Carrier::Preloader, "weather.addSnowKeyframe", idArg(newMapId) +
              ";r:64;r:0.2;r:0;r:10;r:0;r:0;r:0", ErrorCode::BackendRejected);
  check(send(Carrier::Preloader, "weather.clearKeyframes", idArg(newMapId)).ok(), "keyframe capacity recovers after clear");
  check(send(Carrier::Preloader, "weather.addSnowKeyframe", idArg(newMapId) +
        ";r:0;r:0.2;r:0;r:10;r:0;r:0;r:0").ok(), "clear permits first keyframe again");
  ResetWeatherAuthorState();
  check(send(Carrier::Hotkey, "weather.isAlive", idArg(newMapId)).integer == 0,
        "MainRunner fallback retirement rejects previous weather handle");
  check(send(Carrier::Hotkey, "managedObject.count").integer == 0,
        "fallback retirement removes weather public registry entry");
  check(!WeatherRuntime::instance().snapshot(1, 9, 9).enabled,
        "old weather is not relabelled as active new-map state");
  const int32_t fallbackMapId = create();
  check(fallbackMapId != newMapId, "fallback does not recycle author handles");
  check(send(Carrier::Preloader, "weather.setEnabled", idArg(fallbackMapId) + ";b:1").ok(),
        "new-map weather can enable after fallback retirement");
  expectError(Carrier::Preloader, "weather.setEnabled", idArg(newMapId) + ";b:1", ErrorCode::BackendRejected);
  check(WeatherRuntime::instance().snapshot(2, 9, 9).enabled,
        "late old-map command does not clear valid new-map weather");
  ResetAuthorState();
  std::printf("weather_protocol: %u checks, %u failures\n", checks, failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
