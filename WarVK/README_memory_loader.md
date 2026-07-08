# WarVK Non-Lua Loader Notes

This note records the clean-room boundary for a future non-Lua WarVK loader.

## What the old MemHack-style JASS does

The old generated `war3map.j` path is not a small DLL-load helper. It builds a
general memory execution bridge:

- declares reserved Warcraft III natives as call carriers;
- derives process memory read/write helpers;
- patches native dispatch/call slots;
- allocates executable memory;
- writes tiny call stubs into that memory;
- resolves `kernel32.dll` exports such as `GetProcAddress` and `LoadLibraryA`;
- optionally exports a DLL-like payload from MPQ, then calls the loader.

That design works because it first creates arbitrary memory and function-call
capability inside the Warcraft III process. It is powerful, but it is also much
larger than WarVK needs and too close to MemHack/JapiFunc territory to copy.

## WarVK decision

WarVK keeps the default path clean and narrow:

```jass
call Cheat("exec-lua:warvk_loader")
```

The Lua loader is WarVK-owned and only calls `LoadLibraryA("WarVK.dll")`, then
`WarVK_Initialize` or `Initialize`.

The JASS API exposes `WarVK_RequestDllLoadByMemory()` only as an explicit
placeholder. It returns `false` and explains that the non-Lua backend is not
implemented.

## Future clean-room backend

If Lua is unavailable and WarVK still needs map-side loading, implement a
dedicated WarVK loader from scratch with these constraints:

- target Warcraft III 1.27a first;
- derive every address and primitive from IDA notes, not from MemHack source;
- implement only `LoadLibraryA(WarVK.dll)` plus `WarVK_Initialize`;
- do not expose general `ReadRealMemory`, `WriteRealMemory`, `VirtualAlloc`, or
  arbitrary call helpers as public map APIs;
- do not reuse MemHack native carrier choices, MPQ names, payload names, or
  function layout.

The goal is a minimal WarVK bootstrapper, not a second MemHack.
