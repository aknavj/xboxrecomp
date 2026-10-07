# Native NV2A to D3D11 Translation

How to translate Xbox NV2A pushbuffer commands into native Direct3D 11 draws
while preserving guest state, memory updates and command completion.

## Overview

The Windows pushbuffer backend consumes commands produced by recompiled game
code and the Xbox D3D runtime. It translates raw NV2A method state rather than
intercepting high-level D3D8 API calls. The implementation supports native
rendering, but does not provide complete Xbox GPU emulation.

## Architecture

The recompiled game and Xbox D3D runtime construct NV2A commands in guest RAM.
The host consumes those commands and translates their state and geometry into
D3D11 draws:

```text
Recompiled game / Xbox D3D runtime
    |
    | guest RAM commands, DMA_PUT
    v
NV2A memory/register servicing
    |
    v
Pushbuffer scanner: packets, jumps, calls, returns
    |
    v
Method executor: state capture, vertex fetch, topology expansion
    |
    | Nv2aGpuDraw + Nv2aGpuVertex + uint32 indices
    v
Native D3D11 backend
    |
    | VS / optional GS / PS, offscreen color and depth resources
    v
Completion + staging readback + guest-memory publication
    |
    v
Framebuffer presenter: displayed RGB image, F12 capture
```

Geometry, textures, matrices, lighting, combiners and blend settings come from
the guest command stream.

The important distinction from a high-level D3D8 wrapper is that this backend
consumes **raw NV2A method state**. An Xbox API enum, host D3D9 default, or
high-level render-state conversion cannot automatically be substituted for a
hardware register encoding.

### Source Files

All paths below are relative to the XboxRecomp repository.

| Component | Responsibility |
| --- | --- |
| [xbox_memory_layout.c](../../src/kernel/xbox_memory_layout.c) | Guest address mapping, NV2A register servicing and DMA consumption/completion ordering |
| [nv2a_pb_scan.c](../../src/kernel/nv2a_pb_scan.c) | Walks the submitted command stream and dispatches method parameters |
| [nv2a_pb_exec.c](../../src/kernel/nv2a_pb_exec.c) | Retains NV097 state, decodes vertices/textures, assembles native batches and reports unsupported methods |
| [nv2a_gpu.h](../../src/kernel/nv2a_gpu.h) | C/C++ interface between command execution and native rendering |
| [nv2a_gpu_d3d11.cpp](../../src/kernel/nv2a_gpu_d3d11.cpp) | D3D11 device, resources, caches, generated shaders, draw submission and readback |
| [nv2a_gpu_shader.h](../../src/kernel/nv2a_gpu_shader.h) | Shared embedded HLSL for fixed transforms, lighting, texture stages, combiners, clipping and depth |
| [nv2a_shader_cpu.h](../../src/d3d/nv2a_shader_cpu.h) | Shared NV2A descriptor definitions and numerical/decoding helpers; also used by the separate CPU path |
| [d3d8_swizzle.h](../../src/d3d/d3d8_swizzle.h) | Shared swizzle, packed-format and DXT decoding |
| [fb_present.c](../../src/video/fb_present.c) | Framebuffer window, RGB conversion and displayed-frame capture |
| [lifter.py](../../tools/recomp/lifter.py), [translator.py](../../tools/recomp/translator.py) | Translation of guest CPU instructions that produce rendering state |

The separate [nv2a_pgraph_d3d11.c](../../src/nv2a/nv2a_pgraph_d3d11.c)
translator is separate from this pushbuffer backend. Likewise, the older
D3D8 combiner translator is not the pixel-shader generator described here.

The Windows backend is linked into `xbox_kernel` with C++17, `d3d11` and
`d3dcompiler`; see [kernel CMake configuration](../../src/kernel/CMakeLists.txt).
It requests a hardware D3D11 device and uses Shader Model 5.0 shaders. It does
not retry with WARP when hardware initialization or native rendering fails.

## Draw Interface

[nv2a_gpu.h](../../src/kernel/nv2a_gpu.h) deliberately separates three kinds of
data:

- **`Nv2aGpuVertex`**: position, diffuse/specular colors, four float4 texture
  coordinates, fog, normal, blend weights, and all 16 raw attributes.
- **`Nv2aGpuTexture`**: guest source bytes and layout, dimensions/pitch/format,
  cube/volume/mip information, sampler controls, bump parameters, and decoder
  callbacks for pixels, faces, levels and volume slices.
- **`Nv2aGpuDraw`**: target memory/layout, surface and window clipping, texture
  shader modes, combiners, raster/depth/stencil/blend state, vertex microcode
  and constants, indices, and fixed-function lighting/transform state.

Target and texture pointers refer to mapped guest storage. They are not
already-created D3D resources. The backend resolves them into retained host
resources, checking layout and aliasing before use.

The fixed/pretransformed input upload excludes the raw-attribute tail;
programmable vertices upload the full structure. Input offsets for newer
attributes derive from `offsetof`, and input-layout element counts derive
from the descriptor array rather than a stale hardcoded count.

