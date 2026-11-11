#pragma once
#include "d3d9_wire.h"
#include <d3d9.h>
#include <cstring>
#include <vector>

namespace warvk::host::d9 {
// Subresource bytes are tightly packed on the wire. Native LockRect pitches
// remain local; pointers/COM objects never cross the connection.
constexpr uint32_t MaxTextureDimension=1024,MaxTextureLevels=11;
struct TextureFormat { uint32_t blockWidth=0,blockHeight=0,blockBytes=0; };
inline TextureFormat TextureFormatInfo(uint32_t format) {
  switch(format) {
    case D3DFMT_A8R8G8B8:case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8:case D3DFMT_X8B8G8R8:return {1,1,4};
    case D3DFMT_R5G6B5:case D3DFMT_A1R5G5B5:case D3DFMT_X1R5G5B5:
    case D3DFMT_A4R4G4B4:case D3DFMT_A8L8:return {1,1,2};
    case D3DFMT_A8:case D3DFMT_L8:return {1,1,1};
    case D3DFMT_DXT1:return {4,4,8};
    case D3DFMT_DXT2:case D3DFMT_DXT3:case D3DFMT_DXT4:case D3DFMT_DXT5:return {4,4,16};
    default:return {};
  }
}
struct TextureLevel { uint32_t width=0,height=0,pitch=0,offset=0,bytes=0; };
struct TextureLayout {
  uint32_t width=0,height=0,levels=0,bytes=0;
  uint32_t format=D3DFMT_A8R8G8B8;
  std::array<TextureLevel,MaxTextureLevels> level{};
};
inline bool MakeTextureLayout(uint32_t width,uint32_t height,uint32_t requested,TextureLayout& output,
    uint32_t format=D3DFMT_A8R8G8B8) {
  const auto info=TextureFormatInfo(format);if(!info.blockBytes)return false;
  if(!width||!height||width>MaxTextureDimension||height>MaxTextureDimension)return false;
  uint32_t full=1;for(uint32_t n=std::max(width,height);n>1;n>>=1)++full;
  if(requested>full)return false;
  TextureLayout next;next.width=width;next.height=height;next.levels=requested?requested:full;next.format=format;
  for(uint32_t i=0;i<next.levels;++i) {
    auto& level=next.level[i];level.width=std::max(1u,width>>i);level.height=std::max(1u,height>>i);
    level.pitch=((level.width+info.blockWidth-1)/info.blockWidth)*info.blockBytes;
    level.offset=next.bytes;level.bytes=level.pitch*((level.height+info.blockHeight-1)/info.blockHeight);next.bytes+=level.bytes;
  }
  output=next;return true;
}
struct TextureRegion { uint32_t level=0,x=0,y=0,width=0,height=0,bytes=0; };
inline bool MakeTextureRegion(const TextureLayout& layout,uint32_t level,uint32_t x,uint32_t y,
    uint32_t width,uint32_t height,TextureRegion& output) {
  if(level>=layout.levels||!width||!height)return false;
  const auto& s=layout.level[level];
  if(x>=s.width||y>=s.height||width>s.width-x||height>s.height-y)return false;
  const auto info=TextureFormatInfo(layout.format);if(!info.blockBytes)return false;
  // BC payloads use complete blocks. The last subresource edge may be shorter
  // than one block; a non-edge partial block cannot represent a Vulkan copy.
  if(x%info.blockWidth||y%info.blockHeight
      ||(width%info.blockWidth&&x+width!=s.width)||(height%info.blockHeight&&y+height!=s.height))return false;
  output={level,x,y,width,height,((width+info.blockWidth-1)/info.blockWidth)
    *((height+info.blockHeight-1)/info.blockHeight)*info.blockBytes};return true;
}
inline uint32_t TextureRowBytes(const TextureLayout& layout,uint32_t width) {
  const auto info=TextureFormatInfo(layout.format);
  return info.blockBytes?((width+info.blockWidth-1)/info.blockWidth)*info.blockBytes:0;
}
inline bool MakeManagedLockRegion(const TextureLayout& layout,uint32_t level,uint32_t left,uint32_t top,
    uint32_t right,uint32_t bottom,TextureRegion& output) {
  if(level>=layout.levels||left>=right||top>=bottom)return false;
  const auto& s=layout.level[level];const auto info=TextureFormatInfo(layout.format);
  if(!info.blockBytes||right>s.width||bottom>s.height)return false;
  // Native managed locks may start inside a BC block. The returned pointer
  // identifies that whole block; only the upload domain is expanded to the
  // touched blocks. Default-pool restrictions are not applied to MANAGED.
  const auto x=left/info.blockWidth*info.blockWidth,y=top/info.blockHeight*info.blockHeight;
  right=std::min(s.width,((right+info.blockWidth-1)/info.blockWidth)*info.blockWidth);
  bottom=std::min(s.height,((bottom+info.blockHeight-1)/info.blockHeight)*info.blockHeight);
  return MakeTextureRegion(layout,level,x,y,right-x,bottom-y,output);
}
inline uint32_t TextureByteOffset(const TextureLayout& layout,uint32_t level,uint32_t x,uint32_t y) {
  const auto info=TextureFormatInfo(layout.format);const auto& s=layout.level[level];
  return s.offset+(y/info.blockHeight)*s.pitch+(x/info.blockWidth)*info.blockBytes;
}
inline bool FullTextureLevel(const TextureLayout& layout,const TextureRegion& region) {
  return region.level<layout.levels&&!region.x&&!region.y
    &&region.width==layout.level[region.level].width&&region.height==layout.level[region.level].height;
}
// Writes a tightly packed, already validated region into a tightly packed copy
// of the whole layout (the host's retained managed content). Neither side uses
// a caller pitch; rows are the layout's own block rows.
inline void PatchTextureLayout(const TextureLayout& layout,const TextureRegion& region,const uint8_t* packed,uint8_t* target) {
  const auto info=TextureFormatInfo(layout.format);const auto& s=layout.level[region.level];
  const auto rowBytes=TextureRowBytes(layout,region.width),rows=(region.height+info.blockHeight-1)/info.blockHeight;
  const auto origin=TextureByteOffset(layout,region.level,region.x,region.y);
  for(uint32_t row=0;row<rows;++row)std::memcpy(target+origin+row*s.pitch,packed+row*rowBytes,rowBytes);
}
// One ReadContent reply: at most this many bytes, inside the reply packet.
constexpr uint32_t MaxContentRead=MaxPayload;
static_assert(MaxContentRead<=MaxBytes-ReplyBytes,"content read must fit one reply");
// One bounded, owned transaction per host connection. GPU state is untouched
// until the exact contiguous payload is complete. Rejected chunks change
// neither bytes nor cursor; abort/destroy drops the uncommitted patch.
class TextureUpload final {
public:
  bool begin(ResourceRef ref,uint32_t token,const TextureRegion& region) {
    if(active()||!ref.id||!ref.generation||!token||!region.bytes
        ||region.bytes>MaxTextureDimension*MaxTextureDimension*4)return false;
    std::vector<uint8_t> next(region.bytes);
    m_bytes.swap(next);m_ref=ref;m_token=token;m_region=region;m_written=0;return true;
  }
  bool matches(ResourceRef ref,uint32_t token) const {
    return active()&&ref.id==m_ref.id&&ref.generation==m_ref.generation&&token==m_token;
  }
  bool append(ResourceRef ref,uint32_t token,uint32_t offset,const uint8_t* data,uint32_t bytes) {
    if(!matches(ref,token)||offset!=m_written||!data||!bytes||bytes>MaxPayload||bytes>m_bytes.size()-m_written)return false;
    std::memcpy(m_bytes.data()+m_written,data,bytes);m_written+=bytes;return true;
  }
  bool complete(ResourceRef ref,uint32_t token) const { return matches(ref,token)&&m_written==m_bytes.size(); }
  bool active() const { return m_token!=0; }
  bool owns(ResourceRef ref) const {return active()&&ref.id==m_ref.id&&ref.generation==m_ref.generation;}
  const TextureRegion& region() const {return m_region;}
  const uint8_t* data() const {return m_bytes.data();}
  void reset() {std::vector<uint8_t>().swap(m_bytes);m_ref={};m_token=0;m_written=0;m_region={};}
private:
  ResourceRef m_ref{};uint32_t m_token=0,m_written=0;TextureRegion m_region{};
  std::vector<uint8_t> m_bytes;
};
}
