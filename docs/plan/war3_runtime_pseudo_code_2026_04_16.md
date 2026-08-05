# War3 Runtime 核心函数伪代码导出 (2026-04-16)

基于 `rb_runtime_hook_migration_plan.md` 和 `war3_runtime_rtti_ground_truth_2026_04_16.md` 提取。
本文件内容通过 IDA-Pro MCP 自动生成，用于后续静态分析和对比，无需再切回 IDA。

## MainLoop_6F05F710 (`0x6f05f710`)

```cpp
int __stdcall MainLoop_6F05F710(void *a1)
{
  int v1; // ebx
  DWORD CurrentThreadId; // eax
  int v3; // eax
  int v4; // esi
  signed int v5; // edi
  int v6; // edi
  int v7; // edi
  bool v8; // zf
  BOOL v9; // ebx
  int v10; // edi
  unsigned int v11; // eax
  int v12; // ecx
  int v14; // [esp+0h] [ebp-5Ch]
  int v15; // [esp+4h] [ebp-58h]
  DWORD TickCount; // [esp+Ch] [ebp-50h]
  int v17; // [esp+10h] [ebp-4Ch] BYREF
  int v18; // [esp+14h] [ebp-48h]
  _BYTE v19[64]; // [esp+18h] [ebp-44h] BYREF

  sub_6F04F000(0);
  if ( a1 )
  {
    v1 = dword_6FBB8978;
    v18 = dword_6FBB8978;
  }
  else
  {
    v1 = sub_6F05E790();
    v18 = v1;
  }
  MainLoop_WaitGate(0xFFFFFFFF);
  CurrentThreadId = GetCurrentThreadId();
  Storm_578(v19, 64, "Engine %x", CurrentThreadId);
  sub_6F159B00(v19);
  while ( 1 )
  {
    if ( sub_6F155300() != nPriority )
      sub_6F155840(nPriority);
    if ( !MainLoop_WaitGate(0) )
      break;
    v3 = MainLoop_SelectWorker(v1);
    v4 = v3;
    v5 = -1;
    if ( v3 )
    {
      v6 = *(_DWORD *)(v3 + 60);
      v5 = v6 - GetTickCount();
      if ( v5 < 0 )
        v5 = 0;
    }
    if ( dword_6FBB8964 )
    {
      if ( v5 == -1 )
        v5 = 100;
      MainLoop_SleepGate(v5);
      v7 = 258;
    }
    else
    {
      MainLoop_PrepareWait(v1);
      v7 = MainLoop_WaitGate(v5);
    }
    if ( v4 )
    {
      sub_6F04F000(*(LPVOID *)(v4 + 520));
      sub_6F04F010(0);
      sub_6F159DD0(*(_DWORD *)(v4 + 524));
      TickCount = GetTickCount();
      *(_DWORD *)(v4 + 40) = TickCount;
      if ( v7 == 258 )
      {
        MainLoop_PrepareDispatch(v4);
        MainLoop_RunCallbacks(v4);
        v8 = (*(_BYTE *)(v4 + 68) & 2) == 0;
        v17 = 0;
        if ( !v8 )
        {
          MainLoop_MessagePump(v4, (int)&v17);
          if ( v17 )
          {
            sub_6F1577E0((LPCRITICAL_SECTION)(v4 + 16));
            if ( !*(_DWORD *)(v4 + 44) )
              *(_DWORD *)(v4 + 44) = 1;
            sub_6F158440((LPCRITICAL_SECTION)(v4 + 16));
            sub_6F05E730();
          }
        }
        MainLoop_FinalizeDispatch(v4);
        MainLoop_QueueFlush(v14, v15);
        MainLoop_TickUpdate(v4);
        ReleaseCriticalSection_05FCD0(v4);
      }
      sub_6F1577E0((LPCRITICAL_SECTION)(v4 + 16));
      v9 = *(_DWORD *)(v4 + 44) == 1;
      sub_6F158440((LPCRITICAL_SECTION)(v4 + 16));
      if ( v9 )
      {
        v1 = v18;
        MainLoop_FinalizeWorker(v18, v4);
        MainLoop_FinalizeTick(v4);
      }
      else
      {
        if ( (*(_BYTE *)(v4 + 68) & 4) != 0 )
        {
          v10 = 0;
        }
        else
        {
          v11 = MainLoop_ComputeWakeDelta(v4, TickCount);
          v12 = *(_DWORD *)(v4 + 72);
          if ( v12 != *(_DWORD *)(v4 + 76) )
            v11 = *(_DWORD *)(v4 + 72);
          v10 = v12 + *(_DWORD *)(v4 + 64) - TickCount;
          if ( v10 < 0 )
            v10 = 0;
          if ( v11 < v10 )
            v10 = v11;
        }
        sub_6F159D70(*(_DWORD *)(v4 + 524));
        sub_6F04F000(0);
        v1 = v18;
        MainLoop_Reschedule(v10 + TickCount, *(_DWORD *)(v4 + 84));
      }
    }
  }
  sub_6F05DAC0(v14, v15);
  sub_6F1595C0();
  return 0;
}

```

## CWorldFrameWar3_UpdateWorldFrameAndPreparePasses (`0x6f368480`)

