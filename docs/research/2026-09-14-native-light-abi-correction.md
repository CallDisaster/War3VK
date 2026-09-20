# Native FFP ABI correction and IDA annotation

Pinned Game.dll: E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A.
Preferred addresses below are RVA+6F000000, not an ASLR load assumption.

- 6F0EF2D0 is thiscall `(device, slot, D3DLIGHT9*, enabled)`, EAX returned
  unchanged by the hook. The native cache copies **0x68 = 104 bytes**, 26 DWORDs.
  The older IDA comments' `68-byte` wording is incorrect in decimal. Its
  arithmetic checksum is not an identity proof; compare all 104 bytes with the
  actual D3D9 frontend state after independently proving native ownership.
- 6F0EFB60 uses 36-byte native copied slots, creates D3DLIGHT9 Range=10000 and
  global device attenuation. Enhanced MDX range comes from independently
  parsed and hash-pinned source, not those globals or CGxuLight selection score.
- 6F0F01E0 converts packed colors: output R=byte2*scale, G=byte1*scale,
  B=byte0*scale, A=byte3*scale. This confirms 0xAARRGGBB; EFB60 supplies
  intensity/255. NativeValue's direct RGB/intensity remain animation-driven.

IDA repeatable function comments before append:

6F0EF2D0:
`[WarVK ModelSystem 2026-09-13] CONFIRMED COM calls vtable+CC SetLight and +D4
LightEnable, caching 68-byte light description. Native lighting already
contributes; enhanced pass must avoid double illumination.`
The existing previous-name/evidence suffix is retained unchanged.

6F0EFB60:
`[WarVK ModelSystem 2026-09-13] CONFIRMED constructs 68-byte D3DLIGHT9 for up to
8 slots. Range=10000, Attenuation0=1; Attenuation1/2 from device+36C/+370. Not
per-model MDX start/end. 0EAD80 is separate OpenGL counterpart.`
The existing previous-name/evidence suffix is retained unchanged.

6F0F01E0: no previous repeatable function comment.

Only append the dated correction to these three function comments. No names,
types, instruction bytes, function bounds or game files change.
