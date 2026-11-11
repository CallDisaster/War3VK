#include "../tools/render_host/buffer_lab_protocol.h"
#include "../tools/render_host/owned_child.h"
#include <atomic>
#include <cstdio>
#include <exception>
#include <memory>
#include <thread>

using namespace warvk::host;
using bufferlab::Require;
static_assert(sizeof(void*) == 4, "Real x86 queue/transport parent required");

int wmain(int argc, wchar_t** argv) {
  DWORD childPid = 0, childExit = STILL_ACTIVE;
  uint64_t childCreation = 0, exchanges = 0, fullRefusals = 0;
  bool peerVerified = false, expectedRejected = false;
  bufferlab::Reply last;
  std::array<uint64_t, 3> hashes{};
  size_t hashCount = 0;
  try {
    Require(argc == 4, "arguments: exact-host-exe fresh-host-log normal|stale-generation|wrong-map|bad-range");
    const std::wstring mode = argv[3];
    Require(mode == L"normal" || mode == L"stale-generation" || mode == L"wrong-map" || mode == L"bad-range", "case-name");
    Require(!GetModuleHandleW(L"Game.dll") && !GetModuleHandleW(L"vulkan-1.dll"), "unexpected-renderer-in-x86");
    const auto nonce = lab::RandomNonce();
    const commands::Scope scope{nonce.low, nonce.high, 1, 1};
    auto queue = std::make_unique<commands::SpscRenderCommandQueue>(scope);
    // Producer performs bounded encode/copy only. Exact process ownership,
    // blocking laboratory IPC and child settlement all belong to this worker.
    // The caller's explicit retries below are a lab policy, not a D3D9 contract.
    std::atomic<bool> startWorker{false};
    std::exception_ptr workerFailure;
    std::thread worker([&] {
      try {
        while (!startWorker.load(std::memory_order_acquire)) Sleep(1);
        const std::wstring args = lab::NonceText(nonce) + L" " + std::to_wstring(GetCurrentProcessId())
          + L" " + std::to_wstring(lab::CreationTime(GetCurrentProcess()));
        lab::OwnedChild child(argv[1], args, argv[2]);
        childPid = child.pid(); childCreation = child.creation();
        try {
          auto pipe = lab::Connect(nonce, child.process(), child.pid(), lab::Channel::Buffers4);
          peerVerified = true;
          commands::Batch batch;
          for (;;) {
            auto result = queue->tryDequeue(batch);
            if (result == commands::Dequeue::Empty) { Sleep(1); continue; }
            if (result == commands::Dequeue::Finished) break;
            Require(result == commands::Dequeue::Item, "worker-queue-state");
            buffers::View view;
            Require(buffers::Decode(batch.payload.data(), batch.bytes, view) == buffers::Error::None,
                    "local-encoded-command");
            lab::IoStats io;
            lab::WriteBytes(pipe.get(), child.process(), batch.payload.data(), batch.bytes, io);
            bufferlab::ReplyPacket reply;
            const lab::ReadWindow deadline;
            lab::ReadExact(pipe.get(), child.process(), reply.data(), reply.size(), deadline, lab::Stage::Body, io);
            last = bufferlab::DecodeReply(batch.payload.data(), reply);
            ++exchanges;
            if (last.error != buffers::Error::None) {
              Require(mode != L"normal" && exchanges == 10 && last.accepted == 9, "unexpected-remote-rejection");
              expectedRejected = true;
              queue->requestFault(); // No CPU ack, replay or scope revival for rejected batch.
              break;
            }
            Require(last.accepted == exchanges && last.backendCalls == exchanges, "exact-remote-acceptance");
            Require(last.gpuCompleted == last.submitted && last.retired == last.submitted, "missing-gpu-retirement");
            if (view.header.op == buffers::Op::Verify) {
              const auto bytes = view.header.generation == 1 ? 131101u : 98307u;
              const auto expected = bufferlab::Golden(bytes, uint32_t(view.header.generation), hashCount == 1);
              Require(hashCount < hashes.size() && last.verifiedBytes == bytes && last.hash == expected,
                      "GPU-buffer-bytes-or-hash");
              hashes[hashCount++] = last.hash;
            } else Require(last.verifiedBytes == 0 && last.hash == 0, "unexpected-readback-result");
            Require(queue->acknowledgeCpu(scope, batch.sequence) == commands::CpuAck::Acknowledged, "exact-local-cpu-ack");
          }
          childExit = child.wait(5000);
          Require(childExit == (expectedRejected ? 2u : 0u), "host-exit");
        } catch (...) {
          // Preserve the exact host's terminal report before Job containment.
          childExit = child.wait(5000);
          throw;
        }
      } catch (...) {
        workerFailure = std::current_exception();
        queue->requestFault();
      }
    });
    std::exception_ptr producerFailure;
    uint64_t sequence = 1;
    try {
      buffers::Packet encoded;
      std::array<uint8_t, buffers::MaxUploadBytes> payload{};
      const auto until = GetTickCount64() + 40000;
      auto publish = [&](buffers::Op op, uint64_t generation, uint64_t argument,
                         uint32_t bytes = 0, uint32_t salt = 0, uint64_t patternOffset = 0) {
        buffers::Header h;
        h.op = op; h.payloadBytes = bytes; h.scope = scope; h.sequence = sequence;
        h.resourceId = op == buffers::Op::Finish ? 0 : 1;
        h.generation = generation; h.argument = argument;
        if (mode == L"wrong-map" && sequence == 10) ++h.scope.mapEpoch;
        for (uint32_t i = 0; i < bytes; ++i) payload[i] = bufferlab::Pattern(patternOffset + i, salt);
        size_t encodedBytes = 0;
        Require(buffers::Encode(h, bytes ? payload.data() : nullptr, encoded, encodedBytes) == buffers::Error::None,
                "encode-request");
        for (;;) {
          const auto result = queue->tryPublish(scope, encoded.data(), uint32_t(encodedBytes));
          if (result.mayCommitLocalState()) {
            Require(result.sequence == sequence, "queue-wire-sequence"); ++sequence; break;
          }
          Require(result.outcome == commands::Publish::Full, "producer-admission-failed");
          ++fullRefusals;
          // The first four batches are deliberately queued before the worker
          // starts, exercising non-consuming Full through the actual bridge.
          startWorker.store(true, std::memory_order_release);
          Require(GetTickCount64() < until, "producer-lab-backpressure-deadline");
          Sleep(1);
        }
      };
      auto uploadAll = [&](uint64_t generation, uint32_t size) {
        for (uint32_t offset = 0; offset < size;) {
          const auto count = (size - offset < buffers::MaxUploadBytes) ? size - offset : buffers::MaxUploadBytes;
          publish(buffers::Op::Upload, generation, offset, count, uint32_t(generation), offset);
          offset += count;
        }
      };
      publish(buffers::Op::Create, 1, 131101);
      uploadAll(1, 131101);
      publish(buffers::Op::Verify, 1, 0);
      publish(buffers::Op::Upload, 1, 3, 97, 9); // unaligned partial update, outside original upload source
      publish(buffers::Op::Verify, 1, 0);
      publish(buffers::Op::Destroy, 1, 0);
      publish(buffers::Op::Create, 2, 98307);
      if (mode == L"normal") {
        uploadAll(2, 98307);
        publish(buffers::Op::Verify, 2, 0);
        publish(buffers::Op::Destroy, 2, 0);
        publish(buffers::Op::Finish, 0, 0);
      } else {
        publish(buffers::Op::Upload, mode == L"stale-generation" ? 1 : 2,
                mode == L"bad-range" ? 98307 : 0, 1, 2);
      }
    } catch (...) {
      producerFailure = std::current_exception();
      queue->requestFault();
    }
    queue->closeProducer();
    startWorker.store(true, std::memory_order_release);
    worker.join();
    if (producerFailure) std::rethrow_exception(producerFailure);
    if (workerFailure) std::rethrow_exception(workerFailure);
    const auto c = queue->countersAfterJoin();
    Require(c.published == sequence - 1 && c.dequeued == exchanges && fullRefusals > 0, "queue-bridge-accounting");
    Require(c.cpuAcknowledged == exchanges - (expectedRejected ? 1 : 0) && c.queued == 0,
            "queue-ack-balance");
    Require(expectedRejected ? c.faulted && c.pendingCpuAck : !c.faulted && !c.pendingCpuAck, "fault-settlement");
    Require(!GetModuleHandleW(L"vulkan-1.dll") && !GetModuleHandleW(L"Game.dll"), "x86-loaded-renderer");
    std::printf("{\"schema\":\"warvk-buffer-bridge/v1\",\"ok\":true,\"bits\":32,"
      "\"childPid\":%lu,\"childCreation\":%llu,\"childExit\":%lu,\"peerVerified\":%s,"
      "\"exchanges\":%llu,\"published\":%llu,\"cpuAcknowledged\":%llu,\"fullRefusals\":%llu,"
      "\"expectedRejected\":%s,\"error\":\"%s\",\"accepted\":%llu,\"submitted\":%llu,"
      "\"gpuCompleted\":%llu,\"retired\":%llu,\"liveBytes\":%llu,\"hashCount\":%zu,"
      "\"hashes\":[%llu,%llu,%llu],\"queueBytes\":%zu,\"parentVulkanAbsent\":true,"
      "\"productIntegrated\":false,\"gameTouched\":false,\"memorySavingsProven\":false}\n",
      childPid, childCreation, childExit, peerVerified ? "true" : "false", exchanges, c.published,
      c.cpuAcknowledged, fullRefusals, expectedRejected ? "true" : "false", buffers::ErrorName(last.error),
      last.accepted, last.submitted, last.gpuCompleted, last.retired, last.liveBytes, hashCount,
      hashes[0], hashes[1], hashes[2], sizeof(commands::SpscRenderCommandQueue));
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "buffer bridge failure: %s\n", error.what());
    std::printf("{\"ok\":false,\"childPid\":%lu,\"childCreation\":%llu,\"childExit\":%lu}\n",
                childPid, childCreation, childExit);
    return 1;
  }
}
