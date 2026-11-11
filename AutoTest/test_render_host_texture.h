#pragma once
#include "../tools/render_host/d3d9_texture_wire.h"
namespace warvk_texture_test {
template<class Check> void Run(Check check) {
  namespace d9=warvk::host::d9;
  d9::TextureLayout layout;
  for(uint32_t w:{1u,2u,3u,7u,64u,129u,257u,511u,1024u})for(uint32_t h:{1u,3u,65u,129u,1024u}) {
    check(d9::MakeTextureLayout(w,h,0,layout));
    uint64_t total=0;uint32_t levels=0,x=w,y=h;
    for(;;) {
      const auto& level=layout.level[levels];
      check(level.width==x&&level.height==y&&level.pitch==x*4&&level.bytes==uint64_t(x)*y*4&&level.offset==total);
      total+=uint64_t(x)*y*4;++levels;if(x==1&&y==1)break;x=std::max(1u,x/2);y=std::max(1u,y/2);
    }
    check(layout.levels==levels&&layout.bytes==total);
    const auto before=layout;
    check(!d9::MakeTextureLayout(w,h,levels+1,layout));check(std::memcmp(&layout,&before,sizeof(layout))==0);
  }
  for(uint32_t invalid:{0u,1025u,UINT32_MAX}){check(!d9::MakeTextureLayout(invalid,1,0,layout));check(!d9::MakeTextureLayout(1,invalid,0,layout));}
  check(d9::MakeTextureLayout(257,129,0,layout));
  d9::TextureRegion region;
  check(d9::MakeTextureRegion(layout,0,0,0,257,129,region));
  const auto before=region;
  check(!d9::MakeTextureRegion(layout,11,0,0,1,1,region));
  check(!d9::MakeTextureRegion(layout,0,UINT32_MAX,0,1,1,region));
  check(!d9::MakeTextureRegion(layout,0,0,0,UINT32_MAX,1,region));
  check(std::memcmp(&region,&before,sizeof(region))==0);
  std::vector<uint8_t> source(region.bytes);for(size_t i=0;i<source.size();++i)source[i]=uint8_t((i*71+i/257)^0xa3);
  d9::TextureUpload upload;
  const d9::ResourceRef identity{7,3};check(!upload.active());check(upload.begin(identity,1,region));
  check(!upload.begin(identity,2,region));check(!upload.complete(identity,1));
  uint32_t offset=0;
  while(offset<source.size()) {
    const uint32_t count=std::min<uint32_t>(d9::MaxPayload,uint32_t(source.size())-offset);
    check(!upload.append({7,2},1,offset,source.data()+offset,count));
    check(!upload.append(identity,2,offset,source.data()+offset,count));
    check(!upload.append(identity,1,offset+1,source.data()+offset,count));
    check(!upload.append(identity,1,offset,nullptr,count));
    check(!upload.append(identity,1,offset,source.data()+offset,0));
    check(upload.append(identity,1,offset,source.data()+offset,count));
    offset+=count;
    check(std::memcmp(upload.data(),source.data(),offset)==0);
    check(upload.complete(identity,1)==(offset==source.size()));
  }
  check(!upload.append(identity,1,offset,source.data(),1));
  check(upload.complete(identity,1));upload.reset();check(!upload.active());
  check(!upload.complete(identity,1));check(upload.begin({7,4},2,region));
  check(!upload.matches(identity,2));upload.reset();
  // Independent bit/block-size table; no production format helper is used
  // to derive expected pitches or offsets.
  struct Format {uint32_t format,block,bytes;};
  const Format formats[]={
    {D3DFMT_A8R8G8B8,1,4},{D3DFMT_X8R8G8B8,1,4},{D3DFMT_A8B8G8R8,1,4},{D3DFMT_X8B8G8R8,1,4},
    {D3DFMT_R5G6B5,1,2},{D3DFMT_A1R5G5B5,1,2},{D3DFMT_X1R5G5B5,1,2},{D3DFMT_A4R4G4B4,1,2},
    {D3DFMT_A8L8,1,2},{D3DFMT_A8,1,1},{D3DFMT_L8,1,1},
    {D3DFMT_DXT1,4,8},{D3DFMT_DXT2,4,16},{D3DFMT_DXT3,4,16},{D3DFMT_DXT4,4,16},{D3DFMT_DXT5,4,16}};
  for(const auto f:formats)for(uint32_t w:{1u,2u,3u,4u,7u,64u,129u,257u,1024u})for(uint32_t h:{1u,3u,65u,1024u}) {
    check(d9::MakeTextureLayout(w,h,0,layout,f.format));check(layout.format==f.format);
    uint32_t total=0;
    for(uint32_t i=0;i<layout.levels;++i) {
      const auto x=std::max(1u,w>>i),y=std::max(1u,h>>i);
      const auto rows=(y+f.block-1)/f.block,cols=(x+f.block-1)/f.block;
      const auto& level=layout.level[i];
      check(level.width==x&&level.height==y&&level.pitch==cols*f.bytes&&level.bytes==rows*cols*f.bytes&&level.offset==total);
      total+=rows*cols*f.bytes;
      check(d9::MakeTextureRegion(layout,i,0,0,x,y,region)&&region.bytes==level.bytes);
      check(d9::TextureByteOffset(layout,i,0,0)==level.offset);
      check(d9::TextureRowBytes(layout,x)==cols*f.bytes);
      if(x>4&&y>4) {
        check(d9::MakeTextureRegion(layout,i,4,4,x-4,y-4,region));
        check(d9::TextureByteOffset(layout,i,4,4)==level.offset+(4/f.block)*level.pitch+(4/f.block)*f.bytes);
        if(f.block==4) {
          const auto saved=region;
          check(!d9::MakeTextureRegion(layout,i,1,0,4,4,region));
          check(!d9::MakeTextureRegion(layout,i,0,1,4,4,region));
          check(!d9::MakeTextureRegion(layout,i,0,0,3,4,region));
          check(!d9::MakeTextureRegion(layout,i,0,0,4,3,region));
          check(std::memcmp(&region,&saved,sizeof(region))==0);
        }
      }
    }check(layout.bytes==total);
  }
  for(uint32_t f:{0u,uint32_t(D3DFMT_R8G8B8),uint32_t(D3DFMT_D24S8),UINT32_MAX}) {
    const auto saved=layout;check(!d9::MakeTextureLayout(64,64,1,layout,f));check(std::memcmp(&layout,&saved,sizeof(layout))==0);
  }
  check(d9::MakeTextureLayout(65,33,1,layout,D3DFMT_DXT1));
  check(d9::MakeManagedLockRegion(layout,0,5,5,11,11,region));
  check(region.x==4&&region.y==4&&region.width==8&&region.height==8&&region.bytes==32);
  check(d9::MakeManagedLockRegion(layout,0,64,32,65,33,region));
  check(region.x==64&&region.y==32&&region.width==1&&region.height==1&&region.bytes==8);
  const auto saved=region;check(!d9::MakeManagedLockRegion(layout,0,0,0,66,33,region));
  check(std::memcmp(&saved,&region,sizeof(region))==0);
  // Retained host copy: a packed region lands at the layout's own block rows.
  // Expected offsets come from the independent format table above.
  for(const auto f:{formats[0],formats[11],formats[15]}) {
    check(d9::MakeTextureLayout(65,33,0,layout,f.format));
    std::vector<uint8_t> target(layout.bytes,0xee),expected(layout.bytes,0xee);
    for(uint32_t level:{0u,1u,layout.levels-1}) {
      const auto& s=layout.level[level];const uint32_t x=s.width>4?4:0,y=s.height>4?4:0;
      check(d9::MakeTextureRegion(layout,level,x,y,s.width-x,s.height-y,region));
      std::vector<uint8_t> packed(region.bytes);for(size_t i=0;i<packed.size();++i)packed[i]=uint8_t(i*13+level);
      d9::PatchTextureLayout(layout,region,packed.data(),target.data());
      const auto cols=(region.width+f.block-1)/f.block,rows=(region.height+f.block-1)/f.block;
      uint32_t offset=0;for(uint32_t i=0;i<level;++i)offset+=layout.level[i].bytes;
      const auto pitch=((s.width+f.block-1)/f.block)*f.bytes;
      for(uint32_t r=0;r<rows;++r)for(uint32_t b=0;b<cols*f.bytes;++b)
        expected[offset+(y/f.block+r)*pitch+(x/f.block)*f.bytes+b]=packed[r*cols*f.bytes+b];
    }
    check(target==expected);
  }
  check(d9::MaxContentRead<=d9::MaxBytes-d9::ReplyBytes);
}
}