### Lifecycle Entry Points

| Function | Meaning |
| --- | --- |
| `nv2a_gpu_available` | Lazily attempts hardware-device creation |
| `nv2a_gpu_compile` | Compiles/creates the shared base shader pipeline |
| `nv2a_gpu_draw` | Validates and submits a native triangle-, line- or point-list batch |
| `nv2a_gpu_execute_state` | Executes a guest transform-state program through a compute shader and returns updated constants |
| `nv2a_gpu_sync` | Waits for pending work and publishes dirty render targets to guest memory |
| `nv2a_gpu_flush` | Synchronizes, then marks retained targets for guest-memory refresh before reuse |
| `nv2a_gpu_invalidate` | Synchronizes and drops retained color/depth target resources |
| `nv2a_gpu_report` | Reports cumulative execution, cache, upload, publication and timing counters |

On Windows, failure to render a submitted batch produces diagnostics and
terminates the executor instead of silently switching to CPU rasterization.
The non-Windows CPU path remains separate. CPU-side command decoding,
topology conversion and texture-format decoding still occur; "zero CPU
fallback" describes rasterization, not the absence of CPU work.

## Command Processing and Geometry

The scanner retains incomplete method packets across consecutive submissions.
A parameter word must remain a parameter even when the producer splits a packet
at a submission boundary. It also implements incrementing/nonincrementing
methods, both supported jump encodings, a DMA subroutine call/return slot,
address checks and a bounded command walk. Discontinuous packets, invalid
addresses, nested DMA subroutines and returns without calls fail explicitly.
GET reports fetched commands; native fences separately establish rendering
completion and guest-memory publication.

The executor retains state between methods and assembles primitives at draw
boundaries. Triangle lists, strips, fans/polygons, quads and quad strips become
ordered triangle lists. Strip winding alternates correctly; fan anchors and
the last provoking vertex are preserved.

Point lists remain points; independent lines, line strips and closed line
loops become ordered line lists. Chunks end only between complete primitives,
including the closing line-loop edge. Indexed, inline-array and immediate
paths accept the appropriate minimum vertex count. `Nv2aGpuDraw.topology`
defaults to triangles at zero, preserving existing zero-initialized callers.
Native IA topology and geometry shaders match the normalized primitive type.
Line flat shading uses the final endpoint; nonfinite positions are rejected
by the line/point geometry shaders. Polygon culling and polygon depth offsets
do not suppress or offset actual lines/points. NV097 point-size state is
retained, but only unit-size points (encoded size 8) currently render; other
sizes fail explicitly. Native line and point counts are reported separately
from triangle statistics.

Index and attribute handling follows these rules:

- `ARRAY_ELEMENT16` (`0x1800`) contributes two 16-bit indices per word;
  `ARRAY_ELEMENT32` (`0x1808`) contributes one full-width index.
- Mixed commands share one ordered `uint32_t` index stream. `DRAW_ARRAYS`
  retains its full start index instead of narrowing it to 16 bits.
- Guest index storage grows geometrically rather than truncating geometry
  at a fixed capacity.
- Larger primitives are expanded into ordered native chunks, each within
  65,536 vertices and 196,608 indices. These are **per-native-draw limits**,
  not a reason to discard the remainder of a guest primitive.
- Vertex remapping compares full-width source indices, not just their low
  16 bits. Native index buffers use `DXGI_FORMAT_R32_UINT`.
- Attribute addressing uses wide byte-offset arithmetic and checks the
  complete attribute footprint against the applicable mapped-memory extent.
- Immediate vertices snapshot all 16 current attributes when a position
  completes. Later normal/weight/color writes must not retroactively change
  every vertex in the batch.

## Vertex Processing and Coordinate Spaces

There are three distinct vertex paths:

1. **Pretransformed vertices** already describe guest screen-space positions.
2. **Fixed-function vertices** use the retained NV2A transform/light/texgen
   state in the shared vertex shader.
3. **Programmable vertices** supply all raw attributes to a generated HLSL
   translation of uploaded NV2A microcode.

### Fixed-Function Transforms

Dedicated matrix methods and generic constant uploads update the same
192-entry float4 constant bank. The shared shader uses these register ranges:

| Purpose | Constant rows |
| --- | --- |
| Composite projection | 0-3 |
| Model-view transform for slot `i` | `8 + i*8` through `11 + i*8` |
| Normal transform for slot `i` | `12 + i*8` through `15 + i*8` |
| Local-eye position | 56 |
| Viewport XY offset | 59 |
| Texgen planes for stage `s` | `64 + s*8` through `67 + s*8` |
| Texture matrix for stage `s` | `68 + s*8` through `71 + s*8` |

`fixed_matrix` explicitly computes the output components from the captured
constant vectors; it does not guess a host matrix storage convention.

With skinning disabled, the composite already includes model-view, so
**object-space position is projected directly**. Applying model-view a second
time would produce an incorrect transform. Eye-space position is computed
separately for lighting, fog and coordinate generation.

