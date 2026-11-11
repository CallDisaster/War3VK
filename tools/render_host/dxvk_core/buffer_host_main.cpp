#include "buffer_backend.h"
#include "../buffer_lab_protocol.h"
#include "../../../src/util/log/log.h"
#include <cstdio>
#include <memory>

namespace dxvk { Logger Logger::s_instance("warvk-dxvk-buffer-host.log"); }
using namespace warvk::host;
using bufferlab::Require;
static_assert(sizeof(void*) == 8, "Actual DXVK buffer owner must be x64");

namespace {
// Sole resource execution entry: only the private preflight/commit Session
// invokes this callback. Resource IDs become slot indices after validation.
class Adapter final : public buffers::Backend {
public:
  dxvkcore::BufferBackend gpu;
  uint64_t calls = 0;
  dxvkcore::BufferBackend::Verification verified{};
  bool apply(const buffers::View& view) override {
    const auto& h = view.header;
    const auto slot = uint32_t(h.resourceId - 1);
    ++calls;
    verified = {};
    switch (h.op) {
      case buffers::Op::Create: gpu.create(slot, uint32_t(h.argument)); break;
      case buffers::Op::Upload: gpu.upload(slot, uint32_t(h.argument), view.payload, h.payloadBytes); break;
      case buffers::Op::Verify: verified = gpu.verify(slot); break;
      case buffers::Op::Destroy: gpu.destroy(slot); break;
      case buffers::Op::Finish: gpu.finish(); break;
    }
    return true;
  }
};
}

int wmain(int argc, wchar_t** argv) {
  std::unique_ptr<Adapter> adapter;
  uint64_t accepted = 0;
  bool peerVerified = false;
  const char* stage = "arguments";
  buffers::Error lastError = buffers::Error::None;
  try {
    Require(argc == 4, "nonce parent-pid parent-creation");
    const auto nonce = lab::ParseNonce(argv[1]);
    const auto pidValue = std::stoull(argv[2]);
    Require(pidValue > 0 && pidValue <= UINT32_MAX, "parent-pid-range");
    const auto parentPid = DWORD(pidValue);
    const auto parentCreation = std::stoull(argv[3]);
    stage = "exact-parent-connection";
    auto parent = lab::OpenPeer(parentPid, parentCreation);
    auto pipe = lab::CreateServer(nonce, lab::Channel::Buffers4);
    lab::IoStats io;
    lab::Accept(pipe.get(), parent.get(), io);
    lab::VerifyPeer(pipe.get(), true, parentPid);
    peerVerified = true;
    Require(!GetModuleHandleW(L"Game.dll") && !GetModuleHandleW(L"d3d9.dll"), "unexpected-game-module");
    stage = "actual-dxvk-owner";
    adapter = std::make_unique<Adapter>();
    buffers::Session session(commands::Scope{nonce.low, nonce.high, 1, 1});
    buffers::Packet packet;
    for (;;) {
      stage = "read-command";
      size_t bytes = 0;
      lastError = bufferlab::ReadCommand(pipe.get(), parent.get(), packet, bytes, io);
      if (lastError != buffers::Error::None) session.fault();
      else {
        stage = "preflight-execute-commit";
        lastError = session.execute(packet.data(), bytes, *adapter);
      }
      const auto snapshot = session.snapshot();
      accepted = snapshot.accepted;
      const auto& stats = adapter->gpu.stats();
      bufferlab::Reply r;
      r.error = lastError; r.accepted = accepted; r.backendCalls = adapter->calls;
      r.submitted = stats.submitted; r.gpuCompleted = stats.gpuCompleted; r.retired = stats.retired;
      r.liveBytes = stats.liveBytes;
      if (lastError == buffers::Error::None) {
        r.verifiedBytes = adapter->verified.bytes; r.hash = adapter->verified.hash;
      }
      const auto reply = bufferlab::EncodeReply(packet, r);
      stage = "reply";
      lab::WriteBytes(pipe.get(), parent.get(), reply.data(), reply.size(), io);
      if (lastError != buffers::Error::None || snapshot.state == buffers::State::Finished) break;
    }
    const auto stats = adapter->gpu.stats();
    const auto calls = adapter->calls;
    // Normal Finish has destroyed all resources and the real DXVK device. In a
    // fault, no cleanup is inferred from a CPU rejection: BufferBackend retains
    // its owner for terminal process containment. Do not report normal teardown.
    adapter.reset();
    const bool success = lastError == buffers::Error::None;
    Require(!success || stats.normalTeardown, "normal-dxvk-teardown-missing");
    std::printf("{\"schema\":\"warvk-buffer-host/v1\",\"ok\":%s,\"bits\":64,"
      "\"actualDxvkCore\":true,\"peerVerified\":true,\"accepted\":%llu,\"backendCalls\":%llu,"
      "\"error\":\"%s\",\"submitted\":%llu,\"gpuCompleted\":%llu,\"retired\":%llu,"
      "\"liveBytes\":%llu,\"peakLiveBytes\":%llu,\"created\":%llu,\"destroyed\":%llu,"
      "\"uploadedBytes\":%llu,\"verifiedBytes\":%llu,\"normalTeardown\":%s,"
      "\"vendorId\":%u,\"deviceId\":%u,\"productIntegrated\":false,\"gameTouched\":false,"
      "\"memorySavingsProven\":false}\n", success ? "true" : "false", accepted, calls,
      buffers::ErrorName(lastError), stats.submitted, stats.gpuCompleted, stats.retired, stats.liveBytes,
      stats.peakLiveBytes, stats.created, stats.destroyed, stats.uploadedBytes, stats.verifiedBytes,
      stats.normalTeardown ? "true" : "false", stats.vendorId, stats.deviceId);
    if (!success) {
      // Quarantined DXVK threads may still exist. Do not run Logger/static
      // destructors concurrently with them on a protocol/backend failure.
      std::fflush(stdout); std::fflush(stderr);
      ExitProcess(2);
    }
    return 0;
  } catch (const std::exception& error) {
    (void)adapter.release(); // Unknown GPU state: exact process boundary, not fake CPU retirement.
    std::fprintf(stderr, "buffer host failure stage=%s error=%s\n", stage, error.what());
    std::printf("{\"schema\":\"warvk-buffer-host/v1\",\"ok\":false,\"bits\":64,"
      "\"stage\":\"%s\",\"peerVerified\":%s,\"accepted\":%llu,\"normalTeardown\":false}\n",
      stage, peerVerified ? "true" : "false", accepted);
    std::fflush(stdout); std::fflush(stderr);
    ExitProcess(2);
  } catch (...) {
    (void)adapter.release();
    std::fprintf(stderr, "buffer host unknown failure stage=%s\n", stage);
    std::fflush(stderr);
    ExitProcess(3);
  }
}
