#include "slot_channel.h"
#include "shared_slots.h"
#include "slot_host_runtime.h"
#include <cstdio>
#include <cstring>
#include <memory>

using namespace warvk::host;
static_assert(sizeof(void*) == 8, "RH1 requires an actual 64-bit consumer");
namespace {
struct Rejected { const char* reason; };
void Require(bool valid, const char* reason) { if (!valid) throw Rejected{reason}; }
void Ledger(slots::Error e) { Require(e == slots::Error::None, slots::ErrorName(e)); }
void Wire(slotwire::Error e) { Require(e == slotwire::Error::None, slotwire::ErrorName(e)); }
uint64_t Number(const wchar_t* p) {
  Require(p && *p, "Argument");
  uint64_t n = 0;
  for (; *p; ++p) {
    Require(*p >= L'0' && *p <= L'9', "Argument");
    auto digit = uint64_t(*p - L'0');
    Require(n <= (UINT64_MAX - digit) / 10, "Argument");
    n = n * 10 + digit;
  }
  return n;
}
void SampleReceipt(const slots::Key& key, const lab::Digest& hash) {
  std::printf("{\"sample\":true,\"map\":%llu,\"device\":%llu,\"frame\":%llu,"
    "\"generation\":%llu,\"slot\":%u,\"bytes\":%u,\"sha256\":\"",
    (unsigned long long)key.map, (unsigned long long)key.device, (unsigned long long)key.frame,
    (unsigned long long)key.generation, key.slot, key.bytes);
  for (auto byte : hash) std::printf("%02x", unsigned(byte));
  std::printf("\"}\n");
}
}