Skin modes 1-6 select two, three or four model-view/normal transforms.
Odd-numbered modes derive the last weight as one minus the earlier weights;
even-numbered modes use explicit weights. The shader does not clamp or
renormalize the guest's weights. Normal normalization follows the guest
enable bit rather than being unconditional.

After projection, fixed vertices divide XYZ by W and add the programmed
viewport XY offset. The common bridge then reconstructs host homogeneous
coordinates from guest screen coordinates:

```text
clip.x = (2 * screen.x / targetWidth  - 1) * W
clip.y = (1 - 2 * screen.y / targetHeight) * W
clip.w = W
```

Z and W depth use different handling described below. Finite W magnitude is
bounded to the established reciprocal range while preserving its sign;
negative W is **not** converted to positive W.

### Programmable Vertex and State Programs

The backend validates uploaded 128-bit instructions and their end marker,
then generates HLSL with MAC/ILU operations, swizzles, negation, write masks,
temporary/output registers and relative constant addressing. Both halves
read pre-instruction operands before their results are written.

The generated VS bridges position, diffuse/specular, texture coordinates and
fog into the same downstream shader interface. A transform-state program
instead executes as a one-thread compute dispatch with writable constant
storage. Staging readback returns its updated 192 float4 constants to the
executor before later commands consume them.

Vertex/state shader cache identity includes the microcode itself. Constant
values remain dynamic, so changing constants does not require recompilation;
changing uploaded instructions cannot reuse a stale program. Vertex-program
constant writes remain unsupported; state-program writes are supported.
Complete physical-NV2A arithmetic equivalence is not established.

## Lighting, Fog and Generated Coordinates

The lighting method is `0x0314`; `0x0310` controls dithering. These raw method
encodings are distinct from high-level API render-state values.

Fixed-function lighting carries eight lights, with directional, local and
spot types, ambient/diffuse/specular colors, positions/ranges, attenuation,
directions, half-vectors and spot parameters. It also carries material/source
selection, scene ambient, alpha, normalization and specular controls.

Per-vertex evaluation includes the diffuse normal/light dot product, range
checks and local attenuation:

```text
attenuation = 1 / (constant + linear * distance + quadratic * distance^2)
```

Half-vector selection respects local-eye state; disabled specular and
separate/nonseparate specular follow the captured controls. Vertex colors
clamp before interpolation. These operations are gated to the fixed-function
path: a guest vertex program implements its own lighting.

Each texture stage independently supports disabled, object-linear and
eye-linear S/T/R/Q generation, plus legal sphere/normal/reflection components.
An enabled texture matrix transforms the resulting float4, retaining Q for
projective sampling. Lighting, eye-space fog and texgen share the weighted
eye-space transform.

Fog retains either the programmable fog output or the fixed source/generation
mode. Linear, exponential, exponential-squared and absolute variants produce
an interpolated factor. The pixel shader exposes it through the **combiner
fog register**; fog is not an unconditional blend pasted onto the final image.

Important limitations include approximate specular-exponent reconstruction,
incomplete spot-falloff coefficients, missing two-sided/back-light evaluation,
the uninterpreted alpha-from-material-specular flag, and unsupported
infinite-viewer reflection/sphere texgen.

## Textures, Texture Shaders and Register Combiners

### Resource Layout and Sampling

The executor resolves texture layout and supplies existing shared decoders.
Supported resources include ordinary linear/swizzled 2D images, six-face cube
maps, declared mip chains and DXT1/3/5 storage. Suitable DXT uploads use native
BC1/2/3 resources; layouts unsuitable for that direct path use decoding.
Mip levels are uploaded from guest data, not generated by the host.

Projective 3D sampling uses native `Texture3D` resources. Volume depth comes
from the NV097 texture-format W-size field and shrinks with each mip level.
Uncompressed swizzled volumes use three-axis Morton addressing; DXT volumes
use block-compressed 2D slices. Both upload as decoded RGBA volume slices,
since D3D11's BC resources are 2D. Cache identity includes depth, snapshots
cover every slice/mip, and refreshes retain the native slice pitch.

Packed/replicated formats include luminance/alpha, AL8, A8L8, G8B8/R8B8 and
additional 32-bit channel orders. Format conversion must preserve the Xbox's
channel replication, not merely choose a similarly sized DXGI format.

Samplers retain U/V/W addressing, border color, min/mag/mip filtering, LOD
bias/clamps and supported anisotropic combinations. Linear textures use
their dimensions to convert pixel-like coordinates into sampling coordinates.

FILTER's A/R/G/B sign flags convert selected sampled UNORM channels to
`2*x - 1` in the shader. This is not a different uploaded resource format.
Sampling modes also respect CONTROL0's texture-enable state; an explicitly
disabled texture needed by an active sampling mode rejects rather than
inventing a white/black/stale result.