```cpp
int __thiscall sub_6F368480(int this, int a2, int a3, int a4)
{
  float v5; // xmm0_4
  int v6; // eax
  unsigned int v7; // xmm1_4
  int v8; // esi
  int GameUI_34F3A0; // eax
  unsigned int v10; // eax
  unsigned int v11; // xmm0_4
  unsigned int v12; // eax
  unsigned int v13; // xmm0_4
  unsigned int v14; // eax
  unsigned int v15; // xmm0_4
  unsigned int v16; // eax
  int v17; // ecx
  int v18; // edi
  int v19; // ecx
  int v20; // eax
  bool v21; // zf
  unsigned int v22; // ecx
  int v23; // edi
  int v24; // eax
  int v25; // eax
  int v26; // eax
  float v28[16]; // [esp+18h] [ebp-11Ch] BYREF
  _OWORD v29[3]; // [esp+58h] [ebp-DCh] BYREF
  _BYTE v30[12]; // [esp+8Ch] [ebp-A8h] BYREF
  _DWORD v31[3]; // [esp+98h] [ebp-9Ch] BYREF
  _BYTE v32[12]; // [esp+A4h] [ebp-90h] BYREF
  int v33; // [esp+B0h] [ebp-84h] BYREF
  _BYTE v34[4]; // [esp+B4h] [ebp-80h] BYREF
  unsigned int v35; // [esp+B8h] [ebp-7Ch] BYREF
  unsigned int v36; // [esp+BCh] [ebp-78h]
  unsigned int v37; // [esp+C0h] [ebp-74h] BYREF
  _OWORD v38[6]; // [esp+C4h] [ebp-70h] BYREF
  int v39; // [esp+130h] [ebp-4h]

  v5 = *(float *)(this + 912) + *(float *)(this + 572);
  v36 = *(_DWORD *)(this + 572);
  *(_DWORD *)(this + 572) = 0;
  *(float *)(this + 912) = v5;
  sub_6F35EBE0(v34);
  v39 = 0;
  v6 = sub_6F30D610();
  sub_6F186D60(v6);
  memset(v38, 0, sizeof(v38));
  v29[0] = xmmword_6F95AC20;
  v29[1] = xmmword_6F95AC20;
  v29[2] = xmmword_6F95AC20;
  sub_6F0D0AF0(v28);
  v7 = v36;
  *(float *)(this + 896) = *(float *)(this + 896) + *(float *)&v36;
  if ( sub_6F0A2300(v7, a3, a4) )
  {
    GameUI_34F3A0 = GetGameUI_34F3A0(1, 0);
    sub_6F37C520(*(_DWORD *)(GameUI_34F3A0 + 592));
    sub_6F364960(this + 784, 0);
    sub_6F368D60(this);
    sub_6F346980(v36, a4);
    sub_6F0E3A00(v29);
    sub_6F0E3910(v28);
    sub_6F0D2370(v29, v28, (int)v38);
    sub_6F139860();
    if ( *(float *)(this + 916) == INFINITY )
      *(float *)(this + 916) = sub_6F770270();
    *(__m128i *)(this + 468) = _mm_loadu_si128((const __m128i *)sub_6F361D00(8, *(_DWORD *)(this + 916)));
    v35 = *(_DWORD *)(this + 468);
    sub_6F0528B0((int *)&v37, (int *)&v35, (_DWORD *)(dword_6FBB82BC + 112));
    v37 = (v37 - 41943040) & ~((int)(v37 ^ (v37 - 50331648)) >> 31);
    v10 = sub_6F052F30((int *)&v37);
    v11 = *(_DWORD *)(this + 472);
    *(_DWORD *)(this + 484) = v10;
    v35 = v11;
    sub_6F0528B0((int *)&v37, (int *)&v35, (_DWORD *)(dword_6FBB82BC + 108));
    v37 = (v37 - 41943040) & ~((int)(v37 ^ (v37 - 50331648)) >> 31);
    v12 = sub_6F052F30((int *)&v37);
    v13 = *(_DWORD *)(this + 476);
    *(_DWORD *)(this + 488) = v12;
    v35 = v13;
    sub_6F0528B0((int *)&v37, (int *)&v35, (_DWORD *)(dword_6FBB82BC + 112));
    v37 = (v37 - 41943040) & ~((int)(v37 ^ (v37 - 50331648)) >> 31);
    v14 = sub_6F052F30((int *)&v37);
    v15 = *(_DWORD *)(this + 480);
    *(_DWORD *)(this + 492) = v14;
    v35 = v15;
    sub_6F0528B0((int *)&v37, (int *)&v35, (_DWORD *)(dword_6FBB82BC + 108));
    v37 = (v37 - 41943040) & ~((int)(v37 ^ (v37 - 50331648)) >> 31);
    v16 = sub_6F052F30((int *)&v37);
    v17 = *(_DWORD *)(this + 412);
    *(_DWORD *)(this + 496) = v16;
    v18 = *(_DWORD *)(v17 + 52);
    v35 = v18;
    v33 = v18;
    sub_6F343150(v32);
    sub_6F3431C0(v31);
    sub_6F1398E0(v32);
    v19 = *(_DWORD *)(this + 852);
    if ( v19 )
      *(_DWORD *)(this + 860) = sub_6F184F00(v19, v18, v36, 0);
    if ( *(float *)(this + 896) > 3.0 )
      *(_DWORD *)(this + 904) = 1;
    v20 = sub_6F343150(v30);
    v21 = *(_DWORD *)(this + 908) == 0;
    *(_QWORD *)(this + 396) = *(_QWORD *)v20;
    *(_DWORD *)(this + 404) = *(_DWORD *)(v20 + 8);
    if ( !v21 )
    {
      sub_6F76EF80();
      sub_6F770670((int)v32, (int)v31, 1084227584);
    }
    sub_6F76D920();
    if ( *(_DWORD *)(this + 816) )
      sub_6F368E90(this);
    sub_6F76EF80();
    sub_6F184F00(*(_DWORD *)(this + 824), v18, v36, 0);
    sub_6F770670((int)v32, (int)v31, *(_DWORD *)(this + 568));
    if ( dword_6FBE4238 && sub_6F1C3200(dword_6FBE4238) )
      sub_6F26E900(*(_DWORD *)(this + 568));
    if ( *(_DWORD *)(this + 816) )
      sub_6F368E00(this);
    sub_6F184F00(*(_DWORD *)(this + 828), v18, v36, 0);
    sub_6F3702A0(v36);
    sub_6F3B6600(v36);
    sub_6F3B8760(v36);
    if ( *(_DWORD *)(this + 904) )
    {
      *(_DWORD *)(this + 904) = 0;
      *(_DWORD *)(this + 896) = 0;
      *(_DWORD *)(this + 900) = 0;
    }
    sub_6F3AC130();
    v22 = 0;
    v37 = 0;
    if ( *(_DWORD *)(this + 948) )
    {
      v23 = 0;
      do
      {
        v24 = *(_DWORD *)(this + 952);
        if ( *(_DWORD *)(v24 + v23 + 16) )
        {
          v25 = *(_DWORD *)(v24 + v23 + 8);
          if ( v25 )
          {
            sub_6F0CAA90(*(void **)(this + 364), v25, __SPAIR64__(v35, v36), 0);
            v22 = v37;
          }
        }
        ++v22;
        v23 += 24;
        v37 = v22;
      }
      while ( v22 < *(_DWORD *)(this + 948) );
      v18 = v35;
    }
    if ( (*(_BYTE *)(this + 940) & 2) != 0 )
      sub_6F0CAA90(*(void **)(this + 364), *(_DWORD *)(this + 1416), __SPAIR64__(v18, v36), 0);
    v26 = *(_DWORD *)(this + 1420);
    if ( v26 )
      sub_6F0CAA90(*(void **)(this + 364), v26, __SPAIR64__(v18, v36), 0);
    sub_6F378420();
    sub_6F3AC290();
    sub_6F369370(v36, &v33);
    sub_6F0A2AA0(0, 0, v36);
    sub_6F0A2AE0(0, 0, v36);
    *(_DWORD *)(this + 568) = 0;
    v8 = 1;
  }
  else
  {
    v8 = 0;
  }
  v39 = -1;
  sub_6F360270(v34);
  return v8;
}

```

## CWorld_RenderScene (`0x6f3681c0`)

```cpp
// CWorld 主渲染链（asm）：0? -> 1 -> 13 -> Flush -> 19 -> 9 -> 2 -> 3 -> 8 -> (17?) -> 14 -> 5 -> 10 -> (shadow?12) -> 11 -> Flush -> 4 -> 7 -> 6 -> 20 -> (activeQueue==0:15,18,21)
int __thiscall CWorld_RenderScene(int *this)
{
  _DWORD *v2; // ecx
  int v3; // ecx
  void *v4; // edi
  int v5; // eax
  int v6; // eax

  StateCleanup((_DWORD *)*(this + 206));
  StateCleanup((_DWORD *)*(this + 207));
  v2 = (_DWORD *)*(this + 213);
  if ( v2 )
    StateCleanup(v2);
  v3 = *(this + 192);
  *(this + 409) = -1;
  *(this + 408) = -1;
  v4 = (void *)*(this + 199);
  if ( v3 != -1 )
    CWorld_SetShadowMode(v3, 1);
  if ( *(this + 214) && *(this + 213) && *(this + 215) )
    CWorld_DispatchStage(this, 0, 0, 1, 0);
  CWorld_DispatchStage(this, 1, 1, 2, v4);
  CWorld_DispatchStage(this, 13, 1, 2, v4);
  RenderQueue_FlushAndReset((int)v4, (int)this);
  CWorld_DispatchStage(this, 19, 1, 2, v4);
  CWorld_DispatchStage(this, 9, 1, 2, v4);
  CWorld_DispatchStage(this, 2, 1, 2, v4);
  CWorld_DispatchStage(this, 3, 1, 2, v4);
  CWorld_DispatchStage(this, 8, 1, 2, v4);
  if ( *(this + 201) )
    CWorld_DispatchStage(this, 17, 1, 2, v4);
  CWorld_DispatchStage(this, 14, 2, 4, v4);
  CWorld_DispatchStage(this, 5, 2, 4, v4);
  CWorld_DispatchStage(this, 10, 2, 4, v4);
  if ( *(this + 192) != -1 )
  {
    sub_6F3621E0(this, 1);
    CWorld_DispatchStage(this, 12, 2, 4, v4);
  }
  CWorld_DispatchStage(this, 11, 2, 4, v4);
  RenderQueue_FlushAndReset((int)v4, (int)this);
  if ( *(this + 192) != -1 )
    sub_6F3621E0(this, 0);
  CWorld_DispatchStage(this, 4, 1, 2, v4);
  CWorld_DispatchStage(this, 7, 1, 2, v4);
  CWorld_DispatchStage(this, 6, 1, 2, v4);
  CWorld_DispatchStage(this, 20, 2, 4, v4);
  if ( !v4 )
  {
    CWorld_DispatchStage(this, 15, -1, -1, 0);
    CWorld_DispatchStage(this, 18, 2, 4, 0);
    CWorld_DispatchStage(this, 21, -1, -1, 0);
  }
  v5 = *(this + 409);
  if ( v5 != -1 )
  {
    RenderCategory_Disable(this, v5, *(this + 409));
    *(this + 409) = -1;
  }
  v6 = *(this + 408);
  if ( v6 != -1 )
  {
    sub_6F363350((_DWORD **)this, v6, 0);
    *(this + 408) = -1;
  }
  return sub_6F3ACCF0((int)(this + 149));
}

```

## CWorld_DispatchStage (`0x6f363020`)

