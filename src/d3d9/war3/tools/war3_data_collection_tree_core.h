#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace dxvk::war3::collection {

enum class Tag : uint16_t {
  Root, SceneCollect, VisiblePublish, VisibleFinalize, VisibleQuery,
  ModelBind, ModelQuery, InstanceWrite, InstanceQuery, PoseWrite, PoseQuery,
  GeosetCapture, GeosetBind, GeosetQuery, GeosetMaterialize,
  CurrentDrawPublish, CurrentDrawQuery, PaletteCapture, PaletteDecode,
  ShadowObjectWrite, ShadowObjectQuery, SemanticAugment, SemanticBuild,
  DrawCapture, MetadataCapture, Populate, DirectGrouped, SnapshotPublish,
  RegistryMaintenance, ModelObservation, GeometryFreeze, SnapshotCopy,
  ArenaCommit, CommandEnqueue, Count
};

inline const char* TagName(Tag tag) noexcept {
  constexpr const char* names[] = {"DataCollection", "SceneCollect", "VisiblePublish",
    "VisibleFinalize", "VisibleQuery", "ModelBind", "ModelQuery", "InstanceWrite",
    "InstanceQuery", "PoseWrite", "PoseQuery", "GeosetCapture", "GeosetBind",
    "GeosetQuery", "GeosetMaterialize", "CurrentDrawPublish", "CurrentDrawQuery",
    "PaletteCapture", "PaletteDecode", "ShadowObjectWrite", "ShadowObjectQuery",
    "SemanticAugment", "SemanticBuild", "DrawCapture", "MetadataCapture", "Populate",
    "DirectGrouped", "SnapshotPublish", "RegistryMaintenance", "ModelObservation",
    "GeometryFreeze", "SnapshotCopy", "ArenaCommit", "CommandEnqueue"};
  static_assert(sizeof(names) / sizeof(names[0]) == size_t(Tag::Count));
  return uint16_t(tag) < uint16_t(Tag::Count) ? names[uint16_t(tag)] : "Invalid";
}

struct Node {
  uint16_t parent = 0;
  Tag tag = Tag::Root;
  uint64_t ticks = 0, childTicks = 0, calls = 0;
};

struct TreeSnapshot {
  static constexpr uint16_t Capacity = 512;
  std::array<Node, Capacity> nodes = {};
  uint16_t count = 1;
  bool faulted = false;
  uint64_t faults = 0;
};

// One owner thread, no allocation/locks/strings/hash table. A complete sampled
// outer invocation is the unit: all its descendants are included together.
class Tree {
public:
  uint64_t enter(Tag tag, uint64_t now) noexcept {
    if (m_data.faulted) return 0;
    if (tag == Tag::Root || tag >= Tag::Count || m_depth == m_stack.size() ||
        m_serial == UINT64_MAX) { fault(); return 0; }
    const uint16_t parent = m_depth ? m_stack[m_depth - 1].node : 0;
    uint16_t& index = m_edges[parent][uint16_t(tag)];
    if (!index) {
      if (m_data.count == TreeSnapshot::Capacity) { fault(); return 0; }
      index = m_data.count++;
      m_data.nodes[index].parent = parent;
      m_data.nodes[index].tag = tag;
    }
    const uint64_t token = ++m_serial;
    m_stack[m_depth++] = {index, token, now, 0};
    return token;
  }

  bool leave(uint64_t token, uint64_t now) noexcept {
    if (m_data.faulted) return false;
    if (!m_depth || !token || m_stack[m_depth - 1].token != token) { fault(); return false; }
    const auto frame = m_stack[--m_depth];
    if (now < frame.start || now - frame.start < frame.children) { fault(); return false; }
    const uint64_t elapsed = now - frame.start;
    auto& node = m_data.nodes[frame.node];
    if (!add(node.ticks, elapsed) || !add(node.childTicks, frame.children) || !add(node.calls, 1)) return false;
    if (m_depth) return add(m_stack[m_depth - 1].children, elapsed);
    auto& root = m_data.nodes[0];
    return add(root.ticks, elapsed) && add(root.childTicks, elapsed) && add(root.calls, 1);
  }

  void reset() noexcept { *this = Tree(); }
  void fault() noexcept { m_data.faulted = true; if (m_data.faults != UINT64_MAX) ++m_data.faults; }
  const TreeSnapshot& snapshot() const noexcept { return m_data; }
  uint16_t depth() const noexcept { return m_depth; }
  static bool closed(const TreeSnapshot& s) noexcept {
    if (s.faulted || !s.count || s.count > TreeSnapshot::Capacity) return false;
    std::array<uint64_t, TreeSnapshot::Capacity> childSum = {};
    for (uint16_t i = 1; i < s.count; ++i) {
      const auto& n = s.nodes[i];
      if (n.parent >= i || n.childTicks > n.ticks || UINT64_MAX - childSum[n.parent] < n.ticks) return false;
      childSum[n.parent] += n.ticks;
    }
    for (uint16_t i = 0; i < s.count; ++i)
      if (childSum[i] != s.nodes[i].childTicks || s.nodes[i].childTicks > s.nodes[i].ticks) return false;
    return s.nodes[0].ticks == s.nodes[0].childTicks;
  }
private:
  bool add(uint64_t& to, uint64_t value) noexcept {
    if (value > UINT64_MAX - to) { fault(); return false; }
    to += value; return true;
  }
  struct Frame { uint16_t node; uint64_t token, start, children; };
  TreeSnapshot m_data;
  std::array<std::array<uint16_t, size_t(Tag::Count)>, TreeSnapshot::Capacity> m_edges = {};
  std::array<Frame, 64> m_stack = {};
  uint16_t m_depth = 0;
  uint64_t m_serial = 0;
};
} // namespace dxvk::war3::collection