Texture caches include layout and full source-byte snapshots, including
mips/faces. Reusing a guest address after its contents change must not reuse
the previous texture.

### Texture Shader Stages

Four NV2A texture stages run before register combiners. Current paths include
projective 2D/3D sampling, cube sampling/projection, passthrough, sign-based clip
planes, bump mapping/luminance, dependent AR/GB reads, DOTPRODUCT/DOT_ST,
selected paired diffuse/specular reflection, cube-direction lookup and
constant-eye reflection.

Stage dependencies are validated: forward/self references, missing dot
predecessors and unsupported resource/mode combinations reject. Signed dot
channel encodings and unsigned HILO reconstruction are explicit conversions.
Selected hemisphere mappings, non-projective volume modes, shadow sampling,
BRDF and DOT_ZW remain unsupported. Projective volume mode 2 requires a
nonlinear volume binding and preserves W addressing, projective division,
signed channels, alpha kill and color-key processing.

Texture alpha kill discards zero-alpha fetches before combining and target
writes. Color-key processing is separate from final alpha testing. The
uncombined fast path cannot bypass required clip-plane, alpha-kill or
fragment-kill color-key behavior.

### Register Combiners

Up to eight combiner stages evaluate independent RGB and alpha inputs,
component selections, input mappings, products/dots, sum/MUX selection,
output mappings and destination writes. A final combiner produces the output
color. Constants, diffuse/specular, texture results and fog are inputs to
this register machine.

Pixel shader variants specialize structural state: combiner words, texture
shader program, W-depth/depth enable, effective texture sign masks and window
clip mode. Color constants and other draw uniforms remain dynamic. This
allows unused stages/branches to compile away without replacing guest
material behavior with a universal `texture * diffuse` rule.

Pixel and guest vertex variants use separate least-recently-used caches,
each bounded by 512 entries and 16 MiB of compiled bytecode. Cache hits update
recency rather than letting frequently used shaders age out by insertion
order. The former 128-entry FIFO repeatedly recompiled working sets larger
than 128 variants. First-time compilation remains synchronous; these caches
are process-local, not persistent disk caches. The `shaders` timing report
separates vertex/pixel hits, misses, compilation time and eviction counts.
Shader keys and strict compiler flags are unchanged. Guest vertex variants
compile with fixed-function transformation disabled at compile time, pruning
unreachable lighting/texgen code. Fixed-function shaders retain that machinery;
guest program instructions and their outputs are unchanged.
The base vertex shader now uses optimization level 3 instead of disabling
optimization. IEEE-strict compilation remains enabled. Startup compiles the
uncombined pixel shader and geometry shaders; combiner pixel variants compile
on demand. The unused universal pixel shader is not compiled: optimizing its
dynamic branch/combiner graph caused a roughly 40-second first-frame delay.

Draw counters establish submission, not exact filtering or combiner precision.
Validate stage behavior with controlled inputs and framebuffer comparisons.

## Rasterization, Depth, Stencil and Clipping

The backend maps supported culling, front-face orientation, solid/wireframe
fill, blending, blend constants, color masks and depth/stencil state to
cached D3D11 state objects. Stencil operations/masks/reference are supported
on Z24S8 storage.

The native draw interface currently requires 32-bit color targets with valid
row pitch and dimensions no larger than 4096 per axis. Depth storage supports
integer Z16 and Z24S8 through D16/D24S8 host resources; floating-depth storage
is not supported. The presenter's ability to convert other framebuffer
formats does not imply the native draw path renders to all of them.

Z24S8 upload and publication share inverse byte-rotation helpers. On SSE2
hosts, these convert four pixels per iteration directly from source to
destination, avoiding the previous copy-then-convert publication pass.
Unaligned rows, scalar tails and in-place conversion remain supported; Z16
remains a byte copy. This optimization does not skip depth publication or
defer command completion.

New depth resources use the same staging upload as retained-resource refresh.
Supplying Z24S8 through texture creation data was observed to round some depth
values by one integer step on the local D3D11 device. The staging path preserves
the packed depth/stencil bits, including when Z16 and Z24S8 layouts alias.

Full native-view clears can stay queued on D3D11 rather than publishing prior
draws, clearing guest memory on the CPU and uploading it again. The fast path
supports complete 32-bit color writes, integer Z16 clears, exact zero/far Z24
clears and stencil clears. Depth-only and stencil-only Z24 operations preserve
the other plane. Fully overwritten planes need no old-content upload.
Nonzero clip origins, partial color masks, arbitrary Z24 depth integers,
unsupported layouts and overlapping color/depth storage retain the CPU clear
path and its synchronization. Incompatible dirty aliases still publish before
layout reuse, and real visibility/completion boundaries remain unchanged.
`RECOMP_NV2A_NATIVE_CLEARS=0` selects CPU clears for comparison. The `clears`
report counts native operations and requests declined by the native helper;
it does not count CPU paths rejected before that helper or explicitly disabled.