```cpp
// stageId 分发器（0..21）。注意：stage21 并非传统 UI 含义，而是 TerrainShadowDispatch(13)+收尾链。
int __thiscall RenderWorld_DispatchStage(
        _DWORD *this,
        int stageId,
        int renderMode,
        int categoryMask,
        void *activeQueue)
{
  int currentRenderCategory; // eax
  int v8; // eax
  int result; // eax
  _DWORD *v10; // ecx
  void *v11; // ecx
  int v12; // [esp+0h] [ebp-Ch]
  int v13; // [esp+4h] [ebp-8h]

  if ( activeQueue )
    renderMode = 3;
  currentRenderCategory = *(this + 409);
  // [2] 渲染类别 (Category) 状态机切换
  //      对应 offset 0x664 (409)
  //      作用：比如从“不透明物体”切换到“透明物体”，需要开关深度写入、Alpha混合等
  if ( categoryMask != currentRenderCategory )
  {
    // 如果旧状态不是 -1，先禁用旧状态
    if ( currentRenderCategory != -1 )
      RenderCategory_Disable(this, currentRenderCategory, *(this + 409));
    // 如果新状态不是 -1，启用新状态
    if ( categoryMask != -1 )
      RenderCategory_Enable(this, categoryMask, categoryMask);
    // 更新缓存
    *(this + 409) = categoryMask;
  }
  // [3] 渲染模式 (Mode) 状态机切换
  //      对应 offset 0x660 (408)
  //      作用：比如切换“地形渲染模式” vs “单位渲染模式”
  v8 = *(this + 408);
  if ( renderMode != v8 )
  {
    if ( v8 != -1 )
      sub_6F363350((_DWORD **)this, v8, 0);
    if ( renderMode != -1 )
      sub_6F363350((_DWORD **)this, renderMode, 1);
    *(this + 408) = renderMode;
  }
  result = stageId;
  switch ( stageId )
  {
    case 0:
      sub_6F186300((int *)*(this + 213));
      break;
    case 1:
      result = CWorld_TerrainShadow_Dispatch(0);
      break;
    case 2:
      result = CWorld_TerrainShadow_Dispatch(1);
      break;
    case 3:
      result = CWorld_TerrainShadow_Dispatch(2);
      break;
    case 4:
      result = CWorld_TerrainShadow_Dispatch(3);
      break;
    case 5:
      result = CWorld_TerrainShadow_Dispatch(5);
      break;
    case 6:
      result = CWorld_TerrainShadow_Dispatch(8);
      break;
    case 7:
      result = CWorld_TerrainShadow_Dispatch(9);
      break;
    case 8:
      result = CWorld_TerrainShadow_Dispatch(10);
      break;
    case 9:
      result = CWorld_TerrainShadow_Dispatch(6);
      break;
    case 10:
      result = CWorld_TerrainShadow_Dispatch(4);
      break;
    case 11:
      CWorld_TerrainShadow_Dispatch(12);
      result = CWorld_WorldObjects_RenderGroup(this, renderMode, 0);
      break;
    case 12:
      result = CWorld_WorldObjects_RenderGroup(this, renderMode, 1);
      break;
    case 13:
      result = CWorld_WorldObjects_RenderGroup(this, renderMode, 2);
      break;
    case 14:
      result = CWorld_TerrainShadow_Dispatch(7);
      break;
    case 15:
      result = sub_6F367980();
      break;
    case 16:
      result = dword_6FB66E24;
      if ( (dword_6FB66E24 & 0x10) != 0 )
      {
        sub_6F368A90(this, 0);
        result = dword_6FB66E24;
      }
      if ( (result & 0x200) != 0 )
      {
        sub_6F368A90(this, (_DWORD *)1);
        result = dword_6FB66E24;
      }
      if ( (result & 0x60) != 0 )
      {
        sub_6F368A90(this, (_DWORD *)2);
        result = dword_6FB66E24;
      }
      if ( (result & 0x100) != 0 )
      {
        sub_6F368A90(this, (_DWORD *)3);
        result = dword_6FB66E24;
      }
      if ( (result & 0x80u) != 0 )
        result = sub_6F369560();
      break;
    case 17:
      result = CWorld_TerrainShadow_Dispatch(11);
      break;
    case 18:
      if ( *(this + 146) )
      {
        if ( sub_6F3597C0() )
        {
          v10 = (_DWORD *)*(this + 148);
          if ( v10 )
            sub_6F3C4330(v10);
        }
      }
      result = sub_6F3597C0();
      if ( result )
        result = sub_6F3ACFF0();
      break;
    case 19:
      result = CWorld_TerrainShadow_Dispatch(14);
      break;
    case 20:
      result = CWorld_TerrainShadow_Dispatch(15);
      break;
    case 21:
      result = CWorld_TerrainShadow_Dispatch(13);
      v11 = (void *)*(this + 192);
      if ( v11 != (void *)-1 && *(this + 191) )
        result = sub_6F76F190(v11);
      if ( dword_6FBE4238 )
      {
        result = sub_6F1C3200(dword_6FBE4238);
        if ( result )
          result = sub_6F26C7F0(v12, v13);
      }
      break;
    default:
      return result;
  }
  return result;
}

```

## CWorld_WorldObjects_RenderGroup (`0x6f368e30`)

```cpp
// CWorld 对象组渲染入口（group0/1/2）
int __userpurge WorldObjects_RenderGroup@<eax>(_DWORD *a1@<ecx>, int a2@<edi>, int a3)
{
  int result; // eax
  int v4; // esi
  int v5; // edi
  int i; // esi
  int v7; // [esp-4h] [ebp-8h]
  int v8; // [esp+0h] [ebp-4h]

  result = a3;
  if ( a3 )
  {
    result = a3 - 1;
    if ( a3 == 1 )
    {
      v4 = a1[92];
    }
    else
    {
      result = a3 - 2;
      if ( a3 != 2 )
        return result;
      v4 = a1[93];
    }
  }
  else
  {
    v4 = a1[91];
  }
  if ( v4 )
  {
    v7 = a2;
    v5 = List_GetData(v4);
    result = List_GetCount(v4);
    for ( i = result; i; --i )
    {
      result = WorldObjectEntry_Render(v7, v8);
      v5 += 24;
    }
  }
  return result;
}

```

## WorldObjectEntry_Render (`0x6f184ee0`)

```cpp
int __cdecl sub_6F184EE0(int a1, int a2)
{
  _DWORD *v2; // ecx
  int result; // eax

  if ( v2[8] )
  {
    (*(void (__thiscall **)(_DWORD *))(*v2 + 20))(v2);
    return RenderQueue_AddBatch(a1, a2);
  }
  return result;
}

```

## RenderQueue_AddBatch (`0x6f139190`)

```cpp
void __thiscall RenderQueue_AddBatch(int this)
{
  int v2; // edi
  unsigned int v3; // ebx
  int *v4; // ecx
  char v5; // al
  int v6; // eax
  int v7; // edi
  int v8; // [esp+0h] [ebp-20h]
  int v9; // [esp+4h] [ebp-1Ch]
  int *v10; // [esp+10h] [ebp-10h]

  v2 = *(_DWORD *)(this + 156);
  RenderBatch_Submit((_DWORD *)this);
  if ( (*(_BYTE *)(this + 148) & 0x10) != 0 )
  {
    SceneNode_AddTransparentList0(this, v2);
    SceneNode_AddTransparentList2(this, v2);
    SceneNode_AddTransparentList3(this, v2);
    SceneNode_AddTransparentList4(this);
    v3 = 0;
    if ( *(_DWORD *)(this + 196) )
    {
      v4 = (int *)(*(_DWORD *)(this + 200) + 8);
      v10 = v4;
      do
      {
        if ( !*(_DWORD *)(this + 152)
          || ((v5 = *(_BYTE *)(v3 + *(_DWORD *)(this + 212)), (v5 & 1) == 0)
            ? (v6 = sub_6F777FE0(*(_DWORD *)(this + 152), v3),
               *(_BYTE *)(v3 + *(_DWORD *)(this + 212)) = (v6 != 0 ? 2 : 0) | 1,
               v4 = v10)
            : (int *)(v6 = v5 & 2),
              v6) )
        {
          v7 = *v4;
          if ( *v4 > 0 )
          {
            do
            {
              RenderQueue_AddBatch(v8, v9);
              v7 = *(_DWORD *)(v7 + 4);
            }
            while ( v7 > 0 );
            v4 = v10;
          }
        }
        ++v3;
        v4 += 3;
        v10 = v4;
      }
      while ( v3 < *(_DWORD *)(this + 196) );
    }
  }
}

```

## RenderBatch_Submit (`0x6f1375c0`)

