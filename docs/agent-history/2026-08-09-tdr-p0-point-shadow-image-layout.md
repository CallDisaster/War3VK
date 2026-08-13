# TDR P0: point-shadow cube image layout ownership

## Audit result

The point-shadow image declares both sampled and depth-attachment usage, and its
CubeArray sampling view and per-face attachment views already declare the uses
they actually perform.  However, the rendering path maintained only one boolean
for the entire image.  Its manual Vulkan barriers did not update
`DxvkImage::trackLayout`, so DXVK's subresource tracker could continue to report
the recreated image as `UNDEFINED` after face rendering had moved it between
attachment and read-only layouts.

The 1x1 neutral fallback had a second concrete issue: it transitioned from
`UNDEFINED` directly to read-only and was sampled without ever defining its
depth contents.

There is no separate point-shadow staging cube in the current implementation.
The scheduled face updates are recorded into the publication cube itself, with
CPU publication withheld until the face-validity contract is complete.  This
change does not introduce a staging resource or alter that scheduling contract.

## Change

- Each of the 24 possible cube faces now owns an independent
  `War3OwnedImageLayoutState`.
- A newly allocated CubeArray establishes read-only layout over the complete
  descriptor range from `UNDEFINED/NONE/0`.
- Only faces named by the existing update mask transition to depth attachment;
  successful and exceptional exits return exactly those faces to read-only.
- Every transition uses ignored queue-family indices and commits the exact face
  range to `DxvkImage::trackLayout`.
- Recreating the cube resets every face state only after the replacement image
  and all views have been created successfully.
- The neutral cube now declares transfer-destination usage, transitions from
  `UNDEFINED` to transfer destination, clears all six faces to depth 1.0, and
  then transitions to read-only before its first sampling use.

Point-shadow depth encoding, receiver bias, PCF, face selection, temporal reuse,
worker scheduling and publication identity are unchanged.

## Verification boundary

The value-only Win32 runnable now verifies independent cube-face histories in
addition to first-use and ordinary transition semantics.  A dedicated static
contract checks image/view usage, recreation reset, face write/read transitions,
and neutral-depth initialization.

- all 79 static-test scripts passed;
- all 21 Win32 Meson runnable tests passed;
- the Win32 DLL linked successfully and `ninja -C build32 -n` reported no work;
- `git diff --check` passed;
- `d3d9.dll` is 34,132,624 bytes with SHA-256
  `8E09D9355B62B7CE0BD5A94251633729108C4AF334BA0030547DF5EA27FC6EBF`.

No DLL is deployed and no game process is started by this branch.
