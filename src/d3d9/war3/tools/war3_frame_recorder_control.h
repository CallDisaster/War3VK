#pragma once
#include <cstdint>

namespace dxvk::war3::tools::evidence {
class RecorderControlAuthority;

// A stable, once-issued CPU control credential, not a ring/GPU completion token.
// Borrow only while the owning FrameHistory and its joined worker remain alive.
class RecorderControlLease final {
public:
  RecorderControlLease() noexcept = default;
  RecorderControlLease(const RecorderControlLease&) = delete;
  RecorderControlLease& operator=(const RecorderControlLease&) = delete;
  RecorderControlLease(RecorderControlLease&&) = delete;
  RecorderControlLease& operator=(RecorderControlLease&&) = delete;
private:
  friend class RecorderControlAuthority;
  const RecorderControlAuthority* authority = nullptr;
  uint64_t generation = 0;
  bool issued = false;
};

// All calls use the existing controlMutex. Authority must outlive its leases;
// no callbacks, allocations, atomics or destructor-driven ownership transfer.
class RecorderControlAuthority final {
public:
  explicit RecorderControlAuthority(uint64_t generationLimit = UINT64_MAX) noexcept
    : limit(generationLimit) { }
  RecorderControlAuthority(const RecorderControlAuthority&) = delete;
  RecorderControlAuthority& operator=(const RecorderControlAuthority&) = delete;
  RecorderControlAuthority(RecorderControlAuthority&&) = delete;
  RecorderControlAuthority& operator=(RecorderControlAuthority&&) = delete;

  bool claim(RecorderControlLease& out, bool ringIdle) noexcept {
    if (active || !ringIdle || out.issued || generation == limit) return false;
    out.authority = this;
    out.generation = ++generation;
    out.issued = true;
    active = &out;
    return true;
  }
  bool owns(const RecorderControlLease& lease) const noexcept {
    return active == &lease && lease.authority == this &&
      lease.generation != 0 && lease.generation == generation;
  }
  bool release(RecorderControlLease& lease) noexcept {
    if (!owns(lease)) return false;
    active = nullptr;
    lease.authority = nullptr;
    lease.generation = 0;
    // Keep issued set: a released object is never a new owner's credential.
    return true;
  }
  bool occupied() const noexcept { return active != nullptr; }
  bool allows(const RecorderControlLease* caller, bool readOnly) const noexcept {
    return caller ? owns(*caller) : (!occupied() || readOnly);
  }
private:
  RecorderControlLease* active = nullptr;
  uint64_t generation = 0;
  const uint64_t limit;
};
}