```cpp
void __thiscall sub_6F1375C0(_DWORD *this)
{
  _DWORD *v2; // edi
  _DWORD *v3; // edi
  _DWORD *v4; // esi
  int v5; // eax
  unsigned int v6; // edx
  _DWORD *v7; // eax
  int v8; // esi
  int v9; // eax
  _DWORD *v10; // eax
  size_t v11; // edx
  size_t v12; // esi
  unsigned int v13; // ecx
  int v14; // ecx
  _DWORD *v15; // edi
  _DWORD *v16; // eax
  unsigned int v17; // esi
  bool v18; // cf
  int v19; // [esp-8h] [ebp-3Ch]
  _DWORD *v20; // [esp+14h] [ebp-20h]
  int v21; // [esp+18h] [ebp-1Ch]
  unsigned int v22; // [esp+1Ch] [ebp-18h]
  int v23; // [esp+20h] [ebp-14h]
  _DWORD *v24; // [esp+24h] [ebp-10h]
  int v25; // [esp+28h] [ebp-Ch]
  _DWORD *v26; // [esp+2Ch] [ebp-8h]
  unsigned int v27; // [esp+30h] [ebp-4h]

  v21 = 0;
  v2 = (_DWORD *)*(this + 4);
  v24 = v2;
  if ( *(this + 3) )
  {
    do
    {
      v3 = (_DWORD *)*v2;
      v20 = v3;
      if ( !v3[4] )
      {
        v4 = (_DWORD *)v3[3];
        if ( *(_BYTE *)(*(this + 8) + 16 * v4[71] + 3) )
        {
          v3[5] = this;
          if ( RenderBatch_CanEnqueueToMainQueue(this, v3) )
          {
            v6 = 0;
            v23 = 0;
            v27 = 0;
            v7 = *(_DWORD **)(*(this + 12) + 4 * v4[66]);
            v8 = v7[4];
            v22 = v7[3];
            v9 = *(_DWORD *)(v7[14] + 16);
            if ( v22 )
            {
              v10 = (_DWORD *)(v9 + 28);
              v26 = v10;
              v25 = v8 + 4;
              do
              {
                if ( *(_BYTE *)(*v10 + *(this + 20)) )
                {
                  v11 = g_RenderQueue_NumOfElements;
                  v12 = g_RenderQueue_NumOfElements + 1;
                  if ( g_RenderQueue_NumOfElements + 1 > g_RenderQueue_BatchCapacity )
                  {
                    v13 = g_RenderQueue_BatchGrowStep;
                    if ( !g_RenderQueue_BatchGrowStep )
                      v13 = RenderQueue_ComputeBatchGrowStep(g_RenderQueue_NumOfElements + 1);
                    if ( v12 % v13 )
                      v12 += v13 - v12 % v13;
                    RenderQueue_ReserveBatchArray(v12);
                    v11 = g_RenderQueue_NumOfElements;
                  }
                  v14 = 5 * v11;
                  g_RenderQueue_NumOfElements = v11 + 1;
                  v6 = v27;
                  v15 = (_DWORD *)(g_RenderQueue_BatchArray + 4 * v14);
                  v15[2] = v27;
                  *v15 = v20;
                  v15[4] = v25;
                  v15[1] = 0;
                  if ( *(_DWORD *)(v20[3] + 260) )
                    v15[1] = 1;
                  v15[3] = v23++;
                  if ( v15[3] )
                  {
                    v15[1] |= 2u;
                  }
                  else
                  {
                    v16 = v26;
                    v17 = v27;
                    while ( 1 )
                    {
                      ++v17;
                      v16 += 11;
                      if ( v17 >= v22 )
                        break;
                      if ( *(_BYTE *)(*v16 + *(this + 20)) )
                      {
                        v15[1] |= 2u;
                        break;
                      }
                    }
                    if ( (v15[1] & 1) != 0 )
                      break;
                    v6 = v27;
                  }
                }
                ++v6;
                v25 += 36;
                v10 = v26 + 11;
                v27 = v6;
                v26 += 11;
              }
              while ( v6 < v22 );
            }
          }
          else
          {
            v19 = v4[72];
            v5 = TransformPoint3x4(this + 25);
            AUCTransparent_AddEntry(v5, v19);
          }
        }
      }
      ++v24;
      v18 = (unsigned int)++v21 < *(this + 3);
      v2 = v24;
    }
    while ( v18 );
  }
}

```

## SpriteHost_CreateSpriteAndBindRuntimeModel (`0x6f185250`)