The optional geometry shader preserves the last provoking vertex for flat
colors, rejects nonfinite positions and computes polygon-offset depth slope.
It does **not** discard an entire finite triangle merely because one vertex
has negative W. D3D11 homogeneous clipping retains its visible portion.

Host near/far depth clipping is disabled. The pixel shader applies the
guest's clip/clamp range to interpolated guest Z or perspective W, applies
enabled face-specific polygon offset, and quantizes depth to the guest
integer storage range. W-depth uses its own shader variant and guard for
invalid/nonpositive depth; it is not treated as normalized Z.

Window clipping retains all eight hardware rectangles:

- Inclusive mode accepts their union.
- Exclusive mode rejects pixels inside the union.
- Rectangles use the hardware inclusive endpoints converted to half-open
  host bounds.
- Simple inclusive coverage can reduce to a native scissor; disjoint unions
  and exclusion use a scissor bound plus pixel-shader membership checks.

Using only the bounding box for a disjoint union would incorrectly fill its
holes. Using only the first rectangle would drop valid geometry.

## Guest-Memory Coherence and Presentation

The D3D11 device renders to offscreen textures, **not a title swap chain**.
The guest still expects framebuffers, clears, CPU memory accesses and command
completion to behave like guest-memory operations. The window displays the
published framebuffer through the existing RGB/GDI presenter.

Retained color/depth surfaces therefore have:

- host render resources and staging resources;
- guest memory identity and layout;
- dirty/refresh flags;
- a published source snapshot;
- conservative possible-write bounds and, where needed, disjoint scissor
  coverage masks.

Synchronization copies/maps dirty resources and publishes covered spans back
to guest memory with pitch and depth/stencil packing handled explicitly.
Cold snapshots initialize the full surface. Retained color readback can copy
a smaller bounding region; depth uses full resource copies. Publication
coverage represents possible writes, not an exact record of fragments that
passed every pixel test.

Refresh compares guest memory with the retained snapshot. Unchanged targets
avoid unnecessary upload; color mutations can produce a boxed upload.
Completion still requires an event query when pending draws have no color
or depth readback, for example output-masked work. Skipping readback must not
skip the GPU completion guarantee.

`RECOMP_NV2A_NATIVE_FENCES` opts into handling `SET_CONTEXT_DMA_SEMAPHORE`,
`SET_SEMAPHORE_OFFSET` and `BACK_END_WRITE_SEMAPHORE_RELEASE`. The executor
resolves the channel's DMA handle through RAMHT and the RAMIN descriptor,
checks the writable class, target, offset, alignment and physical extent,
then publishes preceding rendering before writing the command's fence value.
NVM and coherent PCI RAM targets are supported; unsupported descriptors fail
explicitly. Title integrations must disable synthetic fence-counter mirrors
when enabling this path: mirroring the CPU's latest submitted counter can
retire work that the asynchronous command walker has not consumed.
Object bindings are resolved by class, not by a hardcoded pattern subchannel.
For class `0x44`, `SET_MONOCHROME_COLOR0` updates `PGRAPH_PATT_COLOR0`;
Xbox D3D uses that register as its consumed-pushbuffer checkpoint and compares
its fence stamp against the semaphore. Updating only semaphore memory leaves
that guest wait stuck and can present as an entirely black in-game window.
The toolkit path remains opt-in; Ghost enables it by default. Setting the
variable to `0` disables it.

Nonzero `NO_OPERATION` parameters are queued to the title's original
software-method dispatcher, registered with `xbox_Nv2aSoftwareMethodHandler`.
It runs at DPC level on the existing guest-stack/TIB worker. The executor
publishes preceding rendering and snapshots the depth/color-clear registers,
then waits for that CPU work before fetching the next command. GPU register
acknowledgements continue during the wait, allowing PFB flushes and a preceding
vblank ISR to finish. Ghost's dispatcher command 13 applies deferred
pushbuffer fixups, including each invocation's return link; ignoring it
replays old ring commands when a static buffer is reused. Other commands
retain their original guest semantics rather than being treated as harmless
no-ops. Zero remains an ordinary no-op.

Software requests wake the existing worker through an event instead of waiting
for its periodic polling interval. Completion still requires the callback to
return and pass its stack check. The submitting thread services acknowledgements
with nonblocking completion checks for at most 250 microseconds, then uses
one-millisecond timed waits. High-resolution deadlines keep extra callback
wakeups from accelerating APU, vblank, DPC and timer servicing; the periodic
deadline advances after every periodic pass, even when no guest timer is due.
The `software dispatch` report exposes request counts and total/average waits.

DMA GET advances as parameters are fetched, before semaphore release and
software callbacks, and follows jump/call/return targets. Publishing GET only
at a whole submission boundary leaves it behind completed fences: a CPU can
then insert a notification into already-fetched padding and wait forever.
GET is fetch progress, not proof that asynchronous rendering has completed.
Ghost also enables vblank delivery for native flip callbacks and disables its
synthetic frame-counter mirror in this mode.

