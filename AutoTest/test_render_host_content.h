#pragma once
#include "../tools/render_host/d3d9_content_wire.h"
#include <vector>
// Content payload codecs, reference records and the admission index both ends
// run, independently of any connection, host or GPU.
namespace warvk_content_test {
using namespace warvk::host;
template<class Check> void Run(Check check) {
  using Admit=d9::ContentIndex::Admit;
  const auto geometry=uint32_t(d9::ContentKind::Geometry),bones=uint32_t(d9::ContentKind::BoneMatrices);
  const auto geometryRef=uint16_t(d9::AnnotationSchema::GeometryRef),bonesRef=uint16_t(d9::AnnotationSchema::BonesRef);
  // Kinds, and the reference schema that names each.
  for(uint32_t kind=0;kind<5;++kind)check(d9::ValidContentKind(kind)==(kind==1||kind==2));
  check(d9::ContentKindOf(geometryRef)==geometry&&d9::ContentKindOf(bonesRef)==bones&&geometryRef==3&&bonesRef==4);
  for(const uint32_t schema:{0u,1u,2u,5u,0x10003u})check(!d9::ContentKindOf(schema));
  check(d9::ValidDrawRecord(3,16)&&d9::ValidDrawRecord(4,16)&&!d9::ValidDrawRecord(3,12)&&!d9::ValidDrawRecord(4,24)
    &&!d9::ValidDrawRecord(5,16)&&d9::ValidDrawRecord(1,16)&&!d9::ValidDrawRecord(2,16));
  const d9::ContentRef ref{0x1122334455667788ull,(uint64_t(3)<<32)|9};
  const auto encoded=d9::EncodeContentRef(ref);const auto decoded=d9::DecodeContentRef(encoded.data());
  check(encoded.size()==16&&decoded.key==ref.key&&decoded.revision==ref.revision
    &&d9::U32(encoded.data())==0x55667788u&&d9::U32(encoded.data()+4)==0x11223344u&&d9::U32(encoded.data()+8)==9&&d9::U32(encoded.data()+12)==3);
  // One draw may carry its tag, its geometry and its bones, ascending by schema.
  {
    const auto tag=d9::EncodeDrawTag({5,1,2});std::array<uint8_t,96> records{};uint32_t at=0;
    at+=d9::AppendDrawRecord(records.data()+at,96-at,0,1,tag.data(),16);
    at+=d9::AppendDrawRecord(records.data()+at,96-at,0,geometryRef,encoded.data(),16);
    at+=d9::AppendDrawRecord(records.data()+at,96-at,0,bonesRef,encoded.data(),16);
    d9::Header h;h.op=d9::Op::AnnotateDraws;h.args[0]=3;h.args[1]=1;h.args[2]=uint32_t(d9::Op::DrawFixed);h.payloadBytes=at;
    d9::DrawAnnotations out;check(at==72&&d9::DecodeDrawAnnotations(h,records.data(),out)&&out.count==3);
    uint32_t seen=0;out.forEach([&](uint16_t index,uint16_t schema,const uint8_t* data,uint16_t bytes){
      check(!index&&bytes==16&&schema==(seen==0?1:seen==1?geometryRef:bonesRef));
      if(seen)check(d9::DecodeContentRef(data).key==ref.key&&d9::DecodeContentRef(data).revision==ref.revision);++seen;});
    check(seen==3);
    auto bad=records;bad[24+4]=12;check(!d9::DecodeDrawAnnotations(h,bad.data(),out)); // a reference is exactly 16 bytes
    check(!d9::AppendDrawRecord(records.data(),96,0,geometryRef,encoded.data(),12)&&!d9::AppendDrawRecord(records.data(),96,0,5,encoded.data(),16));
  }
  // Geometry is opaque; bone matrices are whole finite floats.
  std::vector<uint8_t> opaque(96,0xff),floats(96);
  for(uint32_t i=0;i<24;++i)d9::P32(floats.data()+4*i,d9::FloatBits(float(i)*.5f-3));
  check(d9::ValidContentPayload(geometry,0,opaque.data(),96)&&d9::ValidContentPayload(geometry,5,opaque.data(),7)
    &&!d9::ValidContentPayload(geometry,0,nullptr,4)&&!d9::ValidContentPayload(geometry,0,opaque.data(),0));
  check(d9::ValidContentPayload(bones,0,floats.data(),96)&&d9::ValidContentPayload(bones,48,floats.data(),48)
    &&!d9::ValidContentPayload(bones,0,floats.data(),94)&&!d9::ValidContentPayload(bones,2,floats.data(),48)
    &&!d9::ValidContentPayload(bones,0,opaque.data(),96)&&!d9::ValidContentPayload(bones,0,nullptr,96));
  for(uint32_t word=0;word<24;++word)for(const uint32_t value:{0x7fc00000u,0x7f800000u,0xff800000u}) {
    auto broken=floats;d9::P32(broken.data()+word*4,value);check(!d9::ValidContentPayload(bones,0,broken.data(),96));
  }
  // ContentUpload: round trip, then every refusal leaves the destination untouched.
  {
    const d9::ContentChunk chunk{geometry,ref.key,ref.revision,200,96,96};
    const auto h=d9::EncodeContentUpload(chunk);d9::ContentChunk back,untouched;
    check(h.op==d9::Op::ContentUpload&&h.payloadBytes==96&&!h.id&&!h.generation&&d9::DecodeContentUpload(h,opaque.data(),back)
      &&back.kind==geometry&&back.key==ref.key&&back.revision==ref.revision&&back.total==200&&back.offset==96&&back.count==96);
    auto refused=[&](d9::Header c,const uint8_t* p){
      check(!d9::DecodeContentUpload(c,p,untouched)&&!untouched.kind&&!untouched.key&&!untouched.total&&!untouched.count);};
    refused(h,nullptr);
    {auto c=h;c.op=d9::Op::ContentRelease;refused(c,opaque.data());c=h;c.id=1;refused(c,opaque.data());c=h;c.generation=1;refused(c,opaque.data());}
    {auto c=h;c.args[0]=0;refused(c,opaque.data());c.args[0]=3;refused(c,opaque.data());}
    {auto c=h;c.args[1]=c.args[2]=0;refused(c,opaque.data());c=h;c.args[3]=c.args[4]=0;refused(c,opaque.data());}
    {auto c=h;c.args[5]=0;refused(c,opaque.data());c.args[5]=d9::MaxContentBlobBytes+1;refused(c,opaque.data());
      c.args[5]=d9::MaxContentBlobBytes;check(d9::DecodeContentUpload(c,opaque.data(),back)&&back.total==d9::MaxContentBlobBytes);}
    {auto c=h;c.args[6]=200;refused(c,opaque.data());c.args[6]=105;refused(c,opaque.data());
      c.args[6]=104;check(d9::DecodeContentUpload(c,opaque.data(),back)&&back.offset==104);}
    {auto c=h;c.payloadBytes=0;refused(c,opaque.data());c.payloadBytes=d9::MaxPayload+1;refused(c,opaque.data());}
    for(size_t i=7;i<h.args.size();++i){auto c=h;c.args[i]=1;refused(c,opaque.data());}
    // The payload rule of the kind applies to every chunk.
    {auto c=h;c.args[0]=bones;refused(c,opaque.data());check(d9::DecodeContentUpload(c,floats.data(),back)&&back.kind==bones);
      c.args[6]=98;refused(c,floats.data());}
  }
  // ContentRelease and ContentRead.
  {
    const auto release=d9::EncodeContentRelease(ref);d9::ContentRef back,untouched;
    check(release.op==d9::Op::ContentRelease&&!release.payloadBytes&&d9::DecodeContentRelease(release,back)
      &&back.key==ref.key&&back.revision==ref.revision);
    auto refused=[&](d9::Header c){check(!d9::DecodeContentRelease(c,untouched)&&!untouched.key&&!untouched.revision);};
    {auto c=release;c.op=d9::Op::ContentRead;refused(c);c=release;c.id=1;refused(c);c=release;c.generation=1;refused(c);
      c=release;c.payloadBytes=4;refused(c);c=release;c.args[0]=c.args[1]=0;refused(c);c=release;c.args[2]=c.args[3]=0;refused(c);}
    for(size_t i=4;i<release.args.size();++i){auto c=release;c.args[i]=1;refused(c);}
    const auto read=d9::EncodeContentRead(ref,40,d9::MaxPayload);uint32_t offset=1,count=1;
    check(read.op==d9::Op::ContentRead&&!read.payloadBytes&&d9::DecodeContentRead(read,back,offset,count)
      &&back.key==ref.key&&back.revision==ref.revision&&offset==40&&count==d9::MaxPayload);
    auto refusedRead=[&](d9::Header c){uint32_t o=7,n=7;d9::ContentRef r;check(!d9::DecodeContentRead(c,r,o,n)&&o==7&&n==7&&!r.key);};
    {auto c=read;c.op=d9::Op::ContentRelease;refusedRead(c);c=read;c.id=1;refusedRead(c);c=read;c.generation=1;refusedRead(c);
      c=read;c.payloadBytes=4;refusedRead(c);c=read;c.args[0]=c.args[1]=0;refusedRead(c);c=read;c.args[2]=c.args[3]=0;refusedRead(c);
      c=read;c.args[5]=0;refusedRead(c);c.args[5]=d9::MaxPayload+1;refusedRead(c);}
    for(size_t i=6;i<read.args.size();++i){auto c=read;c.args[i]=1;refusedRead(c);}
  }
  // The index: open, chunk order, commit, replacement, release, clear.
  {
    d9::ContentIndex index;
    auto part=[&](uint32_t kind,uint64_t key,uint64_t revision,uint32_t total,uint32_t offset,uint32_t count){
      return index.admit({kind,key,revision,total,offset,count});};
    check(index.canOpen(7,100)&&!index.canOpen(0,100)&&!index.canOpen(7,0)&&!index.canOpen(7,d9::MaxContentBlobBytes+1)
      &&index.canOpen(7,d9::MaxContentBlobBytes)&&!index.count()&&!index.lastRevision());
    check(part(geometry,7,1,100,0,100)==Admit::Complete&&index.count()==1&&index.bytes()==100&&index.lastRevision()==1
      &&index.committedBlobs==1&&!index.active());
    check(index.bound({7,1},geometry)&&!index.bound({7,1},bones)&&!index.bound({7,2},geometry)&&!index.bound({8,1},geometry)
      &&!index.bound({7,0},geometry)&&index.find({7,1})->bytes==100&&!index.find({7,2}));
    // An open blob: nothing binds, opens or leaves until its last chunk.
    check(part(bones,9,2,300,0,100)==Admit::More&&index.active()&&index.lastRevision()==2&&index.count()==1
      &&!index.bound({7,1},geometry)&&index.find({7,1})&&!index.canOpen(11,10)&&!index.release({7,1})
      &&index.pending().key==9&&index.pending().count==100);
    check(part(bones,9,2,300,100,100)==Admit::More&&part(bones,9,2,300,200,100)==Admit::Complete&&!index.active()
      &&index.count()==2&&index.bytes()==400&&index.bound({9,2},bones)&&index.bound({7,1},geometry));
    // A revision is consumed when its blob opens and is never accepted again.
    check(part(geometry,12,2,10,0,10)==Admit::Refused&&part(geometry,12,1,10,0,10)==Admit::Refused&&index.count()==2);
    // A refused first chunk consumes nothing.
    check(part(geometry,12,3,10,4,6)==Admit::Refused&&part(3,12,3,10,0,10)==Admit::Refused&&part(geometry,0,3,10,0,10)==Admit::Refused
      &&part(geometry,12,3,10,0,0)==Admit::Refused&&part(geometry,12,3,10,0,11)==Admit::Refused
      &&part(geometry,12,3,d9::MaxContentBlobBytes+1,0,10)==Admit::Refused&&index.lastRevision()==2&&!index.active()&&!index.abortedBlobs);
    // Any chunk but the open blob's next one is refused and drops the blob.
    uint64_t revision=2;
    const d9::ContentChunk next{geometry,12,0,100,40,30};
    for(uint32_t variant=0;variant<8;++variant) {
      ++revision;check(part(geometry,12,revision,100,0,40)==Admit::More);
      auto wrong=next;wrong.revision=revision;
      switch(variant) {
        case 0: wrong.kind=bones;break;
        case 1: wrong.key=13;break;
        case 2: wrong.revision=revision+1;break;
        case 3: wrong.total=101;break;
        case 4: wrong.offset=41;break;
        case 5: wrong.offset=0;wrong.count=40;break;
        case 6: wrong.count=61;break;
        default: wrong.count=0;break;
      }
      const auto aborted=index.abortedBlobs;
      check(index.admit(wrong)==Admit::Refused&&!index.active()&&index.abortedBlobs==aborted+1&&index.count()==2&&index.bytes()==400);
      auto right=next;right.revision=revision;check(index.admit(right)==Admit::Refused&&index.lastRevision()==revision);
    }
    check(index.abortedBlobs==8&&!index.find({12,revision}));
    // Replacement: the same key under a newer revision; the old one never binds again.
    ++revision;
    check(part(geometry,7,revision,60,0,60)==Admit::Complete&&index.count()==2&&index.bytes()==360&&index.replacedBlobs==1
      &&!index.bound({7,1},geometry)&&index.bound({7,revision},geometry)&&index.find({7,revision})->bytes==60);
    // A blob may change kind with its revision.
    ++revision;check(part(bones,7,revision,48,0,48)==Admit::Complete&&index.bound({7,revision},bones)&&!index.bound({7,revision},geometry)
      &&index.bytes()==348&&index.replacedBlobs==2);
    // Release takes the exact live identity.
    check(!index.release({7,1})&&!index.release({8,revision})&&!index.release({7,0})&&index.release({7,revision})
      &&!index.release({7,revision})&&index.count()==1&&index.bytes()==300&&index.releasedBlobs==1&&!index.bound({7,revision},bones));
    check(index.peakBytes==400&&index.peakBlobs==2&&index.committedBlobs==4);
    // A session reset drops every blob; revisions keep ascending.
    index.clear();
    check(!index.count()&&!index.bytes()&&index.clearedBlobs==1&&!index.bound({9,2},bones)&&index.lastRevision()==revision);
    check(part(bones,9,revision,48,0,48)==Admit::Refused&&part(bones,9,revision+1,48,0,48)==Admit::Complete&&index.bound({9,revision+1},bones));
  }
  // Capacity: the blob count, with replacement still possible at the limit.
  {
    d9::ContentIndex index;uint64_t revision=0;bool filled=true;
    for(uint32_t i=0;i<d9::MaxContentBlobs;++i)filled=filled&&index.admit({geometry,uint64_t(i)+1,++revision,4,0,4})==Admit::Complete;
    check(filled&&index.count()==d9::MaxContentBlobs&&index.bytes()==uint64_t(d9::MaxContentBlobs)*4);
    check(!index.canOpen(uint64_t(d9::MaxContentBlobs)+1,4)&&index.admit({geometry,uint64_t(d9::MaxContentBlobs)+1,++revision,4,0,4})==Admit::Refused);
    check(index.canOpen(5,8)&&index.admit({geometry,5,++revision,8,0,8})==Admit::Complete&&index.count()==d9::MaxContentBlobs);
    check(index.release({9,9})&&index.canOpen(uint64_t(d9::MaxContentBlobs)+1,4));
  }
  // Capacity: total bytes, counted net of the blob being replaced.
  {
    d9::ContentIndex index;uint64_t revision=0;bool filled=true;const uint32_t big=d9::MaxContentBlobBytes;
    const auto blobs=uint32_t(d9::MaxContentStoreBytes/big);
    for(uint32_t i=0;i<blobs;++i)filled=filled&&index.admit({geometry,uint64_t(i)+1,++revision,big,0,big})==Admit::Complete;
    check(filled&&blobs==64&&index.bytes()==d9::MaxContentStoreBytes&&index.peakBytes==d9::MaxContentStoreBytes);
    check(!index.canOpen(1000,1)&&index.admit({geometry,1000,++revision,1,0,1})==Admit::Refused);
    check(index.canOpen(1,big)&&index.admit({geometry,1,++revision,big-4,0,big-4})==Admit::Complete&&index.bytes()==d9::MaxContentStoreBytes-4);
    check(index.canOpen(1000,4)&&!index.canOpen(1000,5)&&index.release({2,2})&&index.canOpen(1000,big));
  }
  // The recorder folds a committed blob as (5, kind, key, revision, bytes, content) and a release as (6, key, revision).
  {
    d9::AnnotationRecorder a,b;a.content(bones,ref.key,ref.revision,floats.data(),96);
    b.word(5);b.word(bones);b.word(0x55667788u);b.word(0x11223344u);b.word(9);b.word(3);b.word(96);b.fold(floats.data(),96);
    check(a.digest==b.digest&&a.contentUploads==1&&!a.contentReleases);
    auto changed=floats;changed[95]^=1;d9::AnnotationRecorder c;c.content(bones,ref.key,ref.revision,changed.data(),96);check(c.digest!=a.digest);
    a.release(ref.key,ref.revision);b.word(6);b.word(0x55667788u);b.word(0x11223344u);b.word(9);b.word(3);
    check(a.digest==b.digest&&a.contentReleases==1);
  }
}
} // namespace warvk_content_test
