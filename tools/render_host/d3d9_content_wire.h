#pragma once
#include "d3d9_annotation_wire.h"
#include <unordered_map>

// Content payloads (enhancement contract item 4), part of the negotiated
// Annotations feature. The x86 data layer uploads a blob once under its own
// 64-bit key; draw records then name {key, revision} instead of carrying the
// bytes. The host holds the only copy: x86 keeps this index, never the content.
// - ContentUpload: args kind, key low/high, revision low/high, total bytes,
//   offset; the payload is one chunk. Chunks of one blob are consecutive
//   messages with contiguous offsets from 0, and the last one commits it. Any
//   other message while a blob is open is refused and drops the open blob.
// - Revisions ascend strictly over the whole connection, so an upload under a
//   live key replaces it and a stale {key, revision} can never bind again.
// - ContentRelease: args key low/high, revision low/high.
// - ContentRead: the same, then offset and count; the reply carries the bytes.
// - GeometryRef / BonesRef draw records are a ContentRef that must name a live
//   blob of the matching kind when their draw message arrives.
// - SessionReset drops every blob: content identities belong to one map.
namespace warvk::host::d9 {
enum class ContentKind : uint32_t { Geometry=1, BoneMatrices=2 };
constexpr uint32_t MaxContentBlobBytes=4u<<20,MaxContentBlobs=16384;
constexpr uint64_t MaxContentStoreBytes=256ull<<20;
struct ContentRef { uint64_t key=0,revision=0; };
struct ContentChunk { uint32_t kind=0;uint64_t key=0,revision=0;uint32_t total=0,offset=0,count=0; };
static_assert(MaxContentBlobBytes%4==0&&MaxPayload%4==0,"whole-word kinds split on word boundaries");

inline bool ValidContentKind(uint32_t kind) {
  return kind==uint32_t(ContentKind::Geometry)||kind==uint32_t(ContentKind::BoneMatrices);
}
// The blob kind a reference record must name, or 0 for any other schema.
inline uint32_t ContentKindOf(uint32_t schema) {
  return schema==uint32_t(AnnotationSchema::GeometryRef)?uint32_t(ContentKind::Geometry)
    :schema==uint32_t(AnnotationSchema::BonesRef)?uint32_t(ContentKind::BoneMatrices):0;
}
// Provisional v1 vocabulary: geometry is opaque to the transport, and its
// layout belongs to the consumer. Bone matrices are whole finite floats.
inline bool ValidContentPayload(uint32_t kind,uint32_t offset,const uint8_t* p,uint32_t bytes) {
  if(!p||!bytes)return false;
  if(kind!=uint32_t(ContentKind::BoneMatrices))return true;
  if((offset|bytes)&3)return false;
  for(uint32_t i=0;i<bytes;i+=4)if(!std::isfinite(Float(U32(p+i))))return false;
  return true;
}
inline std::array<uint8_t,ContentRefBytes> EncodeContentRef(const ContentRef& ref) {
  std::array<uint8_t,ContentRefBytes> p{};P64(p.data(),ref.key);P64(p.data()+8,ref.revision);return p;
}
inline ContentRef DecodeContentRef(const uint8_t* p) { return {U64(p),U64(p+8)}; }
inline void ContentIdentity(Header& h,uint32_t at,uint64_t key,uint64_t revision) {
  h.args[at]=uint32_t(key);h.args[at+1]=uint32_t(key>>32);h.args[at+2]=uint32_t(revision);h.args[at+3]=uint32_t(revision>>32);
}
inline bool UnusedArgs(const Header& h,size_t from) {
  for(;from<h.args.size();++from)if(h.args[from])return false;
  return true;
}
inline Header EncodeContentUpload(const ContentChunk& c) {
  Header h;h.op=Op::ContentUpload;h.args[0]=c.kind;ContentIdentity(h,1,c.key,c.revision);
  h.args[5]=c.total;h.args[6]=c.offset;h.payloadBytes=c.count;return h;
}
// Shape and payload only; whether the chunk is the next one is the index's rule.
inline bool DecodeContentUpload(const Header& h,const uint8_t* p,ContentChunk& out) {
  if(h.op!=Op::ContentUpload||h.id||h.generation||!UnusedArgs(h,7)||!h.payloadBytes||h.payloadBytes>MaxPayload)return false;
  const ContentChunk c{h.args[0],uint64_t(h.args[1])|(uint64_t(h.args[2])<<32),uint64_t(h.args[3])|(uint64_t(h.args[4])<<32),
    h.args[5],h.args[6],h.payloadBytes};
  if(!ValidContentKind(c.kind)||!c.key||!c.revision||!c.total||c.total>MaxContentBlobBytes
    ||c.offset>=c.total||c.count>c.total-c.offset||!ValidContentPayload(c.kind,c.offset,p,c.count))return false;
  out=c;return true;
}
inline Header EncodeContentRelease(const ContentRef& ref) {
  Header h;h.op=Op::ContentRelease;ContentIdentity(h,0,ref.key,ref.revision);return h;
}
inline bool DecodeContentRelease(const Header& h,ContentRef& out) {
  if(h.op!=Op::ContentRelease||h.id||h.generation||h.payloadBytes||!UnusedArgs(h,4))return false;
  const ContentRef ref{uint64_t(h.args[0])|(uint64_t(h.args[1])<<32),uint64_t(h.args[2])|(uint64_t(h.args[3])<<32)};
  if(!ref.key||!ref.revision)return false;
  out=ref;return true;
}
inline Header EncodeContentRead(const ContentRef& ref,uint32_t offset,uint32_t count) {
  Header h;h.op=Op::ContentRead;ContentIdentity(h,0,ref.key,ref.revision);h.args[4]=offset;h.args[5]=count;return h;
}
inline bool DecodeContentRead(const Header& h,ContentRef& out,uint32_t& offset,uint32_t& count) {
  if(h.op!=Op::ContentRead||h.id||h.generation||h.payloadBytes||!UnusedArgs(h,6))return false;
  const ContentRef ref{uint64_t(h.args[0])|(uint64_t(h.args[1])<<32),uint64_t(h.args[2])|(uint64_t(h.args[3])<<32)};
  if(!ref.key||!ref.revision||!h.args[5]||h.args[5]>MaxPayload)return false;
  out=ref;offset=h.args[4];count=h.args[5];return true;
}
// Admission rules, run identically by both ends: x86 refuses locally what the
// host would refuse, so a host refusal of admitted content is a protocol fault.
class ContentIndex {
public:
  struct Meta { uint64_t revision=0;uint32_t bytes=0,kind=0; };
  enum class Admit { Refused, More, Complete };
  // Whether a new blob of `total` bytes may open under `key` now.
  bool canOpen(uint64_t key,uint32_t total) const {
    if(uploading||!key||!total||total>MaxContentBlobBytes||last==UINT64_MAX)return false;
    const auto it=blobs.find(key);
    if(it==blobs.end())return blobs.size()<MaxContentBlobs&&storedBytes+total<=MaxContentStoreBytes;
    return storedBytes-it->second.bytes+total<=MaxContentStoreBytes;
  }
  // The first chunk opens a blob and consumes its revision; the last commits.
  // A chunk that is not the open blob's next one is refused and drops it.
  Admit admit(const ContentChunk& c) {
    if(!uploading) {
      if(c.offset||c.revision<=last||!ValidContentKind(c.kind)||!c.count||c.count>c.total||!canOpen(c.key,c.total))
        return Admit::Refused;
      open=c;open.count=0;uploading=true;last=c.revision;
    } else if(c.kind!=open.kind||c.key!=open.key||c.revision!=open.revision||c.total!=open.total
        ||c.offset!=open.count||!c.count||c.count>open.total-open.count) {
      abort();return Admit::Refused;
    }
    open.count+=c.count;
    if(open.count<open.total)return Admit::More;
    auto& meta=blobs[open.key];
    if(meta.revision){storedBytes-=meta.bytes;++replacedBlobs;}
    meta={open.revision,open.total,open.kind};storedBytes+=open.total;uploading=false;++committedBlobs;
    peakBytes=std::max(peakBytes,storedBytes);peakBlobs=std::max<uint64_t>(peakBlobs,blobs.size());
    return Admit::Complete;
  }
  bool abort() {
    if(!uploading)return false;
    uploading=false;++abortedBlobs;return true;
  }
  bool active() const { return uploading; }
  const ContentChunk& pending() const { return open; }
  const Meta* find(const ContentRef& ref) const {
    const auto it=blobs.find(ref.key);
    return it!=blobs.end()&&it->second.revision==ref.revision&&ref.revision?&it->second:nullptr;
  }
  bool bound(const ContentRef& ref,uint32_t kind) const {
    const auto* meta=find(ref);return !uploading&&meta&&meta->kind==kind;
  }
  bool release(const ContentRef& ref) {
    if(uploading||!find(ref))return false;
    const auto it=blobs.find(ref.key);storedBytes-=it->second.bytes;blobs.erase(it);++releasedBlobs;return true;
  }
  // SessionReset: every blob goes; revisions keep ascending. No blob is open
  // here: a reset is itself "any other message" and was refused if one was.
  void clear() { clearedBlobs+=blobs.size();blobs.clear();storedBytes=0;uploading=false; }
  uint64_t lastRevision() const { return last; }
  uint64_t bytes() const { return storedBytes; }
  size_t count() const { return blobs.size(); }
  uint64_t committedBlobs=0,replacedBlobs=0,releasedBlobs=0,abortedBlobs=0,clearedBlobs=0,peakBytes=0,peakBlobs=0;
private:
  std::unordered_map<uint64_t,Meta> blobs;
  ContentChunk open;
  uint64_t storedBytes=0,last=0;
  bool uploading=false;
};
} // namespace warvk::host::d9