In native-fence mode, reaching a DMA PUT boundary does not force a synchronous
color/depth readback. Submitted native draws can remain queued until an actual
completion or CPU-visibility boundary. `WAIT_FOR_IDLE` (`0x0110`), semaphore
releases, software callbacks, flips and CPU surface access still synchronize
and publish preceding work. Legacy completion mode retains whole-submission
publication. This distinction avoids turning small command submissions into
unrequested GPU idle waits; it does not make GET a completion fence.

The physical-mirror allocator tracks page ownership and reuses freed
contiguous pages under an SRW lock. `MmAllocateContiguousMemoryEx` honors
alignment and physical bounds, including fixed-address requests, without
bypassing allocation ownership. `MmFreeContiguousMemory` releases these
blocks, and allocation-size queries include them. GPU instance storage remains
reserved at the arena's top; the arena is not enlarged. Surface address
resolution checks live allocations rather than classifying every address
below a historical bump pointer as contiguous. `RECOMP_CONTIG_TRACE` reports
bounded allocation/free samples, live bytes and the high-water mark.

The shared DPC ring serializes insert, pop and removal under an SRW lock,
rejects duplicate queued objects, and executes guest callbacks outside the
queue lock. This prevents concurrent USB, GPU and other producers from
overwriting queued work. Gameplay input and repeated level transitions still
require user validation; locking alone is not a full interrupt-emulation claim.

`RECOMP_PB_FAILURE_TRACE` records a bounded 32-word command history. Parser
failures log segment bounds, packet state, return address, live DMA pointers,
the last 16 control-flow transfers and method reports
without skipping invalid commands or changing termination behavior.

Sampling the current color target as a texture is rejected. Compatible
sampling of another retained color target can reuse its SRV. Dirty source
aliases with the same guest base and row pitch can copy their covered regions
on the GPU into an exact retained sampling view. Disjoint write masks preserve
their holes. Write-version tracking reuses an unchanged sampling mirror and
invalidates it after CPU uploads. These copies do not create new guest writes:
the dirty owner still publishes at real completion/CPU visibility boundaries.
Different bases/pitches, depth aliases and missing exact sampling views retain
CPU synchronization. `RECOMP_NV2A_GPU_ALIAS_COPIES=0` selects that older path.
The `source aliases` report counts GPU copies/bytes, version reuses and CPU
synchronization fallbacks. Already-clean overlaps do not force another
synchronization. Incompatible target layouts publish outstanding
overlapping work but retain reusable layouts. Publication marks overlapping
color/depth peers for refresh, so a later layout switch observes the current
guest-memory contents rather than stale host resources.
An already-created covering target can service smaller requested views with
the same guest base/pitch (and depth format). The largest compatible retained
owner is preferred over old smaller aliases to avoid alternating layouts.
Paired color/depth attachments must have identical host dimensions; missing
compatible pairs use exact requested layouts. This never infers or allocates
a larger guest-memory extent. Viewport, scissor and dirty coverage still use
the requested dimensions. Smaller-view clears draw a constant-depth,
constant-color rectangle with independent depth/stencil write masks, rather
than clearing the whole covering resource. Arbitrary D24, partial color masks
and other unsupported clears retain their CPU fallback.
`RECOMP_NV2A_COMPATIBLE_TARGETS=0` selects exact-layout targets for comparison.
The `compatible targets` report counts reused views, rectangle clears and
layout-conflict synchronizations.
Each target cache remains limited to 64 entries, evicting the least recently
used target only when a new entry needs space. Exact-layout hits do not clear
a full cache. Target creation, eviction and synchronization counters expose
resource churn. Target creation/upload failures report their HRESULT and
device-removal reason; invalid depth layouts report format or pitch details.
General arbitrary CPU/GPU concurrent-memory
coherence and feedback rendering are not solved by this model.

This correctness-first publication path has significant cost: resource
copies, blocking maps, depth conversion, guest-memory comparisons and
publication. Vertex/index uploads append to dynamic streams with
`WRITE_NO_OVERWRITE`, using `WRITE_DISCARD` on first use or wrap so queued draws
never see overwritten data. Compact native vertices and full guest-program
vertices have separate streams; each draw binds its vertex/index byte offsets.
Streams grow to fit larger submissions, with 512 KiB vertex and 64 KiB index
starting capacities. The `streams` report counts discard and append maps.
Shaders, samplers and
render states are cached. Reported host timing scopes include CPU work and
synchronization; they are not a pure GPU timestamp profile.

Targets containing only a known full-surface clear publish the exact guest
clear bits after an explicit GPU completion event, without a staging copy/map.
Color clears are exact BGRA8 values; D16 clears retain their integer depth.
D24S8 requires both components cleared, or a previously known full value whose
uncleared component can be retained. Partial clears, changed CPU uploads,
sampling-mirror copies and subsequent target-writing draws invalidate this
shortcut. Guest writes, row padding and target-alias refresh ordering remain
unchanged. The `known clears` report counts publications and avoided downloads;
it does not imply a gameplay-FPS improvement by itself.
`RECOMP_NV2A_KNOWN_CLEARS=0` restores staging readback for comparison.