```cpp
_DWORD *__thiscall SpriteHost_CreateSpriteAndBindRuntimeModel(_DWORD *this)
{
  _DWORD *v1; // esi
  size_t *v2; // eax
  size_t *v3; // ebx
  size_t *v4; // eax
  size_t v6; // eax
  size_t v7; // edi
  int v8; // eax
  _DWORD *v9; // eax
  int v10; // eax
  _DWORD *v11; // eax
  int v12; // edi
  int v13; // esi
  char v14; // al
  size_t *v15; // esi

  v1 = this;
  if ( (*(this + 10) & 0x400000) != 0 )
  {
    v2 = JassFrameAllocator_NewFrame(dword_6FBE3D88, 0, "HSPRITEUBER", -2);
    v3 = v2;
    if ( v2 )
    {
      CSpriteUber__ctor(v2);
      *v3 = (size_t)&TAllocatedHandleObjectLeaf<CSpriteUber_,128>::`vftable';
    }
    else
    {
      v3 = 0;
    }
  }
  else
  {
    v4 = JassFrameAllocator_NewFrame(dword_6FBE3D9C, 0, "HSPRITEMINI", -2);
    v3 = v4;
    if ( v4 )
    {
      CSpriteMini__ctor(v4);
      *v3 = (size_t)&TAllocatedHandleObjectLeaf<CSpriteMini_,256>::`vftable';
    }
    else
    {
      v3 = 0;
    }
  }
  if ( !v3 )
    return 0;
  v6 = v1[12];
  if ( v6 )
  {
    ++*(_DWORD *)(v6 + 24);
    v7 = v6;
    if ( v3[12] )
      sub_6F1A2060();
    v3[12] = v7;
  }
  if ( (*(int (__thiscall **)(_DWORD *))(*v1 + 84))(v1) )
  {
    v8 = (*(int (__thiscall **)(_DWORD *))(*v1 + 84))(v1);
    if ( v8 )
      v9 = (_DWORD *)(v8 + 24);
    else
      v9 = 0;
    ++*v9;
    (*(void (__thiscall **)(size_t *, _DWORD *))(*v3 + 92))(v3, v9 - 6);
  }
  if ( (*(int (__thiscall **)(_DWORD *))(*v1 + 88))(v1) )
  {
    v10 = (*(int (__thiscall **)(_DWORD *))(*v1 + 88))(v1);
    if ( v10 )
      v11 = (_DWORD *)(v10 + 24);
    else
      v11 = 0;
    ++*v11;
    (*(void (__thiscall **)(size_t *, _DWORD *))(*v3 + 96))(v3, v11 - 6);
  }
  if ( v1[8] )
  {
    v12 = sub_6F133660();
    v13 = sub_6F133690(v1[8], v12);
    v3[8] = CModelData_PromoteToRuntimeModel(*(this + 8));
    sub_6F12FA70(4);
    sub_6F12F500(v13);
    sub_6F132E90();
    sub_6F12FA50(v3);
    v1 = this;
  }
  v3[24] = (size_t)sub_6F04F200((void *)v1[24]);
  *((_WORD *)v3 + 22) = *((_WORD *)v1 + 22);
  *((_BYTE *)v3 + 52) = *((_BYTE *)v1 + 52);
  *((_BYTE *)v3 + 53) = *((_BYTE *)v1 + 53);
  v14 = *((_BYTE *)v1 + 54);
  v15 = v1 + 14;
  *((_BYTE *)v3 + 54) = v14;
  if ( v3 + 14 != v15 )
    sub_6F184540(v15[1], v15[2]);
  (*(void (__thiscall **)(size_t *, int))(*v3 + 32))(v3, -1);
  return sub_6F04F1C0(v3);
}

```

## CModel_SetWorldMatrixAndEvaluateRootPose (`0x6f12f0a0`)

```cpp
int __fastcall CModel_SetWorldMatrixAndEvaluateRootPose(int a1, const __m128i *a2, float a3, int a4, int a5)
{
  const __m128i *v6; // eax
  const __m128i *v7; // eoff
  int result; // eax

  if ( (float)fabs((float)(a3 - 1.0)) < 0.00000095367432 )
    *(_DWORD *)(a1 + 148) &= ~4u;
  else
    *(_DWORD *)(a1 + 148) |= 4u;
  *(__m128i *)(a1 + 100) = _mm_loadu_si128(a2);
  *(__m128i *)(a1 + 116) = _mm_loadu_si128(a2 + 1);
  *(__m128i *)(a1 + 132) = _mm_loadu_si128(a2 + 2);
  v6 = (const __m128i *)dword_6FBEE648;
  v7 = (const __m128i *)dword_6FBEE648;
  *(__m128i *)(dword_6FBEE648 + 48) = _mm_loadu_si128((const __m128i *)dword_6FBEE648);
  v6[4] = _mm_loadu_si128(v7 + 1);
  v6[5] = _mm_loadu_si128(v6 + 2);
  dword_6FBEE648 += 48;
  sub_6F780120(a2);
  if ( (*(_BYTE *)(a1 + 148) & 0x10) != 0 )
    result = CModelComplex__EvaluatePoseAndVisibleParts(a4, a5);
  else
    result = CModel__EvaluatePoseSimple(a4, a5);
  dword_6FBEE648 -= 48;
  return result;
}

```

## CModel_PropagatePoseToChildRuntimeTree (`0x6f12f7e0`)

```cpp
int __fastcall CModel_PropagatePoseToChildRuntimeTree(int a1, int a2)
{
  const __m128i *v2; // eax
  const __m128i *v4; // eoff
  int v5; // esi
  int result; // eax

  v2 = (const __m128i *)dword_6FBEE648;
  v4 = (const __m128i *)dword_6FBEE648;
  v5 = *(_DWORD *)(a1 + 156);
  *(__m128i *)(dword_6FBEE648 + 48) = _mm_loadu_si128((const __m128i *)dword_6FBEE648);
  v2[4] = _mm_loadu_si128(v4 + 1);
  v2[5] = _mm_loadu_si128(v2 + 2);
  dword_6FBEE648 += 48;
  sub_6F780120(a2);
  result = CModelComplex__RecurseChildRuntimeTree(a1, v5);
  dword_6FBEE648 -= 48;
  return result;
}

```

## CModelComplex__EvaluatePoseAndVisibleParts (`0x6f12e900`)

```cpp
int __fastcall CModelComplex__EvaluatePoseAndVisibleParts(_DWORD *a1, int a2, int a3, int a4)
{
  _DWORD *v5; // ebx
  int v6; // eax
  bool v7; // zf
  int v8; // edi
  int v9; // ebx
  int v10; // esi
  __int64 v11; // rax
  __int64 v12; // rax
  int v13; // esi
  int v14; // eax
  int v15; // eax
  int v16; // edi
  unsigned int v17; // esi
  int v18; // ecx
  unsigned int v19; // eax
  int *v20; // ecx
  char v21; // al
  int v22; // eax
  int v23; // edi
  int v24; // ebx
  _DWORD v26[16]; // [esp+10h] [ebp-60h] BYREF
  int v27; // [esp+50h] [ebp-20h]
  int v28; // [esp+54h] [ebp-1Ch]
  int *v29; // [esp+58h] [ebp-18h]
  unsigned int v30; // [esp+5Ch] [ebp-14h]
  int v31; // [esp+60h] [ebp-10h]

  v29 = (int *)a2;
  v5 = a1;
  v31 = (int)a1;
  v30 = a1[49];
  v6 = sub_6F12E840(v30);
  v7 = v5[38] == 0;
  v28 = v6;
  if ( v7 )
  {
    sub_6F12FF90(v5);
    sub_6F12FF50(v5);
  }
  else
  {
    v8 = sub_6F12E840(*(_DWORD *)(a2 + 108) + *(_DWORD *)(a2 + 84));
    v9 = sub_6F12E8B0(v5);
    v10 = sub_6F12E7B0(v9);
    sub_6F12E820(v10);
    sub_6F12E890(v9);
    *(_DWORD *)(v31 + 160) = sub_6F138FF0(v29[22]);
    v11 = sub_6F12E870(v8);
    v26[0] = v11;
    v26[1] = v29[21];
    v26[2] = sub_6F139060(SHIDWORD(v11));
    v26[3] = v29[22];
    v26[4] = v29 + 23;
    v26[5] = v31 + 180;
    v26[6] = v31 + 216;
    v26[7] = v31 + 228;
    v26[8] = v31 + 240;
    v12 = sub_6F12E870(v28);
    v26[9] = v12;
    v26[10] = v30;
    v26[11] = *(_DWORD *)(HIDWORD(v12) + 32);
    v26[12] = a3;
    v26[13] = a4;
    v26[14] = HIDWORD(v12) + 72;
    v26[15] = sub_6F12E820(v10);
    sub_6F77C260(*(_DWORD *)(v31 + 152), v26);
    memset(*(void **)(v31 + 212), 0, *(_DWORD *)(v31 + 208));
    sub_6F12E820(v10);
    v13 = v31;
    sub_6F12EDE0(v9);
    v14 = sub_6F12E870(v8);
    sub_6F12FED0(v13, v14);
    v15 = sub_6F12E870(v8);
    v16 = (int)v29;
    CModel_CopyResolvedPoseMatricesToOutputPalette(v13, (int)v29, v15);
    sub_6F12FD20(v9);
    sub_6F12FD30(*(_DWORD *)(v16 + 108) + *(_DWORD *)(v16 + 84));
    v5 = (_DWORD *)v13;
  }
  v17 = 0;
  v18 = v5[50];
  v27 = v5[37] & 4;
  v19 = v30;
  if ( v30 )
  {
    v20 = (int *)(v18 + 8);
    v29 = v20;
    do
    {
      if ( !v5[38]
        || ((v21 = *(_BYTE *)(v17 + v5[53]), (v21 & 1) == 0)
          ? (v22 = sub_6F777FE0((_DWORD *)v5[38], v17), *(_BYTE *)(v17 + v5[53]) = (v22 != 0 ? 2 : 0) | 1, v20 = v29)
          : (int *)(v22 = v21 & 2),
            v7 = v22 == 0,
            v19 = v30,
            !v7) )
      {
        v23 = *v20;
        if ( *v20 > 0 )
        {
          v24 = v27;
          do
          {
            sub_6F12F2F0(v24, a3, a4);
            v23 = *(_DWORD *)(v23 + 4);
          }
          while ( v23 > 0 );
          v5 = (_DWORD *)v31;
          v20 = v29;
          v19 = v30;
        }
      }
      ++v17;
      v20 += 3;
      v29 = v20;
    }
    while ( v17 < v19 );
  }
  return sub_6F12FD30(v19);
}

```

## CModel__EvaluatePoseSimple (`0x6f12eb70`)

```cpp
int __fastcall CModel__EvaluatePoseSimple(int a1, int a2, int a3, int a4)
{
  int v5; // edi
  int v6; // ebx
  int v7; // esi
  int v8; // eax
  int v9; // esi
  int v10; // eax
  int v11; // eax
  int v12; // edi
  _DWORD v14[5]; // [esp+4h] [ebp-48h] BYREF
  __int128 v15; // [esp+18h] [ebp-34h]
  int v16; // [esp+28h] [ebp-24h]
  int v17; // [esp+2Ch] [ebp-20h]
  int v18; // [esp+30h] [ebp-1Ch]
  int v19; // [esp+34h] [ebp-18h]
  int v20; // [esp+38h] [ebp-14h]
  int v21; // [esp+3Ch] [ebp-10h]
  int v22; // [esp+40h] [ebp-Ch]
  int v23; // [esp+44h] [ebp-8h]
  int v24; // [esp+48h] [ebp-4h]

  v23 = a2;
  v24 = a1;
  if ( *(_DWORD *)(a1 + 152) )
  {
    v5 = sub_6F12E840(*(_DWORD *)(a2 + 108) + *(_DWORD *)(a2 + 84));
    v6 = sub_6F12E8B0(a1);
    v7 = sub_6F12E7B0(v6);
    sub_6F12E820(v7);
    sub_6F12E890(v6);
    v14[0] = sub_6F12E870(v5);
    v14[2] = 0;
    v14[3] = 0;
    v14[1] = *(_DWORD *)(v23 + 84);
    v14[4] = v23 + 92;
    v15 = 0;
    v16 = 0;
    v18 = *(_DWORD *)(v24 + 32);
    v19 = a3;
    v20 = a4;
    v17 = 0;
    v21 = v24 + 72;
    v8 = sub_6F12E820(v7);
    v9 = v24;
    v22 = v8;
    sub_6F77C260(*(_DWORD *)(v24 + 152), v14);
    v10 = sub_6F12E870(v5);
    sub_6F12FED0(v9, v10);
    v11 = sub_6F12E870(v5);
    v12 = v23;
    CModel_CopyResolvedPoseMatricesToOutputPalette(v9, v23, v11);
    sub_6F12FD20(v6);
    return sub_6F12FD30(*(_DWORD *)(v12 + 108) + *(_DWORD *)(v12 + 84));
  }
  else
  {
    sub_6F12FF90(a1);
    return sub_6F12FF50(a1);
  }
}

```

## CModelComplex__RecurseChildRuntimeTree (`0x6f12ec90`)

```cpp
void __fastcall CModelComplex__RecurseChildRuntimeTree(int a1, int a2)
{
  int v2; // esi
  int v3; // ecx
  unsigned int v4; // ebx
  const __m128i *v5; // ecx
  int v6; // edx
  char v7; // al
  int v8; // eax
  int v9; // ecx
  int v10; // esi
  int v11; // ebx
  int v12; // edi
  unsigned int v13; // [esp+4h] [ebp-10h]
  unsigned int v15; // [esp+Ch] [ebp-8h]
  int v16; // [esp+10h] [ebp-4h]

  v2 = a1;
  v3 = *(_DWORD *)(a1 + 152);
  if ( v3 )
    sub_6F77C280(v3, a2 + 92);
  if ( (*(_BYTE *)(v2 + 148) & 0x10) != 0 )
  {
    v4 = 0;
    v13 = *(_DWORD *)(v2 + 196);
    v15 = 0;
    if ( v13 )
    {
      v5 = (const __m128i *)dword_6FBEE648;
      v6 = 0;
      v16 = 0;
      do
      {
        if ( !*(_DWORD *)(v2 + 152)
          || ((v7 = *(_BYTE *)(v4 + *(_DWORD *)(v2 + 212)), (v7 & 1) == 0)
            ? (v8 = sub_6F777FE0(*(_DWORD **)(v2 + 152), v4),
               *(_BYTE *)(v4 + *(_DWORD *)(v2 + 212)) = (v8 != 0 ? 2 : 0) | 1,
               v5 = (const __m128i *)dword_6FBEE648,
               v6 = v16)
            : (v8 = v7 & 2),
              v8) )
        {
          if ( *(int *)(v6 + *(_DWORD *)(v2 + 200) + 8) > 0 )
          {
            v5[3] = _mm_loadu_si128(v5);
            v5[4] = _mm_loadu_si128(v5 + 1);
            v5[5] = _mm_loadu_si128(v5 + 2);
            v9 = dword_6FBEE648 + 48;
            dword_6FBEE648 += 48;
            v10 = *(_DWORD *)(v6 + *(_DWORD *)(v2 + 200) + 8);
            if ( v10 > 0 )
            {
              do
              {
                v11 = *(_DWORD *)(v10 + 8);
                v12 = *(_DWORD *)(v11 + 156);
                sub_6F780120(v11 + 100);
                CModelComplex__RecurseChildRuntimeTree(v11, v12);
                v10 = *(_DWORD *)(v10 + 4);
              }
              while ( v10 > 0 );
              v9 = dword_6FBEE648;
              v4 = v15;
              v6 = v16;
            }
            v2 = a1;
            v5 = (const __m128i *)(v9 - 48);
            dword_6FBEE648 = (int)v5;
          }
        }
        ++v4;
        v6 += 12;
        v15 = v4;
        v16 = v6;
      }
      while ( v4 < v13 );
    }
  }
}

```

## CModelComplex__BuildChildRuntimeModelLinks (`0x6f131f60`)

```cpp
int *__thiscall CModelComplex__BuildChildRuntimeModelLinks(int this, _DWORD *a2)
{
  int v3; // edi
  int v4; // ecx
  int *result; // eax
  int v6; // edi
  int v7; // ebx
  _DWORD *v8; // eax
  _DWORD *v9; // esi
  int *v10; // edx
  int v11; // edi
  int v12; // ecx
  int v13; // ecx
  int v14; // ecx
  int v15; // [esp+Ch] [ebp-8h]
  int *v16; // [esp+10h] [ebp-4h]
  int *v17; // [esp+1Ch] [ebp+8h]

  sub_6F136700(a2[49]);
  v3 = a2[52];
  if ( v3 != *(_DWORD *)(this + 208) )
  {
    if ( v3 )
    {
      sub_6F0355E0(v3);
      *(_DWORD *)(this + 208) = v3;
    }
    else
    {
      if ( *(_DWORD *)(this + 212) )
        Storm_403_Free();
      *(_DWORD *)(this + 204) = 0;
      *(_DWORD *)(this + 208) = 0;
      *(_DWORD *)(this + 212) = 0;
    }
  }
  memset(*(void **)(this + 212), 0, *(_DWORD *)(this + 208));
  v4 = *(_DWORD *)(this + 196);
  result = (int *)a2[50];
  if ( v4 )
  {
    v6 = *(_DWORD *)(this + 200) + 4;
    result += 2;
    v16 = (int *)v6;
    v17 = result;
    do
    {
      v7 = *result;
      v15 = --v4;
      if ( *result > 0 )
      {
        do
        {
          v8 = (_DWORD *)AllocMemory_Storm_401(16, (int)aAulinkunique, -2, 8);
          v9 = v8;
          if ( v8 )
          {
            *v8 = 0;
            v10 = v8;
            v8[1] = 0;
            v8[2] = 0;
            v8[3] = 0;
          }
          else
          {
            v9 = 0;
            v10 = (int *)v6;
          }
          v11 = *v10;
          if ( *v10 )
          {
            v12 = v10[1];
            if ( v12 > 0 )
              v13 = (int)v10 + v12 - *(_DWORD *)(v11 + 4);
            else
              v13 = ~v12;
            *(_DWORD *)v13 = v11;
            *(_DWORD *)(*v10 + 4) = v10[1];
            *v10 = 0;
            v10[1] = 0;
          }
          v6 = (int)v16;
          v14 = *v16;
          *v10 = *v16;
          v10[1] = *(_DWORD *)(v14 + 4);
          *(_DWORD *)(v14 + 4) = v9;
          *v16 = (int)v10;
          v9[2] = CModelData_PromoteToRuntimeModel(*(_BYTE **)(v7 + 8));
          v9[3] = *(_DWORD *)(v7 + 12);
          v7 = *(_DWORD *)(v7 + 4);
        }
        while ( v7 > 0 );
        v4 = v15;
        result = v17;
      }
      result += 3;
      v6 += 12;
      v17 = result;
      v16 = (int *)v6;
    }
    while ( v4 );
  }
  return result;
}

```

## CGeoset_CreateFromRawArrays (`0x6f126250`)

```cpp
_DWORD *__fastcall sub_6F126250(int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8)
{
  size_t *v9; // eax
  size_t *v10; // esi
  size_t *v11; // eax
  int v12; // eax
  _DWORD *v13; // ebx
  int i; // eax
  _DWORD *v15; // ecx

  v9 = JassFrameAllocator_NewFrame(dword_6FBC6A8C, 0, "HGEOSET", -2);
  v10 = v9;
  if ( !v9 )
    return 0;
  v9[1] = 0;
  *v9 = (size_t)&CGeoset::`vftable';
  v9[2] = -1;
  v9[3] = 0;
  v9[4] = 0;
  v9[5] = 0;
  v11 = JassFrameAllocator_NewFrame(dword_6FBC6A78, 0, "HGEOSETDATA", -2);
  if ( v11 && (v12 = sub_6F1216A0(v11), (v13 = (_DWORD *)v12) != 0) )
  {
    *(_DWORD *)(v12 + 264) = a8;
    sub_6F12CFB0(a1, a2);
    sub_6F12CFB0(a1, a3);
    sub_6F12D710(1);
    sub_6F12CF20(a1, a4);
    if ( !v13[50] )
    {
      if ( !v13[49] )
        sub_6F12C390(1);
      for ( i = v13[50]; !i; ++i )
      {
        v15 = (_DWORD *)(v13[51] + 8 * i);
        if ( v15 )
        {
          *v15 = 3;
          v15[1] = 0;
        }
      }
    }
    v13[50] = 1;
    *(_DWORD *)v13[51] = a5;
    *(_DWORD *)(v13[51] + 4) = a7;
    sub_6F12CEA0(a7, a6);
    v10[3] = (size_t)sub_6F04F1C0(v13);
    return sub_6F04F1C0(v10);
  }
  else
  {
    (*(void (__thiscall **)(size_t *))(*v10 + 4))(v10);
    return 0;
  }
}

```

## CModelData_CreateOwnedHandleForHost (`0x6f127610`)

```cpp
int __fastcall CModelData_CreateOwnedHandleForHost(int a1, int a2)
{
  size_t *v3; // eax
  _DWORD *v4; // eax

  if ( !a2 )
    return 0;
  v3 = JassFrameAllocator_NewFrame(dword_6FBC6AA0, 0, "HMODELDATA", -2);
  if ( !v3 )
    return 0;
  v4 = (_DWORD *)CModelData__ctor(v3);
  if ( !v4 )
    return 0;
  *(_DWORD *)(a2 + 156) = sub_6F04F1C0(v4);
  return 1;
}

```

## CModel_CreateWithOwnedModelData (`0x6f12a400`)

```cpp
_DWORD *__fastcall CModel_CreateWithOwnedModelData(int a1, int a2)
{
  size_t *v3; // esi
  size_t *v4; // eax
  int v5; // eax
  _DWORD *v6; // edi

  v3 = JassFrameAllocator_NewFrame(dword_6FBC6B44, 0, "HMODEL", -2);
  if ( !v3 )
    return 0;
  CModel__ctor(0);
  *v3 = (size_t)&TAllocatedHandleObjectLeaf<CModel,256>::`vftable';
  v4 = JassFrameAllocator_NewFrame(dword_6FBC6AA0, 0, "HMODELDATA", -2);
  if ( v4 && (v5 = CModelData__ctor(v4), (v6 = (_DWORD *)v5) != 0) )
  {
    *(_DWORD *)(v5 + 84) = 1;
    if ( a2 + v3[3] > v3[2] )
      sub_6F12B9B0(a2 + v3[3]);
    if ( a2 + v3[11] > v3[10] )
      sub_6F12BA40(a2 + v3[11]);
    v3[39] = (size_t)sub_6F04F1C0(v6);
    return sub_6F04F1C0(v3);
  }
  else
  {
    (*(void (__thiscall **)(size_t *))(*v3 + 4))(v3);
    return 0;
  }
}

```

## SpriteHost_CreateSpriteAndBindSourceObject (`0x6f6bd110`)

```cpp
int __thiscall sub_6F6BD110(_DWORD *this, _DWORD *a2, char a3, int a4, int a5, __int16 a6)
{
  _DWORD *v7; // edi
  _DWORD *v8; // esi
  int v9; // eax
  int v10; // edx
  __m128i v11; // xmm0
  __m128i v12; // xmm0
  _DWORD *v13; // esi
  int v14; // edi
  int v15; // eax
  int valid; // eax
  int v17; // eax
  int v18; // eax
  int v19; // edx
  int v20; // ecx
  void *v21; // ecx
  int v22; // eax
  bool v23; // zf
  int v24; // eax
  int result; // eax
  __m128i v26; // [esp-20h] [ebp-78h]
  float v27; // [esp+0h] [ebp-58h]
  __int128 v28; // [esp+14h] [ebp-44h] BYREF
  __int128 v29; // [esp+24h] [ebp-34h] BYREF
  int v30; // [esp+34h] [ebp-24h]
  __m128i v31; // [esp+38h] [ebp-20h] BYREF
  int v32; // [esp+48h] [ebp-10h]
  int v33; // [esp+54h] [ebp-4h]

  v7 = a2;
  v8 = (_DWORD *)sub_6F6A0AD0(a2);
  if ( !a2 )
    goto LABEL_7;
  v9 = sub_6F037350(a2[3], a2[4]);
  if ( !v9 )
    goto LABEL_7;
  v10 = 0;
  if ( *(_DWORD *)(v9 + 12) == 727803756 )
    v10 = v9;
  if ( v10 )
  {
    *(this + 8) = *(_DWORD *)(v10 + 20);
    *(this + 9) = *(_DWORD *)(v10 + 24);
  }
  else
  {
LABEL_7:
    *(this + 9) = -1;
    *(this + 8) = -1;
  }
  *(this + 11) = SpriteHost_CreateSpriteAndBindRuntimeModel(v8);
  memset(&v31, 0, 12);
  sub_6F185A10(v8, &v31);
  v11 = _mm_loadl_epi64(&v31);
  sub_6F186F10(v11.m128i_i8[0], v11.m128i_i32[1], v31.m128i_i32[2]);
  sub_6F185220(*(this + 11), v8);
  v27 = sub_6F185A40(v8);
  sub_6F186F20(LODWORD(v27));
  v28 = xmmword_6F95AC20;
  v30 = 1065353216;
  v29 = xmmword_6F95AC20;
  sub_6F1859D0(v8, &v28);
  v26 = _mm_loadu_si128((const __m128i *)&v28);
  v12 = _mm_loadu_si128((const __m128i *)&v29);
  sub_6F186EC0(
    v26.m128i_i8[0],
    v26.m128i_i32[1],
    v26.m128i_i32[2],
    v26.m128i_i32[3],
    v12.m128i_i32[0],
    v12.m128i_i32[1],
    v12.m128i_i32[2],
    v12.m128i_i32[3],
    v30);
  sub_6F185A60(v8);
  sub_6F186F80(1);
  sub_6F186D40(1);
  sub_6F03B630(&a3);
  sub_6F03F350(1);
  sub_6F03F480(1);
  sub_6F03EDD0(this + 12);
  v13 = 0;
  v31.m128i_i32[3] = 0;
  v33 = 0;
  if ( a2 )
  {
    v14 = sub_6F667470();
    v15 = (*(int (__thiscall **)(_DWORD *))(*a2 + 28))(a2);
    valid = IsValidReferenceCount(v15, v14);
    v7 = a2;
    if ( valid )
    {
      ++a2[1];
      v13 = a2;
      v31.m128i_i32[3] = (__int32)a2;
    }
  }
  v33 = 1;
  if ( v13 && sub_6F66EA60(1) )
    v17 = 74;
  else
    v17 = (*(int (__thiscall **)(_DWORD *))(*v7 + 352))(v7);
  *(this + 21) = v17;
  v18 = sub_6F691240(v7);
  v32 = -1;
  *(this + 17) = -1;
  *(this + 22) = v18 & 0xFCFFFFFF | 0x1000000;
  if ( v13 )
  {
    *(this + 23) = v13[146];
    *((_WORD *)this + 36) = (*(int (__thiscall **)(_DWORD *))(*v13 + 236))(v13);
    *((_WORD *)this + 36) |= sub_6F66E8F0(v13) != 0 ? 0x100 : 0;
    v19 = *((unsigned __int16 *)this + 36) | (((v13[24] >> 8) & 1) << 13);
    *((_WORD *)this + 36) = v19;
    v20 = v19 | (((v13[24] >> 7) & 1) << 14);
    *((_WORD *)this + 36) = v20;
    *((_WORD *)this + 36) = v20 | (((v13[23] >> 18) & 1) << 9);
    *((_WORD *)this + 36) |= sub_6F66EA60(0) != 0 ? 0x1000 : 0;
    v21 = (void *)v13[180];
    if ( v21 != (void *)-1 )
      v32 = sub_6F76DAF0(v21);
    *(this + 20) = v13[12];
  }
  else
  {
    *(this + 23) = 0;
    *((_WORD *)this + 36) = 1024;
    sub_6F1D3600(v7);
    LOBYTE(v33) = 2;
    if ( a2 )
      v22 = a2[8] & 0x100;
    else
      v22 = 0;
    *((_WORD *)this + 36) |= v22 != 0 ? 0x800 : 0;
    *(this + 20) = v7[12];
    *(this + 24) = sub_6F6364C0(v7);
    LOBYTE(v33) = 1;
    if ( a2 )
    {
      v23 = a2[1]-- == 1;
      if ( v23 )
        (*(void (__thiscall **)(_DWORD *))*a2)(a2);
    }
  }
  v24 = (*(int (__thiscall **)(_DWORD *))(*v7 + 368))(v7);
  *(this + 16) = v24;
  if ( v24 != -2 && v24 != -3 )
    *(this + 16) = 1;
  *((_WORD *)this + 37) = a6;
  (*(void (__thiscall **)(_DWORD *))(*this + 64))(this);
  result = v32;
  *(this + 17) = v32;
  v33 = -1;
  if ( v13 )
  {
    v23 = v13[1]-- == 1;
    if ( v23 )
      return (*(int (__thiscall **)(_DWORD *))*v13)(v13);
  }
  return result;
}

```

## CModel_CopyResolvedPoseMatricesToOutputPalette (`0x6f12fdc0`)

```cpp
int __fastcall CModel_CopyResolvedPoseMatricesToOutputPalette(int a1, int a2, int a3)
{
  int v3; // eax
  int v4; // edx
  int v5; // ecx
  int result; // eax

  v3 = *(_DWORD *)(a2 + 84) - *(_DWORD *)(a2 + 108);
  v4 = *(_DWORD *)(a1 + 96);
  v5 = *(_DWORD *)(a1 + 92);
  for ( result = a3 + 48 * v3; v5; --v5 )
  {
    v4 += 48;
    result += 48;
    *(__m128i *)(v4 - 48) = _mm_loadu_si128((const __m128i *)(result - 48));
    *(__m128i *)(v4 - 32) = _mm_loadu_si128((const __m128i *)(result - 32));
    *(__m128i *)(v4 - 16) = _mm_loadu_si128((const __m128i *)(result - 16));
  }
  return result;
}

```

## CSpriteUber__PreRenderAndUpdatePosePalette (`0x6f182300`)

```cpp
int __thiscall CSpriteUber__PreRenderAndUpdatePosePalette(int this, float a2, int a3, unsigned int a4, int a5)
{
  int v6; // eax
  int v7; // eax
  bool v8; // zf
  int v9; // esi
  int v10; // eax
  int v11; // ecx
  int result; // eax
  void *v13; // esi
  int v14; // eax
  int v15; // ecx
  float v16; // xmm1_4
  __int128 v17; // [esp+44h] [ebp-58h] BYREF
  __int64 v18; // [esp+68h] [ebp-34h]
  int v19; // [esp+70h] [ebp-2Ch]
  _DWORD v20[3]; // [esp+74h] [ebp-28h] BYREF
  _DWORD v21[3]; // [esp+80h] [ebp-1Ch] BYREF
  _DWORD v22[3]; // [esp+8Ch] [ebp-10h] BYREF
  int v23; // [esp+98h] [ebp-4h]

  if ( *(_WORD *)(this + 44) == 0xFFFE )
    sub_6F183A30();
  sub_6F18F030(LODWORD(a2));
  if ( !*(_DWORD *)(this + 32) || (*(_DWORD *)(this + 40) & 0x10000) != 0 )
    return 0;
  sub_6F137170(this + 264);
  v6 = *(_DWORD *)(this + 200);
  v18 = *(_QWORD *)(this + 192);
  v19 = v6;
  v7 = sub_6F139AE0(0);
  v8 = *(_DWORD *)(this + 148) == 0;
  v9 = v7;
  v23 = v7;
  if ( v8 )
  {
    sub_6F12FB80(*(_DWORD *)(this + 400));
    v10 = *(_DWORD *)(this + 40);
    v11 = *(_DWORD *)(this + 32);
    if ( (v10 & 0x20000) != 0 )
    {
      if ( !sub_6F12EE90(v11) )
        return 0;
    }
    else if ( (v10 & 0x40000) != 0 )
    {
      result = sub_6F12FAA0(v11, dword_6FBE3D70);
      if ( !result )
        return result;
    }
    else
    {
      result = sub_6F12EF70(v11, (int)(float)(a2 * 1000.0));
      if ( !result )
        return result;
    }
  }
  else
  {
    sub_6F12F500((int)(float)(*(float *)(this + 160) * 1000.0));
  }
  CModel_BuildPartStateAndScratchPoseTree(*(_DWORD *)(this + 32), &v17);
  if ( v9 || a5 )
  {
    *(_DWORD *)(this + 40) |= 0x200000u;
    v22[0] = 1065353216;
    v22[1] = 1065353216;
    v22[2] = 1065353216;
    memset(v21, 0, sizeof(v21));
    if ( a3 )
    {
      sub_6F18EA90(v22);
      sub_6F18EA90(v21);
    }
    else if ( a4 < sub_6F133600(*(_DWORD *)(this + 32)) )
    {
      v13 = (void *)sub_6F133540(*(_DWORD *)(this + 32));
      sub_6F18EA90(v22);
      sub_6F18EA90(v21);
      sub_6F04F1A0(v13);
    }
    v20[0] = *(_DWORD *)(this + 232);
    v20[1] = v20[0];
    v20[2] = v20[0];
    sub_6F1AB240(v20);
    CModel_SetWorldMatrixAndEvaluateRootPose(*(_DWORD *)(this + 232), v22, v21);
    if ( *(int *)(this + 116) >= 0 )
    {
      v14 = *(_DWORD *)(this + 40);
      v15 = (v14 & 0x10000000) != 0;
      if ( (v14 & 0x20000000) != 0 )
        v15 = 2;
      if ( (v14 & 0x40000000) != 0 )
        v15 = 4;
      sub_6F12F4C0(this + 360, v15);
    }
    v9 = v23;
  }
  v16 = fabs((float)(a2 - 0.0));
  if ( v16 >= 0.00000023841858 )
    CModel_PropagatePoseToChildRuntimeTree(*(_DWORD *)(this + 32), &v17);
  return v9;
}

```

## CSpriteUber__LightUpdateAndPropagatePose (`0x6f1826c0`)

```cpp
int __thiscall CSpriteUber__LightUpdateAndPropagatePose(int this, float a2)
{
  int result; // eax
  int v4; // eax
  int v5; // ecx
  int v6; // eax
  float v7; // xmm1_4
  _BYTE v8[36]; // [esp+8h] [ebp-30h] BYREF
  __int64 v9; // [esp+2Ch] [ebp-Ch]
  int v10; // [esp+34h] [ebp-4h]

  if ( *(_WORD *)(this + 44) == 0xFFFE )
    sub_6F183A30();
  result = sub_6F18F030(LODWORD(a2));
  if ( *(_DWORD *)(this + 32) && (*(_DWORD *)(this + 40) & 0x10000) == 0 )
  {
    if ( *(_DWORD *)(this + 148) )
    {
      sub_6F12F500((int)(float)(*(float *)(this + 160) * 1000.0));
    }
    else
    {
      sub_6F12FB80(*(_DWORD *)(this + 400));
      v4 = *(_DWORD *)(this + 40);
      v5 = *(_DWORD *)(this + 32);
      if ( (v4 & 0x20000) != 0 )
      {
        sub_6F12EE90(v5);
      }
      else if ( (v4 & 0x40000) != 0 )
      {
        sub_6F12FAA0(v5, dword_6FBE3D70);
      }
      else
      {
        sub_6F12EF70(v5, (int)(float)(a2 * 1000.0));
      }
    }
    sub_6F137170(this + 264);
    v6 = *(_DWORD *)(this + 200);
    v9 = *(_QWORD *)(this + 192);
    v10 = v6;
    v7 = fabs((float)(a2 - 0.0));
    result = v7 < 0.00000023841858;
    if ( v7 >= 0.00000023841858 )
      return CModel_PropagatePoseToChildRuntimeTree(*(_DWORD *)(this + 32), v8);
  }
  return result;
}

```

## CModelComplex__ctor (`0x6f1219c0`)

```cpp
_DWORD *__thiscall CModelComplex__ctor(_DWORD *this)
{
  CModel__ctor(this, 16);
  *this = &CModelComplex_::`vftable';
  *(this + 40) = -1;
  *(this + 41) = 0;
  *(this + 42) = 0;
  *(this + 43) = 0;
  *(this + 44) = 0;
  *(this + 45) = 0;
  *(this + 46) = 0;
  *(this + 47) = 0;
  *(this + 48) = 0;
  *(this + 49) = 0;
  *(this + 50) = 0;
  *(this + 51) = 0;
  *(this + 52) = 0;
  *(this + 53) = 0;
  *(this + 54) = 0;
  *(this + 55) = 0;
  *(this + 56) = 0;
  *(this + 57) = 0;
  *(this + 58) = 0;
  *(this + 59) = 0;
  *(this + 60) = 0;
  *(this + 61) = 0;
  *(this + 62) = 0;
  *(this + 63) = 0;
  *(this + 64) = 0;
  *(this + 65) = 0;
  *(this + 66) = 0;
  *(this + 67) = 0;
  *(this + 68) = 0;
  return this;
}

```

## CModelComplex__CopyFromModelData (`0x6f130d90`)

```cpp
_DWORD *__thiscall CModelComplex__CopyFromModelData(_DWORD *this, _DWORD *a2)
{
  sub_6F130CD0(this, (int)a2);
  CModelComplex__BuildChildRuntimeModelLinks(a2);
  sub_6F1320D0(a2);
  sub_6F1322B0(a2);
  sub_6F132190(a2);
  if ( this + 54 != a2 + 54 )
    sub_6F1361C0(a2[55], a2[56]);
  if ( this + 60 != a2 + 60 )
    sub_6F136250(a2[61], a2[62]);
  return this;
}

```

