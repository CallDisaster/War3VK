# Native model light: FFP handoff, not yet a consumer

Source audit only. Game.dll E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A,
13,187,048 bytes, x86, IDA base 6F000000. The candidate DLL does NOT yet
automatically take over model lights or authorize native-model cube shadows.

## Newly closed native boundary

| RVA | Independently checked dataflow |
| --- | --- |
| 0CC650 | SelectForSpatialBucket caches selected **CGxuLight pointers** per spatial bucket. Calls E3410(index, pointer), then E33F0(index, 0) to disable remaining slots. Selection cache can skip those calls when bucket/dirty version is unchanged. |
| 0E3410 | fastcall ECX=slot, EDX=CGxuLight pointer. Dispatches `gx_device` virtual slot +84. |
| 0E5690 | Actual D3D9 vtable +84 (table 95D87C). thiscall ECX=device, stack slot/light. Copies exactly nine DWORDs / 36 bytes to device+22C+slot*36; compares them with cached device+3A8+slot*36 and updates dirty slot. Source pointer is NOT retained. |
| 0E5660 | D3D9 vtable +88. thiscall ECX=device, stack slot/enabled. Sets copied enabled and compares cached enabled. |
| 0EFB60 | FlushFixedFunctionLights builds D3DLIGHT9 from the copied value slots, uses Range=10000 and device-global attenuation, then E F2D0 cached SetLight/LightEnable. |

Consequences:

- A D3D9 SetLight hook is too late to infer a native model identity: the owning
  CGxuLight pointer is already lost. Matching only position/color is ambiguous.
- E3410 is a prospective exact identity-to-value handoff; E5690 proves a local
  value copy could be consumed synchronously. No new hook installed here.
- A takeover must respect native bucket caching. A one-call suppression can
  otherwise persist after the enhanced lease has expired; clearing candidate
  TLS alone does not restore the native copied slot.
- Removing all point lights, or adding enhanced lights while leaving their
  original direct contribution, is NOT accepted. Preserve sun, ambient,
  unknown/native-unclaimed lights and all author-owned JAPI lights.
- Before native direct light can be suppressed, the exact enhanced receiver
  must be committed for the same frame, or a restoring fallback must exist.
  A later failed receiver pass cannot retroactively repair earlier native draws.

## Cold producer / lifetime obligations remain

048C10 returns the actually read bytes; 81AB30 controls requested/fallback path,
calls binary parser 819A80 and frees the bytes. 1261D0 builds a runtime template;
13DAA0 maps parsed 376-byte light records to template outputs. Existing 130D90
hook clones into the instance; 1307B0 destroys instance outputs. 77F2D0 evaluates
node outputs using slot index, not MDX ObjectID. +28 remains selection score,
not attenuation radius. All are read-only findings, not running producer proof.

The frozen nine-path / eleven-version policy still needs successful-load hash,
document/template/instance generations, ObjectID-to-output mapping, current
world/frame evidence, deletion/reset invalidation, owned renderer publication,
at most two automatic cube shadows with manual lights retaining priority, and
an actual native-light fixture with occlusion/disable/delete/fallback checks.
Synthetic JAPI lights cannot substitute for that fixture.

## IDA writeback receipt

Before this checkpoint the repeatable function comments at 6F0E3410, 6F0E5690,
6F0E5660 were empty; names were sub_6F0E3410/sub_6F0E5690/sub_6F0E5660.
Only explanatory repeatable comments are appended; no native bytes, types,
function boundaries or names are changed. This note preserves the previous
empty values so the annotation update is reversible.