Draw uniforms reuse the existing constant buffer only when a byte comparison
of the complete, zero-initialized uniform block matches the last successful
upload. Changes to guest-program constants, lighting, clipping, textures or
other uniforms still trigger `WRITE_DISCARD`; native rectangle clears use a
separate buffer. Recreating the draw buffer invalidates its snapshot.
The `constants` report separates uploads from unchanged reuses.

The cumulative `executor time` report uses a monotonic high-resolution clock
to separate native vertex/state preparation from backend calls. `draw phases`
partitions backend time into setup, texture processing, stream/constant uploads,
shader selection, binding/draw submission and bookkeeping. Texture/hash and
constant timing reports overlap their corresponding phases; do not add those
nested timers to the phase totals. These are CPU wall times, including driver
stalls, not GPU query durations. Compare deltas across reports in the same scene
before claiming an FPS improvement.

Native vertex-program batches fetch only attributes consumed by their MAC/ILU
source operands, plus position retained for the vertex snapshot. CPU execution,
input-mask analysis and shader generation share the operand-use rules, including
ARL and ILU-only instructions. Uniform and immediate inputs retain their normal
fetch semantics. Program validation and shader-cache keys remain unchanged;
cache hits compare the complete instruction bytes without allocating a key.
Fixed-function and pretransformed vertices clear only the compact prefix that
the backend uploads, not the unused sixteen-attribute tail.
`RECOMP_NV2A_VERTEX_FETCH_ALL=1` restores all sixteen program-attribute fetches
for comparison. The `vertex preparation` report counts performed/skipped fetches
and avoided zeroing bytes. These changes do not relax fences, CPU visibility or
target publication, and their gameplay-FPS benefit still needs measurement.

## Texture Resource Reuse

Changed content at an unchanged complete layout updates existing native
texture subresources rather than recreating the texture and SRV. Every
face/mip is decoded/uploaded; full source snapshots and byte comparisons
remain the invalidation authority. Same-address textures with different
pitch, mip count, cube layout or source extent retain separate cache entries.
FIFO eviction uses 1024 entries and a 256MiB resource-plus-snapshot threshold;
a single oversized entry may exceed the byte threshold.

Resource reuse must preserve synchronization, surface publication and draw
work. Measure creation, upload and publication costs separately when
evaluating cache changes.

## Debugging Guest Rendering State

Non-increasing subchannel-zero `ARRAY_ELEMENT16/32` packets copy bounded runs
of index parameters into native index storage with one capacity check and one
DMA GET publication per run. GET advances only after every source word in that
run has been copied, so the CPU cannot reuse unread commands. Runs stop at PUT,
the mapped aperture boundary or the command-walk budget; split packets retain
their pending method/count and discontinuity validation.
Control transfers, increasing packets, other subchannels and side-effecting
methods retain word-by-word execution. Method/index/wide-index counters remain
equivalent. Failure tracing or verbose executor tracing selects that scalar
path automatically; `RECOMP_PB_BULK_INDICES=0` selects it for comparison.
The periodic `bulk indices` report exposes copied words and avoided GET writes.

`RECOMP_NV2A_GPU_TIMING=1` adds GPU timestamps and pipeline statistics around
every 128th native draw. Sixteen reusable query groups are polled with
`D3D11_ASYNC_GETDATA_DONOTFLUSH`; unavailable groups are skipped, never waited
on or forcibly flushed. Periodic reports separate programmable, lit and
W-depth draws and show sampled GPU draw time plus VS/GS/PS invocation counts.
These are sampled draw costs, not total frame time or GPU utilization; driver
query overhead can affect the diagnostic run. Disjoint timestamps are counted
and discarded. The flag is off by default and does not change rendering,
depth quantization, publication or fence completion.

Submission tracing is opt-in: `RECOMP_NV2A_TRACE=1` logs DMA PUT/GET and
display-register changes; an unset, empty or `0` value leaves it disabled.
Command execution and ten-second performance reports do not depend on this
flag. Normal submissions no longer flush stderr when no trace line was written.
Per-draw progress lines require `RECOMP_PB_EXEC_VERBOSE`.

Incorrect rendering can originate in guest CPU translation before commands
reach the backend. When a transform produces unexpected clipping or orientation,
trace the uploaded constants back to the guest matrix calculations before
changing shader conventions.

Camera and vector routines can use SSE or x87 compare-to-EFLAGS instructions
followed by `LAHF`, `TEST AH, 0x44` and parity branches. `LAHF` copies
SF/ZF/AF/PF/CF into AH while preserving the rest of EAX. The generator must
materialize tracked flags before that copy and recognize it as a carry
consumer. Floating comparisons clear AF; CF/PF/ZF must retain unordered/NaN
semantics.

