# D3D9 indexed range and WarVK terrain shadow bounds

## Primary contract

Microsoft documents `IDirect3DDevice9::DrawIndexedPrimitive` as follows:

- `MinVertexIndex` is the minimum vertex index used by the call, relative to
  `BaseVertexIndex`.
- `NumVertices` is the number of vertices used by the call.
- The pair specifies the range of vertex indices used by the draw.

Source: [Microsoft Learn: IDirect3DDevice9::DrawIndexedPrimitive](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-drawindexedprimitive).

## WarVK mapping and current boundary

WarVK historically treated these values as insufficient for exact freeze
trimming because Warcraft may submit a loose range. A loose **superset** is
unsafe for compacting/rebasing geometry, but it is conservative for computing
caster bounds. An under-covering range would be unsafe for both purposes.

The current development observer therefore scans the current-generation IB and
classifies the D3D9 range as exact, conservative superset, under-covering, or
invalid. This does not authorize culling and does not change Release behavior.
Only a zero-undercoverage physical evidence set can support a later proposal to
use the O(1) caller range for conservative terrain bounds; exact freeze trimming
will continue to require the exact index domain.
