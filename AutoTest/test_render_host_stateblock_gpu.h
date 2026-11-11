#pragma once
namespace {
unsigned stateBlockImages=0,stateBlockChecks=0;
void ExerciseStateBlocks(Owner& o) {
  o.stage="stateblock";auto* d=o.device.get();
  auto expect=[&](bool ok,const char* why){Require(ok,why);++stateBlockChecks;};
  auto rs=[&](D3DRENDERSTATETYPE key,DWORD value){Check(d->SetRenderState(key,value),"block set render");};
  auto read=[&](D3DRENDERSTATETYPE key){DWORD value=0;Check(d->GetRenderState(key,&value),"block get render");return value;};
  Com<IDirect3DStateBlock9> block,other;
  expect(d->EndStateBlock(block.put())==D3DERR_INVALIDCALL,"end without begin");
  expect(d->CreateStateBlock(D3DSTATEBLOCKTYPE(0),block.put())==D3DERR_INVALIDCALL,"invalid block type");
  expect(d->CreateStateBlock(D3DSBT_ALL,nullptr)==D3DERR_INVALIDCALL,"null block output");
  Check(d->CreateStateBlock(D3DSBT_ALL,other.put()),"existing block");
  Check(d->BeginStateBlock(),"begin record");
  expect(d->BeginStateBlock()==D3DERR_INVALIDCALL,"nested recording");
  expect(d->CreateStateBlock(D3DSBT_PIXELSTATE,block.put())==D3DERR_INVALIDCALL,"create while recording");
  expect(other->Capture()==D3DERR_INVALIDCALL&&other->Apply()==D3DERR_INVALIDCALL,"existing block during recording");
  expect(d->EndStateBlock(nullptr)==D3DERR_INVALIDCALL,"failed end must preserve recording");
  Check(d->EndStateBlock(block.put()),"retry end record");Check(block->Apply(),"empty block");
  Com<IDirect3DDevice9> parent;Check(block->GetDevice(parent.put()),"block device identity");
  expect(parent.get()==d,"block returned another device");parent.reset();
  Com<IDirect3DStateBlock9> identity;Check(block->QueryInterface(__uuidof(IDirect3DStateBlock9),reinterpret_cast<void**>(identity.put())),"block QI");
  expect(identity.get()==block.get(),"block COM identity");identity.reset();block.reset();other.reset();

  // Explicit source-derived native categories, independent of proxy masks.
  struct RenderCase {D3DRENDERSTATETYPE key;DWORD a,b;unsigned category;};
  const RenderCase renders[]={
    {D3DRS_ZENABLE,0,1,1},{D3DRS_ZWRITEENABLE,0,1,1},{D3DRS_ZFUNC,D3DCMP_LESS,D3DCMP_ALWAYS,1},
    {D3DRS_ALPHABLENDENABLE,0,1,1},{D3DRS_SRCBLEND,D3DBLEND_ONE,D3DBLEND_SRCALPHA,1},
    {D3DRS_DESTBLEND,D3DBLEND_ZERO,D3DBLEND_INVSRCALPHA,1},{D3DRS_COLORWRITEENABLE,15,7,1},
    {D3DRS_TEXTUREFACTOR,0xff224466,0xff668822,1},
    {D3DRS_ALPHATESTENABLE,0,1,1},{D3DRS_ALPHAFUNC,D3DCMP_ALWAYS,D3DCMP_LESS,1},{D3DRS_ALPHAREF,12,100,1},
    {D3DRS_SCISSORTESTENABLE,0,1,1},{D3DRS_SEPARATEALPHABLENDENABLE,0,1,1},
    {D3DRS_SRCBLENDALPHA,D3DBLEND_ONE,D3DBLEND_ZERO,1},{D3DRS_DESTBLENDALPHA,D3DBLEND_ZERO,D3DBLEND_ONE,1},
    {D3DRS_BLENDOPALPHA,D3DBLENDOP_ADD,D3DBLENDOP_SUBTRACT,1},
    {D3DRS_FOGENABLE,0,1,2},{D3DRS_FOGCOLOR,0xff112233,0xff445566,2},
    {D3DRS_FOGTABLEMODE,D3DFOG_NONE,D3DFOG_LINEAR,2},{D3DRS_FOGVERTEXMODE,D3DFOG_NONE,D3DFOG_EXP,2},
    {D3DRS_FOGSTART,0,0x3f800000,3},{D3DRS_FOGEND,0x3f800000,0x40000000,3},{D3DRS_FOGDENSITY,0x3f000000,0x3f800000,3},
    {D3DRS_LIGHTING,0,1,2},{D3DRS_COLORVERTEX,0,1,2},{D3DRS_NORMALIZENORMALS,0,1,2},
    {D3DRS_LOCALVIEWER,0,1,2},{D3DRS_SPECULARENABLE,0,1,2},{D3DRS_RANGEFOGENABLE,0,1,2},
    {D3DRS_AMBIENT,0xff102030,0xff403020,2},
    {D3DRS_DIFFUSEMATERIALSOURCE,D3DMCS_COLOR1,D3DMCS_MATERIAL,2},
    {D3DRS_AMBIENTMATERIALSOURCE,D3DMCS_COLOR1,D3DMCS_MATERIAL,2},
    {D3DRS_SPECULARMATERIALSOURCE,D3DMCS_COLOR2,D3DMCS_MATERIAL,2},
    {D3DRS_EMISSIVEMATERIALSOURCE,D3DMCS_COLOR1,D3DMCS_MATERIAL,2}};
  const D3DVIEWPORT9 full{0,0,64,64,0,1},half{0,0,32,64,0,1};
  const RECT scissorA{0,0,64,64},scissorB{3,4,40,42};
  D3DMATRIX matrixA{},matrixB{};for(unsigned i=0;i<4;++i)matrixA.m[i][i]=matrixB.m[i][i]=1;matrixB._41=.25f;
  D3DMATERIAL9 materialA{},materialB{};materialA.Diffuse=materialA.Ambient=materialB.Ambient={1,1,1,1};materialB.Diffuse={.5f,.25f,.75f,1};
  const std::array<D3DTRANSFORMSTATETYPE,11> transforms={D3DTS_WORLD,D3DTS_VIEW,D3DTS_PROJECTION,
    D3DTS_TEXTURE0,D3DTS_TEXTURE1,D3DTS_TEXTURE2,D3DTS_TEXTURE3,D3DTS_TEXTURE4,D3DTS_TEXTURE5,D3DTS_TEXTURE6,D3DTS_TEXTURE7};
  std::array<float,128> constantsA{},constantsB{};for(unsigned i=0;i<128;++i){constantsA[i]=float(i)/128;constantsB[i]=-float(i)/64;}
  for(auto type:{D3DSBT_PIXELSTATE,D3DSBT_VERTEXSTATE,D3DSBT_ALL}) {
    const unsigned category=type==D3DSBT_ALL?3:type==D3DSBT_PIXELSTATE?1:2;
    for(const auto& r:renders)rs(r.key,r.a);
    for(unsigned i=0;i<8;++i){Check(d->SetSamplerState(i,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP),"capture sampler A");
      Check(d->SetTextureStageState(i,D3DTSS_COLORARG1,D3DTA_TEXTURE),"capture stage A");}
    for(auto key:transforms)Check(d->SetTransform(key,&matrixA),"capture transform A");
    Check(d->SetMaterial(&materialA),"capture material A");Check(d->SetViewport(&full),"capture viewport A");Check(d->SetScissorRect(&scissorA),"capture scissor A");
    Check(d->SetVertexShaderConstantF(0,constantsA.data(),32),"capture VS constants A");
    Check(d->SetPixelShaderConstantF(0,constantsA.data(),32),"capture PS constants A");
    Check(d->SetTexture(7,o.texture.get()),"capture binding A");
    Check(d->SetVertexShader(o.vs.get()),"capture VS A");Check(d->SetPixelShader(o.ps.get()),"capture PS A");
    Check(d->SetFVF(D3DFVF_XYZ|D3DFVF_DIFFUSE),"capture FVF A");
    Check(d->SetStreamSource(0,o.vb.get(),16,16),"capture stream A");Check(d->SetIndices(o.ib.get()),"capture index A");
    Check(d->CreateStateBlock(type,block.put()),"create category block");
    for(const auto& r:renders)rs(r.key,r.b);
    for(unsigned i=0;i<8;++i){Check(d->SetSamplerState(i,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP),"capture sampler B");
      Check(d->SetTextureStageState(i,D3DTSS_COLORARG1,D3DTA_DIFFUSE),"capture stage B");}
    for(auto key:transforms)Check(d->SetTransform(key,&matrixB),"capture transform B");
    Check(d->SetMaterial(&materialB),"capture material B");Check(d->SetViewport(&half),"capture viewport B");Check(d->SetScissorRect(&scissorB),"capture scissor B");
    Check(d->SetVertexShaderConstantF(0,constantsB.data(),32),"capture VS constants B");Check(d->SetPixelShaderConstantF(0,constantsB.data(),32),"capture PS constants B");
    Check(d->SetTexture(7,nullptr),"capture binding B");Check(d->SetVertexShader(nullptr),"capture VS B");Check(d->SetPixelShader(nullptr),"capture PS B");
    Check(d->SetFVF(D3DFVF_XYZ|D3DFVF_TEX1),"capture FVF B");Check(d->SetStreamSource(0,o.vb.get(),32,20),"capture stream B");Check(d->SetIndices(nullptr),"capture index B");
    Check(block->Apply(),"apply category");
    for(const auto& r:renders)expect(read(r.key)==((r.category&category)?r.a:r.b),"render capture category");
    for(unsigned i=0;i<8;++i){DWORD value=0;Check(d->GetSamplerState(i,D3DSAMP_ADDRESSU,&value),"read sampler category");expect(value==DWORD(category&1?D3DTADDRESS_WRAP:D3DTADDRESS_CLAMP),"sampler capture category");
      Check(d->GetTextureStageState(i,D3DTSS_COLORARG1,&value),"read stage category");expect(value==DWORD(category&1?D3DTA_TEXTURE:D3DTA_DIFFUSE),"stage capture category");}
    for(auto key:transforms){D3DMATRIX value{};Check(d->GetTransform(key,&value),"read transform category");expect(!std::memcmp(&value,type==D3DSBT_ALL?&matrixA:&matrixB,sizeof(value)),"transform category");}
    D3DVIEWPORT9 vp{};Check(d->GetViewport(&vp),"read viewport category");expect(!std::memcmp(&vp,type==D3DSBT_ALL?&full:&half,sizeof(vp)),"viewport category");
    RECT sc{};Check(d->GetScissorRect(&sc),"read scissor category");expect(!std::memcmp(&sc,type==D3DSBT_ALL?&scissorA:&scissorB,sizeof(sc)),"scissor category");
    D3DMATERIAL9 mat{};Check(d->GetMaterial(&mat),"read material category");expect(!std::memcmp(&mat,type==D3DSBT_ALL?&materialA:&materialB,sizeof(mat)),"material category");
    std::array<float,128> floats{};Check(d->GetVertexShaderConstantF(0,floats.data(),32),"read VS category");expect(floats==(category&2?constantsA:constantsB),"VS constants category");
    Check(d->GetPixelShaderConstantF(0,floats.data(),32),"read PS category");expect(floats==(category&1?constantsA:constantsB),"PS constants category");
    Com<IDirect3DBaseTexture9> texture;Check(d->GetTexture(7,texture.put()),"read texture category");expect(texture.get()==(type==D3DSBT_ALL?o.texture.get():nullptr),"PIXEL must not capture texture binding");
    Com<IDirect3DVertexShader9> vs;Check(d->GetVertexShader(vs.put()),"read VS category");expect(vs.get()==(category&2?o.vs.get():nullptr),"VS identity category");
    Com<IDirect3DPixelShader9> ps;Check(d->GetPixelShader(ps.put()),"read PS category");expect(ps.get()==(category&1?o.ps.get():nullptr),"PS identity category");
    DWORD fvf=0;Check(d->GetFVF(&fvf),"read FVF category");expect(fvf==DWORD(D3DFVF_XYZ|(category&2?D3DFVF_DIFFUSE:D3DFVF_TEX1)),"declaration category");
    Com<IDirect3DVertexBuffer9> vb;UINT offset=0,stride=0;Check(d->GetStreamSource(0,vb.put(),&offset,&stride),"read stream category");
    expect(vb.get()==o.vb.get()&&offset==UINT(type==D3DSBT_ALL?16:32)&&stride==UINT(type==D3DSBT_ALL?16:20),"stream creation capture");
    Com<IDirect3DIndexBuffer9> ib;Check(d->GetIndices(ib.put()),"read index category");expect(ib.get()==(type==D3DSBT_ALL?o.ib.get():nullptr),"index category");
    block.reset();
  }
  // Capture updates the existing selection, with the native offset exception.
  Check(d->SetStreamSource(0,o.vb.get(),16,16),"offset initial");Check(d->CreateStateBlock(D3DSBT_ALL,block.put()),"offset block");
  Check(d->SetStreamSource(0,o.vb.get(),32,20),"offset changed");Check(block->Capture(),"offset recapture");
  Check(d->SetStreamSource(0,o.vb.get(),64,24),"offset third");Check(block->Apply(),"offset apply");
  {Com<IDirect3DVertexBuffer9> vb;UINT off=0,stride=0;Check(d->GetStreamSource(0,vb.put(),&off,&stride),"offset verify");expect(off==16&&stride==20,"Capture must preserve stored offset");}block.reset();

  // Actual image restoration with deferred record writes, partial viewport,
  // constant-register masks, repeat Apply, and Capture retaining only selection.
  for(auto key:{D3DRS_ZENABLE,D3DRS_ALPHATESTENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_FOGENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_LIGHTING,D3DRS_SEPARATEALPHABLENDENABLE})rs(key,FALSE);
  rs(D3DRS_COLORWRITEENABLE,15);Check(d->SetVertexShader(nullptr),"block fixed VS");Check(d->SetPixelShader(nullptr),"block fixed PS");
  for(auto key:transforms)Check(d->SetTransform(key,&matrixA),"block identity");
  for(unsigned i=0;i<8;++i){Check(d->SetTexture(i,nullptr),"block texture reset");Check(d->SetTextureStageState(i,D3DTSS_COLOROP,i?D3DTOP_DISABLE:D3DTOP_SELECTARG1),"block color op");}
  Check(d->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TFACTOR),"block factor");
  Check(d->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1),"block alpha op");Check(d->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TFACTOR),"block alpha arg");
  struct V{float x,y,z;DWORD color;};const V vertices[]={{-1,1,.5f,0xffffffff},{1,1,.5f,0xffffffff},{1,-1,.5f,0xffffffff},{-1,1,.5f,0xffffffff},{1,-1,.5f,0xffffffff},{-1,-1,.5f,0xffffffff}};
  void* data=nullptr;Check(o.vb->Lock(0,sizeof(vertices),&data,0),"block vertices lock");std::memcpy(data,vertices,sizeof(vertices));Check(o.vb->Unlock(),"block vertices upload");
  Check(d->SetFVF(D3DFVF_XYZ|D3DFVF_DIFFUSE),"block FVF");Check(d->SetStreamSource(0,o.vb.get(),0,sizeof(V)),"block stream");
  Check(d->SetViewport(&full),"block full viewport");
  std::array<float,4> untouchedConstant{};Check(d->GetPixelShaderConstantF(2,untouchedConstant.data(),1),"unselected register baseline");
  auto image=[&](DWORD color,bool left=false){std::fprintf(stderr,"stateblock image=%u expected=%08lx\n",stateBlockImages,static_cast<unsigned long>(color));
    Check(d->BeginScene(),"block begin scene");Check(d->DrawPrimitive(D3DPT_TRIANGLELIST,0,2),"block draw");Check(d->EndScene(),"block end scene");
    std::array<uint32_t,4096> pixels{};for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)pixels[y*64+x]=!left||x<32?color:0xff000000;
    CheckImage(o,pixels);--clearCases;++stateBlockImages;};
  rs(D3DRS_TEXTUREFACTOR,0xffff0000);Check(d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0),"block clear");
  Check(d->SetMaterial(&materialA),"record material baseline");
  Check(d->BeginStateBlock(),"record image");rs(D3DRS_TEXTUREFACTOR,0xff0000ff);Check(d->SetViewport(&half),"record half");
  Check(d->SetMaterial(&materialB),"record material");D3DMATERIAL9 recordedMaterial{};Check(d->GetMaterial(&recordedMaterial),"record material getter");
  expect(!std::memcmp(&recordedMaterial,&materialA,sizeof(materialA)),"record changed live material");
  Check(d->SetFVF(D3DFVF_XYZ|D3DFVF_DIFFUSE),"record same FVF");
  const float registerValue[4]={.25f,.5f,.75f,1};Check(d->SetPixelShaderConstantF(3,registerValue,1),"record one constant");
  expect(d->SetViewport(nullptr)==D3DERR_INVALIDCALL,"invalid record setter accepted");
  std::array<float,4> liveConstant{};Check(d->GetPixelShaderConstantF(3,liveConstant.data(),1),"record getter remains live");
  expect(std::memcmp(liveConstant.data(),registerValue,16)!=0,"record constant changed live state");
  expect(read(D3DRS_TEXTUREFACTOR)==0xffff0000,"record changed live render state");image(0xffff0000);
  Check(d->EndStateBlock(block.put()),"end image record");
  rs(D3DRS_TEXTUREFACTOR,0xff00ff00);image(0xff00ff00);
  Check(d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0),"block clear before apply");
  Check(d->SetFVF(D3DFVF_XYZ|D3DFVF_TEX1),"change FVF after record");Check(block->Apply(),"apply image");image(0xff0000ff,true);
  Check(d->GetMaterial(&recordedMaterial),"applied material getter");expect(!std::memcmp(&recordedMaterial,&materialB,sizeof(materialB)),"record material not applied");
  Check(block->Apply(),"repeat image apply");image(0xff0000ff,true);
  DWORD fvf=0;Check(d->GetFVF(&fvf),"recorded same FVF read");expect(fvf==(D3DFVF_XYZ|D3DFVF_DIFFUSE),"same-FVF fastpath skipped recording");
  std::array<float,4> actual{};Check(d->GetPixelShaderConstantF(3,actual.data(),1),"recorded constant read");expect(!std::memcmp(actual.data(),registerValue,16),"recorded constant not restored");
  Check(d->GetPixelShaderConstantF(2,actual.data(),1),"unrecorded constant read");expect(actual==untouchedConstant,"block changed an unselected register");
  Check(d->SetViewport(&full),"recapture full");rs(D3DRS_TEXTUREFACTOR,0xffffff00);Check(block->Capture(),"image recapture");
  rs(D3DRS_TEXTUREFACTOR,0xff00ffff);Check(block->Apply(),"image recapture apply");image(0xffffff00);block.reset();

  // Null declaration Apply is explicitly skipped by the original implementation.
  Check(d->BeginStateBlock(),"null decl begin");Check(d->SetVertexDeclaration(nullptr),"record null decl");Check(d->EndStateBlock(block.put()),"null decl end");
  Check(block->Apply(),"null decl apply");Check(d->GetFVF(&fvf),"null decl read");expect(fvf==(D3DFVF_XYZ|D3DFVF_DIFFUSE),"null block declaration changed live input");block.reset();
  // MultiplyTransform bypasses native recording; SetTransform does not.
  Check(d->SetTransform(D3DTS_WORLD,&matrixA),"multiply baseline");Check(d->BeginStateBlock(),"multiply begin");
  Check(d->SetTransform(D3DTS_WORLD,&matrixB),"record transform");Check(d->MultiplyTransform(D3DTS_WORLD,&matrixB),"live multiply during record");
  D3DMATRIX got{};Check(d->GetTransform(D3DTS_WORLD,&got),"multiply read");expect(got._41==.25f,"multiply unexpectedly recorded");
  Check(d->EndStateBlock(block.put()),"multiply end");Check(d->SetTransform(D3DTS_WORLD,&matrixA),"multiply reset");Check(block->Apply(),"transform apply");
  Check(d->GetTransform(D3DTS_WORLD,&got),"record transform read");expect(got._41==.25f,"record transform value");block.reset();Check(d->SetTransform(D3DTS_WORLD,&matrixA),"world restore");

  // Only the state block holds this texture after End: neither public caller
  // nor Device binding survives. Apply must resurrect the same live resource.
  Com<IDirect3DTexture9> texture;Check(d->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,texture.put(),nullptr),"block owned texture");
  D3DLOCKED_RECT lock{};Check(texture->LockRect(0,&lock,nullptr,0),"block texture lock");const DWORD purple=0xff8040c0;std::memcpy(lock.pBits,&purple,4);Check(texture->UnlockRect(0),"block texture upload");
  DWORD samplerBefore=0;Check(d->GetSamplerState(0,D3DSAMP_ADDRESSU,&samplerBefore),"record sampler baseline");
  auto* originalTexture=texture.get();Check(d->BeginStateBlock(),"resource begin");Check(d->SetTexture(0,texture.get()),"record texture");
  Check(d->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE),"record texture arg");Check(d->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE),"record texture alpha");
  Check(d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP),"record sampler");
  DWORD samplerDuring=0,stageDuring=0;Check(d->GetSamplerState(0,D3DSAMP_ADDRESSU,&samplerDuring),"record sampler getter");
  Check(d->GetTextureStageState(0,D3DTSS_COLORARG1,&stageDuring),"record stage getter");
  expect(samplerDuring==samplerBefore&&stageDuring==D3DTA_TFACTOR,"record changed live sampler/stage");
  Check(d->EndStateBlock(block.put()),"resource end");texture.reset();
  {Com<IDirect3DBaseTexture9> value;Check(d->GetTexture(0,value.put()),"record kept binding unchanged");expect(!value.get(),"record texture changed live binding");}
  Check(block->Apply(),"private-only texture apply");
  {Com<IDirect3DBaseTexture9> value;Check(d->GetTexture(0,value.put()),"recover texture");expect(value.get()==originalTexture,"block lost resource identity");}
  image(purple);
  Check(d->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,texture.put(),nullptr),"replacement texture");
  Check(texture->LockRect(0,&lock,nullptr,0),"replacement texture lock");const DWORD cyan=0xff20a0c0;std::memcpy(lock.pBits,&cyan,4);Check(texture->UnlockRect(0),"replacement texture upload");
  Check(d->SetTexture(0,texture.get()),"replacement binding");Check(block->Capture(),"replace captured resource");
  texture.reset();Check(d->SetTexture(0,nullptr),"replacement unbind");Check(block->Apply(),"replacement private-only apply");
  block.reset();image(cyan);Check(d->SetTexture(0,nullptr),"release restored texture");
  // A private-only VB must keep both its remote allocation and CPU upload alive.
  Com<IDirect3DVertexBuffer9> privateVB;Check(d->CreateVertexBuffer(sizeof(vertices),D3DUSAGE_WRITEONLY,0,D3DPOOL_DEFAULT,privateVB.put(),nullptr),"private VB");
  Check(privateVB->Lock(0,sizeof(vertices),&data,0),"private VB lock");std::memcpy(data,vertices,sizeof(vertices));Check(privateVB->Unlock(),"private VB upload");
  Check(d->BeginStateBlock(),"private VB begin");Check(d->SetStreamSource(0,privateVB.get(),0,sizeof(V)),"private VB record");Check(d->EndStateBlock(block.put()),"private VB end");
  privateVB.reset();Check(block->Apply(),"private VB apply");block.reset();
  Check(d->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TFACTOR),"restore factor arg");Check(d->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TFACTOR),"restore factor alpha");
  image(0xffffff00);Check(d->SetStreamSource(0,o.vb.get(),0,sizeof(V)),"release private VB");

  // Restore the executable shader pair and its actual VS/PS constants, then
  // use a recorded null pair to return to fixed function. Both paths draw.
  {
    constexpr char shader[]="float4 shift:register(c1);float4 color:register(c3);"
      "float4 vs(float3 p:POSITION):POSITION{return float4(p.xy+shift.xy,p.z,1);}"
      "float4 ps():COLOR0{return color;}";
    const auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(o.compiler,"D3DCompile"));
    Com<ID3DBlob> code,errors;Com<IDirect3DVertexShader9> vs;Com<IDirect3DPixelShader9> ps;
    Check(compile(shader,sizeof(shader)-1,"block-vs",nullptr,nullptr,"vs","vs_3_0",0,0,code.put(),errors.put()),"block compile VS");
    Check(d->CreateVertexShader(static_cast<const DWORD*>(code->GetBufferPointer()),vs.put()),"block create VS");code.reset();errors.reset();
    Check(compile(shader,sizeof(shader)-1,"block-ps",nullptr,nullptr,"ps","ps_3_0",0,0,code.put(),errors.put()),"block compile PS");
    Check(d->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()),ps.put()),"block create PS");
    Check(d->SetVertexShader(vs.get()),"block bind VS");Check(d->SetPixelShader(ps.get()),"block bind PS");
    const float zero[4]={0,0,0,0},shift[4]={2,2,0,0},red[4]={1,0,0,1},green[4]={0,1,0,1};
    Check(d->SetVertexShaderConstantF(1,zero,1),"block VS constant");Check(d->SetPixelShaderConstantF(3,red,1),"block PS constant");
    Check(d->CreateStateBlock(D3DSBT_ALL,block.put()),"shader block");
    Check(d->SetVertexShaderConstantF(1,shift,1),"mutate VS constant");Check(d->SetPixelShaderConstantF(3,green,1),"mutate PS constant");
    Check(d->SetVertexShader(nullptr),"remove VS");Check(d->SetPixelShader(nullptr),"remove PS");
    vs.reset();ps.reset();Check(block->Apply(),"restore private-only shader pair");block.reset();image(0xffff0000);
    constexpr char nullShader[]="sampler2D a:register(s0);sampler2D b:register(s7);float4 ps():COLOR0{return tex2D(a,float2(.5,.5))*tex2D(b,float2(.5,.5));}";
    code.reset();errors.reset();Check(compile(nullShader,sizeof(nullShader)-1,"block-null-ps",nullptr,nullptr,"ps","ps_3_0",0,0,code.put(),errors.put()),"null sampler compile");
    Check(d->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()),ps.put()),"null sampler shader");Check(d->SetPixelShader(ps.get()),"null sampler bind");image(0xff000000);
    Check(d->BeginStateBlock(),"record fixed function return");Check(d->SetVertexShader(nullptr),"record null VS");Check(d->SetPixelShader(nullptr),"record null PS");
    Check(d->GetVertexShader(vs.put()),"live recorded VS");Check(d->GetPixelShader(ps.put()),"live recorded PS");expect(vs.get()&&ps.get(),"record null shader changed live pair");
    Check(d->EndStateBlock(block.put()),"end fixed function return");Check(block->Apply(),"apply fixed function return");
    block.reset();vs.reset();ps.reset();image(0xffffff00);
  }

  // Light definitions and enable selection have distinct masks. Initial false
  // is a recorder no-op, while true->false records an actual disable.
  D3DLIGHT9 light{};light.Type=D3DLIGHT_DIRECTIONAL;light.Diffuse={1,1,1,0};light.Direction={0,0,1};
  Check(d->SetLight(7,&light),"light baseline");Check(d->LightEnable(7,TRUE),"light live enable");
  Check(d->BeginStateBlock(),"light no-op begin");Check(d->LightEnable(7,FALSE),"light recorded false");Check(d->EndStateBlock(block.put()),"light no-op end");Check(block->Apply(),"light no-op apply");
  BOOL enabled=0;Check(d->GetLightEnable(7,&enabled),"light no-op read");expect(enabled!=0,"initial false captured an unintended disable");block.reset();
  Check(d->BeginStateBlock(),"light change begin");Check(d->LightEnable(7,TRUE),"light recorded on");Check(d->LightEnable(7,FALSE),"light recorded off");Check(d->EndStateBlock(block.put()),"light change end");
  Check(block->Apply(),"light change apply");Check(d->GetLightEnable(7,&enabled),"light change read");expect(!enabled,"explicit recorded disable missing");block.reset();
  // Defined light capture copies newly defined lights too, but not new enable bits.
  Check(d->BeginStateBlock(),"light definition begin");Check(d->SetLight(7,&light),"light record definition");Check(d->EndStateBlock(block.put()),"light definition end");
  light.Diffuse={.25f,.5f,.75f,0};Check(d->SetLight(6,&light),"new light definition");Check(block->Capture(),"light recapture");
  light.Diffuse={1,0,0,0};Check(d->SetLight(6,&light),"change new light");Check(block->Apply(),"light definitions apply");Check(d->GetLight(6,&light),"read new light");expect(light.Diffuse.g==.5f,"Capture omitted a new defined light");block.reset();
  // Native null stream binding retains its old offset/stride, including Apply.
  Check(d->SetStreamSource(0,o.vb.get(),16,16),"null stream baseline");Check(d->SetStreamSource(0,nullptr,100,100),"null stream direct");
  {Com<IDirect3DVertexBuffer9> value;UINT off=0,stride=0;Check(d->GetStreamSource(0,value.put(),&off,&stride),"null stream read");expect(!value.get()&&off==16&&stride==16,"null stream changed offset/stride");}
  Check(d->BeginStateBlock(),"null stream record begin");Check(d->SetStreamSource(0,nullptr,88,88),"null stream record");Check(d->EndStateBlock(block.put()),"null stream record end");
  Check(d->SetStreamSource(0,o.vb.get(),32,20),"null stream change");Check(block->Apply(),"null stream apply");
  {Com<IDirect3DVertexBuffer9> value;UINT off=0,stride=0;Check(d->GetStreamSource(0,value.put(),&off,&stride),"null stream apply read");expect(!value.get()&&off==32&&stride==20,"null stream Apply changed offset/stride");}block.reset();
  Check(d->SetStreamSource(0,o.vb.get(),0,sizeof(V)),"null stream recovery");
  // The bounded profile rejects a recaptured shorter buffer before applying
  // any selected state, rather than silently keeping the previous VB.
  if(!o.options.reference) {
    D3DVERTEXBUFFER_DESC desc{};Check(o.vb->GetDesc(&desc),"bounded source extent");Require(desc.Size>sizeof(V),"bounded source too small");
    Check(d->SetStreamSource(0,o.vb.get(),desc.Size,sizeof(V)),"bounded capture offset");rs(D3DRS_TEXTUREFACTOR,0xff010203);
    Check(d->CreateStateBlock(D3DSBT_ALL,block.put()),"bounded capture block");
    Check(d->CreateVertexBuffer(sizeof(V),D3DUSAGE_WRITEONLY,0,D3DPOOL_DEFAULT,privateVB.put(),nullptr),"bounded shorter VB");
    Check(d->SetStreamSource(0,privateVB.get(),0,sizeof(V)),"bounded shorter bind");Check(block->Capture(),"bounded shorter capture");
    Check(d->SetStreamSource(0,o.vb.get(),0,sizeof(V)),"bounded live recovery");rs(D3DRS_TEXTUREFACTOR,0xff030201);
    expect(block->Apply()==D3DERR_INVALIDCALL&&read(D3DRS_TEXTUREFACTOR)==0xff030201,"invalid recaptured range partly applied");
    block.reset();privateVB.reset();
  }
  // Leave one texture public and privately bound for the shared Device cycle test.
  Check(d->SetTexture(7,o.texture.get()),"restore teardown texture");
  // Abandoned recording must be destroyed before the proxy sends Finish.
  Check(d->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,texture.put(),nullptr),"abandoned record texture");
  Check(d->BeginStateBlock(),"abandoned record begin");Check(d->SetTexture(0,texture.get()),"abandoned record binding");texture.reset();
}
}