Do not compensate for invalid guest state with absolute W, guessed Z negation,
a second model-view transform or a completion bypass. Check both CPU-produced
state and GPU translation when diagnosing missing geometry.

## Build and Validation

Build the game project using its configured CMake build directory:

```powershell
cmake --build .\build --config Debug --parallel 1
```

From the XboxRecomp repository root, check the embedded shader sources:

```powershell
py -3 .\tools\compile_nv2a_hlsl.py
```

The shader utility concatenates the embedded HLSL fragments and checks shared
shader variants. It does not compile every generated guest vertex/state
program or pixel specialization. Shader compilation and successful draw
submission are not substitutes for runtime and visual validation.

Verify that the game's CMake configuration includes the intended generated
sources and that the executable matches the configuration being tested.
Compare displayed frames with reference output using equivalent guest state.
Exercise index/chunk boundaries, primitive types, texture layouts and shader
combinations independently.

### Framebuffer Capture

Closing the main framebuffer window (`WM_CLOSE`, including Alt+F4) exits the
host process successfully; internal `WM_DESTROY` cleanup only stops its window
thread. The presenter loads the first executable icon-group resource at both
large and small system icon sizes, and sets the class and window icons.
Shared icon handles remain owned by Windows. Missing icon resources use the
Windows application icon; enumeration/loading failures are reported.

The framebuffer window caption uses
`<RECOMP_WINDOW_TITLE> | <fps> FPS | Frame <count>` (with the existing default
title when unset). The window thread updates it once per second using a
high-resolution clock and an atomic counter of completed presentation calls.
GDI repaints do not increase the frame count or FPS; pinned framebuffer mode
still counts game presentation calls. Title updates do not run on the GPU
submission thread or alter input handling.

Set a capture destination before launching the game:

```powershell
$env:RECOMP_FB_CAPTURE = 'C:\captures\framebuffer.bmp'
```

Create the capture directory first. Press **F12 in the framebuffer window**
to capture the presenter's displayed RGB buffer. Without `RECOMP_FB_CAPTURE`,
the presenter writes `framebuffer.bmp` in the working directory.
Allocation, write and close failures are checked and reported.

### Runtime Integration

Input and audio initialization can block guest rendering before any draw
reaches the backend. Diagnose those subsystems separately rather than treating
their diagnostic overrides as renderer requirements. See the
[input integration guide](../../src/input/README.md) and
[audio guide](../../src/apu/README.md) for their setup and limitations.

USB descriptor/HCCA metadata uses the contiguous physical window, separate
from low XBE RAM. Payloads and APU DMA use page provenance recorded by
`MmGetPhysicalAddress`; ordinary-RAM buffers must not be redirected blindly
into the contiguous bank. This does not establish general coherence for
numerically overlapping banks.

## Known Limitations

- Volume/depth/shadow textures, BRDF, DOT_ZW and selected dot mappings.
- Full specular/spot coefficients, back-face lighting and some texgen modes.
- Non-unit point sizes, point polygon fill and visible mixed front/back fill.
- Exact NV2A arithmetic, W-buffer slope behavior and filtering footprints.
- General framebuffer feedback and CPU/GPU memory coherence.
- Full feature coverage and sustained frame-rate improvements.

Implemented features still require validation against reference output.
Successful builds, submission counters or individual captures do not establish
complete GPU compatibility or gameplay correctness.

## References

Cxbx's fixed-function, vertex/pixel and packed-format implementations provide
semantic references. Low-level NV2A method, constant and shader behavior can
be cross-checked against xemu where high-level API state is insufficient.
Reference implementation bodies were not copied into this backend.

Useful reference entry points:

- [Cxbx fixed-function vertex shader](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/master/src/core/hle/D3D8/Direct3D9/FixedFunctionVertexShader.hlsl)
- [Cxbx fixed-function state](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/master/src/core/hle/D3D8/FixedFunctionState.cpp)
- [Cxbx pixel-shader template](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/master/src/core/hle/D3D8/Direct3D9/CxbxPixelShaderTemplate.hlsl)
- [xemu PGRAPH methods](https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/pgraph.c)
- [xemu fixed-function shader generation](https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/glsl/vsh-ff.c)
- [NVIDIA texture-shader specification](https://registry.khronos.org/OpenGL/extensions/NV/NV_texture_shader.txt)

## Adding Backend Features

For a new backend feature:

1. Establish the raw method encoding and retained state, not just an API name.
2. Wire executor capture, the C interface, C++ constants and HLSL together.
3. Update input layouts, shader cache identity and resource requirements where
   the feature changes them; constant-only changes should remain dynamic.
4. Validate combinations explicitly instead of substituting success-shaped
   defaults for unsupported behavior.
5. Preserve target publication, completion, winding and provoking semantics.
6. Distinguish compile success, submitted draws, exercised state and verified
   pixels. A framebuffer/reference comparison is necessary for a visual fix.
7. Inspect guest-produced state when valid renderer math yields an impossible
   result; the error may originate in CPU translation rather than rendering.