// CPU laboratory adapter. SlotLedger alone owns epochs/leases; this loop owns
// only wire order and I/O. No callback or shared view escapes the copy operation.
int warvk::host::lab::RunSlotHost(int argc, wchar_t** argv, SlotPayloadConsumer* consumer) {
  lab::IoStats io;
  lab::SlotIoStats copies;
  DWORD protection = 0, systemError = 0;
  bool verified = false, readersDone = true;
  uint64_t accepted = 0;
  const char* state = "fault";
  const char* reason = "None";
  const char* fault = "none";
  const char* stage = "none";
  int result = 2;
  try {
    Require(argc == 4, "Argument");
    const auto capabilities = consumer ? consumer->capabilities() : slotwire::SharedCpuSlots;
    Require(capabilities == slotwire::SharedCpuSlots ||
      capabilities == (slotwire::SharedCpuSlots | slotwire::EnvelopeSamples) ||
      capabilities == (slotwire::SharedCpuSlots | slotwire::RecorderEvents), "ConsumerCapabilities");
    const auto nonce = lab::ParseNonce(argv[1]);
    const auto parentId = Number(argv[2]);
    Require(parentId && parentId <= MAXDWORD, "Argument");
    auto parent = lab::OpenPeer(DWORD(parentId), Number(argv[3]));
    auto pipe = lab::CreateServer(nonce, lab::Channel::Slots1);
    lab::Accept(pipe.get(), parent.get(), io);
    lab::VerifyPeer(pipe.get(), true, DWORD(parentId));
    verified = true;
    slots::SlotLedger ledger(nonce);
    std::unique_ptr<lab::SharedSlots> mapping;
    try {
      uint64_t next = 1;
      bool greeted = false;
      for (;;) {
        slotwire::Packet input{}, output{};
        size_t size = 0;
        Wire(slotwire::ReadPacket(pipe.get(), parent.get(), input, size, io));
        slotwire::View view;
        Wire(slotwire::Decode(input.data(), size, view));
        auto header = view.header;
        Require(header.flags == 0, "RequestFlags");
        Require(header.nonce == nonce, "Nonce");
        Require(header.sequence == next && next != UINT64_MAX, "Sequence");
        Require(greeted || header.op == slotwire::Op::Hello, "HelloRequired");
        std::array<uint8_t, slotwire::MaxPayloadBytes> reply{};
        size_t replyBytes = 0;
        auto shape = [&](size_t expected) { Require(header.payloadBytes == expected, "Shape"); };
        auto echo = [&] {
          replyBytes = header.payloadBytes;
          if (replyBytes) std::memcpy(reply.data(), view.payload, replyBytes);
        };
        bool terminal = false;
        switch (header.op) {
          case slotwire::Op::Hello: {
            Require(!greeted, "RepeatedHello"); shape(24);
            const auto expected = slotwire::Hello(32, 64, capabilities);
            Require(!std::memcmp(view.payload, expected.data(), expected.size()), "HelloContract");
            mapping = std::make_unique<lab::SharedSlots>(nonce, lab::MappingRole::Reader);
            protection = mapping->protection();
            const auto actual = slotwire::Hello(64, 32, capabilities);
            std::memcpy(reply.data(), actual.data(), actual.size()); replyBytes = actual.size();
            greeted = true;
          } break;
          case slotwire::Op::Begin:
            shape(16);
            Ledger(ledger.beginEpoch(ipc::ReadLe(view.payload, 8), ipc::ReadLe(view.payload + 8, 8)));
            if (consumer) {
              const auto error = consumer->begin(nonce, ipc::ReadLe(view.payload, 8), ipc::ReadLe(view.payload + 8, 8));
              Require(!error, error);
            }
            echo(); break;
          case slotwire::Op::Reserve: {
            shape(16); Require(ipc::ReadLe(view.payload + 12, 4) == 0, "ReserveReserved");
            slots::Key key;
            Ledger(ledger.reserve(ipc::ReadLe(view.payload, 8), uint32_t(ipc::ReadLe(view.payload + 8, 4)), key));
            Ledger(slots::EncodeKey(key, reply.data(), reply.size())); replyBytes = slots::DescriptorBytes;
          } break;
          case slotwire::Op::Publish: {
            shape(96); slots::Key key;
            Ledger(slots::DecodeKey(view.payload, slots::DescriptorBytes, key));
            Ledger(ledger.publish(key));
            slots::ReadPermit permit;
            Ledger(ledger.beginRead(key, permit));
            lab::Digest digest{};
            try {
              lab::Sample sample{};
              mapping->copyPrivate(permit, sample, parent.get());
              digest = lab::Sha256(sample.data(), key.bytes);
              Require(!std::memcmp(digest.data(), view.payload + slots::DescriptorBytes, digest.size()), "Digest");
              if (consumer) {
                const auto error = consumer->consume(permit, sample.data(), key.bytes);
                Require(!error, error);
              }
              Ledger(ledger.finishRead(permit));
            } catch (...) {
              ledger.retire(); // Uncertain consumption never makes the slot Free.
              if (permit.active()) Ledger(ledger.finishRead(permit));
              throw;
            }
            echo(); std::memcpy(reply.data() + slots::DescriptorBytes, digest.data(), digest.size());
            SampleReceipt(key, digest);
          } break;
          case slotwire::Op::Cancel: {
            shape(slots::DescriptorBytes); slots::Key key;
            Ledger(slots::DecodeKey(view.payload, slots::DescriptorBytes, key));
            Ledger(ledger.cancelWrite(key)); echo();
          } break;
          case slotwire::Op::End:
            shape(0); Ledger(ledger.endEpoch());
            if(consumer) {const auto error=consumer->end();Require(!error,error);}
            break;
          case slotwire::Op::Close: {
            shape(0);
            const auto phase = ledger.snapshot().phase;
            Require(phase == slots::Phase::Dormant || phase == slots::Phase::Ready, "ClosePhase");
            Require(ledger.localReadersDone(), "LocalReaders");
            if(consumer) {const auto error=consumer->close();Require(!error,error);}
            terminal = true;
          } break;
          case slotwire::Op::Retire:
            shape(0); ledger.retire();
            Require(ledger.localReadersDone(), "LocalReaders"); terminal = true; break;
        }
        ++accepted; ++next;
        header.flags = 1; header.payloadBytes = uint32_t(replyBytes);
        Wire(slotwire::Encode(header, reply.data(), replyBytes, output.data(), output.size(), size));
        lab::WriteBytes(pipe.get(), parent.get(), output.data(), size, io);
        if (terminal) {
          state = header.op == slotwire::Op::Close ? "closed" : "retired";
          result = 0; break; // Only after actual reply I/O completion.
        }
      }
    } catch (...) {
      ledger.retire(); readersDone = ledger.localReadersDone();
      if (mapping) copies = mapping->stats();
      throw; // All local mapping/handle owners unwind before final receipt.
    }
    readersDone = ledger.localReadersDone();
    if (mapping) copies = mapping->stats();
  } catch (const Rejected& e) { reason = e.reason;
  } catch (const lab::Failure& e) {
    fault = lab::FaultName(e.fault); stage = lab::StageName(e.stage); systemError = e.win32;
  } catch (...) { fault = "unexpected-exception"; }
  std::printf("{\"summary\":true,\"bits\":64,\"peerVerified\":%s,\"state\":\"%s\","
    "\"reason\":\"%s\",\"fault\":\"%s\",\"stage\":\"%s\",\"win32\":%lu,"
    "\"accepted\":%llu,\"readersDone\":%s,\"viewProtection\":%lu,\"poolBytes\":%u,"
    "\"copies\":%llu,\"copiedBytes\":%llu,\"abandoned\":%llu,\"mutexTimeouts\":%llu,"
    "\"reads\":%llu,\"writes\":%llu,\"cancelRequests\":%llu,\"cancelCompletions\":%llu}\n",
    verified ? "true" : "false", state, reason, fault, stage, (unsigned long)systemError,
    (unsigned long long)accepted, readersDone ? "true" : "false", (unsigned long)protection,
    slots::PoolBytes, (unsigned long long)copies.copies, (unsigned long long)copies.copiedBytes,
    (unsigned long long)copies.abandoned, (unsigned long long)copies.mutexTimeouts,
    (unsigned long long)io.reads, (unsigned long long)io.writes,
    (unsigned long long)io.cancelRequests, (unsigned long long)io.cancelCompletions);
  std::fflush(stdout);
  return result;
}
