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
| [nv2a_gpu_memory.cpp](../../src/kernel/nv2a_gpu_memory.cpp) | Opt-in GPU-resident target tracking, CPU alias guards and private publication |
| [nv2a_gpu_simd.cpp](../../src/kernel/nv2a_gpu_simd.cpp), [nv2a_gpu_simd_avx2.cpp](../../src/kernel/nv2a_gpu_simd_avx2.cpp) | Exact CPU-side mutation bounds, integer depth rotation and input validation, with runtime SIMD selection |
| [nv2a_gpu_shader.h](../../src/kernel/nv2a_gpu_shader.h) | Shared embedded HLSL for fixed transforms, lighting, texture stages, combiners, clipping and depth |
| [nv2a_texture_depth.h](../../src/d3d/nv2a_texture_depth.h) | Bounds-checked fixed-depth texture decoding and shadow comparison semantics |
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
programmable vertices upload only reflected live float4 attributes by default.
`RECOMP_NV2A_VERTEX_PACKED=0` restores the full canonical structure. Input offsets for newer
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
| `nv2a_gpu_sync_counters` | Reads completion-wait and asynchronous-idle counters on the rendering thread |

On Windows, failure to render a submitted batch produces diagnostics and
terminates the executor instead of silently switching to CPU rasterization.
The non-Windows CPU path remains separate. CPU-side command decoding,
topology conversion and texture-format decoding still occur; "zero CPU
fallback" describes rasterization, not the absence of CPU work.

The periodic `sync reason` lines attribute calls, calls with pending work,
elapsed time, staging readbacks and published color/depth bytes to idle waits,
software notifications, semaphores, flips, CPU clears/rasterization, reporting,
target-cache conflicts, texture aliases, invalidation and external callers.
Counters are cumulative; compare successive report deltas in the same scene.
No command-level tracing is required. A synchronization with neither pending
draws nor dirty targets avoids rebinding the output merger; a flush still marks
retained targets for CPU-memory comparison before reuse.

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

Fixed-depth texture sampling supports swizzled D16 (`0x2C`), LIN_D16
(`0x30`) and LIN_D24S8 (`0x2E`). D16 uses Morton addressing rather than
linear rows; linear variants preserve declared row padding. D24 takes depth
from the upper 24 bits and excludes stencil. Uploads use `R32_FLOAT`, not
an eight-bit grayscale conversion, and swizzled D16 mip chains retain their
guest contents. Dirty depth-target aliases publish through the existing
texture-alias completion boundary before decoding.

`SET_SHADOW_DEPTH_FUNC` (`0x1E6C`) retains all eight comparison functions.
Depth formats in PROJECT2D compare against zero; PROJECT3D samples a 2D
depth image and compares against clamped `Z/Q` in the guest's 16-/24-bit
depth units. The sampled depth is the left operand. The ordinary native
sampler filters depth before comparison; this is not comparison filtering
or a host-added soft-shadow/PCF effect. Cube/volume depth bindings, floating
depth formats and unsupported depth color transforms are not substituted.
The non-Windows software fragment path also has base-level point/bilinear
fixed-depth sampling; its existing mip/derivative limitations remain.

`nv2a_shader_smoke.exe --depth-textures` checks 120 full-precision
projective comparisons, point/bilinear behavior, mip selection, depth-target
alias publication, stencil exclusion, non-square structured RGBA/XRGB masks
and eight actual pushbuffer comparison changes. These tests establish
backend behavior, not a confirmed repair of Ghost's live shadow artifact.

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

### Offscreen targets and four texture inputs

NV097 has one color destination and one depth/stencil destination per draw;
four texture stages are inputs, not four simultaneous color outputs. Titles
can switch between several offscreen surfaces and sample them in a later
pass. The backend retains up to 64 color and 64 depth layouts, preserving
their independent contents, pitches and views across target switches.
Do not duplicate the single NV2A color result into four D3D11 MRT slots.

`NV2A_GPU_FOUR_TARGETS_ONLY=1` selects a focused correctness test in
`nv2a_shader_smoke.exe`. It renders four independent targets, samples all
four native SRVs through three register-combiner stages, checks the exact
composite, and updates/rebinds one target without disturbing the other three.
Variants cover a shared depth/stencil target with different color pitches,
and a larger rectangular scene image plus three smaller auxiliary images.
They check image contents, padding/exterior bytes, and that native target
sampling does not invoke CPU texture decoding. The ordinary smoke run also
includes these tests.

For gameplay diagnosis, set `RECOMP_NV2A_TARGET_DIAG=1` in the same terminal
used to launch the title. Native reports then include each retained color
and depth layout's host memory address, dimensions, pitch, dirty/refresh
state and content serial (plus the depth format). It is off by default and
does not add per-draw logging. Addresses are host pointers, not guest VAs.
Inventory counts are current retained layouts, whereas `surfaces: N created`
is cumulative; neither count identifies an image's semantic purpose, and
multiple layouts may describe one guest allocation.

### Texture Shader Stages

Four NV2A texture stages run before register combiners. Current paths include
projective 2D/3D sampling, cube sampling/projection, passthrough, sign-based clip
planes, bump mapping/luminance, dependent AR/GB reads, DOTPRODUCT/DOT_ST,
selected paired diffuse/specular reflection, cube-direction lookup and
constant-eye reflection.

Stage dependencies are validated: forward/self references, missing dot
predecessors and unsupported resource/mode combinations reject. Signed dot
channel encodings and unsigned HILO reconstruction are explicit conversions.
Selected hemisphere mappings, non-projective volume modes, floating-depth
and other shadow sampling modes, BRDF and DOT_ZW remain unsupported.
Projective mode 2 uses a 2D binding for the fixed-depth formats above;
ordinary projective volume mode 2 requires a nonlinear volume binding and
preserves W addressing, projective division,
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
Guest-program shader keys and strict compiler flags are unchanged. Guest vertex variants
compile with fixed-function transformation disabled at compile time, pruning
unreachable lighting/texgen code. Fixed-function shaders retain that machinery;
guest program instructions and their outputs are unchanged.
The base vertex shader now uses optimization level 3 instead of disabling
optimization. IEEE-strict compilation remains enabled. Startup compiles the
four uncombined pixel variants and geometry shaders; combiner pixel variants compile
on demand. The unused universal pixel shader is not compiled: optimizing its
dynamic branch/combiner graph caused a roughly 40-second first-frame delay.

The uncombined fast path selects depth-disabled/enabled and Z/W-depth variants
by a two-bit key, with no per-draw variant-list search. Depth mode is compiled
explicitly instead of sharing one runtime-branching shader across all modes.
This fixes an observed cold color-only -> depth-enabled sequence that wrote
zero depth through the former shared shader. The same sequence now publishes
exact Z24S8 values and preserves subsequent CPU stencil changes. The precise
driver/compiler cause of the former shared-shader sequencing failure has not
been established; depth specialization is also consistent with the existing
combiner shader path.

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
Nonzero effective clear origins, partial color masks, arbitrary Z24 depth integers,
unsupported layouts and overlapping color/depth storage retain the CPU clear
path and its synchronization. Incompatible dirty aliases still publish before
layout reuse, and real visibility/completion boundaries remain unchanged.
`RECOMP_NV2A_NATIVE_CLEARS=0` selects CPU clears for comparison. The `clears`
report counts native operations and requests declined by the native helper;
it does not count CPU paths rejected before that helper or explicitly disabled.

The command executor tracks `SET_CLEAR_RECT_HORIZONTAL/VERTICAL` separately
from the drawing clip. Clear coordinates have inclusive 12-bit endpoints;
they are bounded by the current inferred surface extent, not used to enlarge
the allocation. Empty/inverted rectangles are no-ops. CPU color clears honor
the individual R/G/B/A flags (RGB only for RGB565), and depth/stencil clears
preserve the plane not selected. Before either clear-rectangle register has
been written, the corresponding drawing clip remains the compatibility
default. Surface-size inference for inset offscreen views still needs visual
validation; these changes do not prove Ghost's shadows are correct.

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
The opt-in GPU-residency prototype described below defers eligible semaphore
publication behind CPU-access guards; the default path remains eager.
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

### Auxiliary 2D source copies

With native object handling enabled, the executor also implements class
`0x62` (NV062 context surfaces 2D) and class `0x9F` (NV09F image blit)
`SRCCOPY` operation 3. State belongs to the RAMIN object instance, so switching
objects/subchannels does not lose the surface DMA handles, formats, pitches,
offsets or blit coordinates. Y8, RGB565, A8R8G8B8, the two X8R8G8B8 encodings
and Y32 copy their original bytes without texture decoding.

Both DMA descriptors are resolved through RAMHT with checked physical spans,
limits and row pitches. GPU work is published before CPU reads/writes, and
the flush marks cached targets for refresh: a subsequent render-target texture
sample must see the copied bytes. Nonoverlapping copies use direct row copies;
overlapping rectangles first snapshot the source. Unsupported operations,
formats, descriptors or extents fail explicitly rather than pretending the
image was copied. This is a coherent CPU copy path, not a GPU-resident blit
optimization, and no gameplay speedup is claimed.

`nv2a_shader_smoke.exe --effects` runs focused copy/clear correctness tests,
including GPU source publication and sampling a retained destination after
the copy. The ordinary smoke run also starts isolated effects tests and
negative descriptor/pitch/operation/address cases. Reports include
`auxiliary blits: N copies, M MiB`.

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

Output dirtiness also follows depth/stencil operation reachability. `NEVER`
depth or stencil comparison cannot create color/depth writes; stencil fail,
depth-fail and pass operations are considered only on reachable paths.
Already-dirty outputs from earlier draws are retained. Rejected-output draws
still execute on D3D11 and participate in completion waits, including when
only stencil writes are reachable. This avoids stale publication over CPU
changes as well as unnecessary downloads. The `output rejection` counter
counts draws that cannot pass depth/stencil; it is not a count of avoided
readbacks, since stencil writes or earlier dirty draws may still require them.

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

### Packed Guest Vertex Inputs

Lossless compact uploads for guest vertex programs are **enabled by default**.
`RECOMP_NV2A_VERTEX_PACKED=0` restores canonical uploads for comparison.
DXBC reflection
identifies the consumed `TEXCOORD0..15` float32 inputs, including shaders loaded
from the persistent bytecode cache. Unexpected signatures and reflection
failures are explicitly diagnosed and rejected.

Each consumed attribute retains all four original float bit patterns in a
16-byte slot. A sparse two-input program therefore uploads 32 bytes per vertex
instead of the 404-byte canonical structure; all sixteen inputs use 256 bytes.
Constant-only programs use a zeroed 16-byte dummy stream. Unused layout elements
alias offset zero but are not read by the shader. Fixed-function uploads retain
their 148-byte prefix. Shader instructions, CPU vertex preparation, topology,
indexed addressing, target publication and completion fences are unchanged.

New shader variants log their reflected mask, attribute count and upload stride.
The cumulative `vertex upload` report compares submitted bytes with the
equivalent canonical payload and counts packed draws. Shader lookup/compilation
remains attributed to `draw phases` shader time even though the layout must now
be selected before stream upload.

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

### Persistent Shader Bytecode

The native backend retains compiled HLSL bytecode across launches in
`%LOCALAPPDATA%\XboxRecomp\ShaderCache\v1`. This covers shared vertex/pixel/
geometry shaders, specialized guest vertex/pixel programs, transform-state
compute shaders and rectangle-clear shaders. Native shader objects and input
layouts are still created for the current hardware device.

Cache identity includes the complete source bytes, diagnostic source name,
ordered macro names/definitions, entry point, profile, compilation flags and
compiler API version. Entries retain and compare the complete request, not
just a hash. SHA-256 checksums verify bytecode before use. Input/layout changes
that change generated HLSL automatically select a different entry; unrelated
GPU state remains dynamic in the existing uniform buffers.

Writes use a temporary file in the cache directory followed by atomic
replacement. Truncated, mismatched, oversized or checksum-invalid entries
produce a diagnostic and recompile from the current source. Successful
compilations alone are persisted. Cache I/O failures are reported and retain
ordinary compilation; shader compilation failures still propagate normally.
Individual requests/bytecode are limited to 16 MiB. A 256 MiB observed directory
budget stops new writes while retaining hits; stale entries are not automatically
deleted. Simultaneous processes can exceed the observed budget, since it is
not a cross-process storage reservation.

`RECOMP_NV2A_SHADER_CACHE=0` disables disk lookup/writes.
`RECOMP_NV2A_SHADER_CACHE_DIR` selects a dedicated cache directory for controlled
cold/warm comparisons. The periodic `shader disk cache` report counts hits,
misses, writes, invalid entries and I/O errors separately from in-memory shader
variant hits. Existing shader timing scopes include disk lookup and native
object creation as well as compilation on misses.

This removes repeated compilation stalls at startup and first encounter with
cached shader variants. It does not eliminate semaphore readbacks or improve
already-warm shader execution by itself; compare the same scene and duration
when interpreting whole-run average FPS.

## Debugging Guest Rendering State

### Compiled Ghost cloak and shadow programs

The optional asset-driven regression loads the supplied Xbox binaries from
`game_files\VertexShaders\xbox`; it does not embed game shaders in test sources:

```powershell
.\build-xr\src\d3d\d3d8_smoke\Release\nv2a_shader_smoke.exe --ghost-effects .\game_files\VertexShaders\xbox
```

Use the **Xbox subfolder**, not the similarly named parent-folder binaries.
The Xbox pixel programs use PSB0 definitions; their cloak, dissolve and
dissolve-shadow programs have six, five and three combiner stages respectively.
Xbox vertex binaries contain raw NV2A instructions, whereas the parent-folder
vertex binaries inspected contain desktop-style tokens.

The suite checks 48 independent formula/CPU/D3D11 cases for each pixel
program, including shared constants and equivalent final R0 passthroughs.
Formula tolerance is `1e-5`; framebuffer tolerance is one 8-bit channel unit.
Four actual rigid/skinned/soft-skinned cloak vertex programs are compared
against CPU-translated vertices using gradient textures, with exactly 28
covered pixels each. Indexed GPU submission and NV097 program, constant,
array, texture and combiner uploads are also checked. Run again with
`RECOMP_NV2A_VERTEX_PREPARED=0` to check reference attribute fetching.
The synthetic identity bone pose includes v5 indices and v6 weights; it is
not a reproduction of Nova's live animation or material state.

The compiled cloak programs use independent projective texture reads, not a
dependent bump-texture instruction. Authoring intent identifies t0 as the
scene image, t1 as membrane, t2 as mask/dissolve, and t3 as base color.
The shadow transition uses the dissolve texture's blue channel and an
intermediate signed combiner clamp before opacity multiplication. Passing
these tests does **not** prove the live bindings, constants, depth/stencil,
blend state or visible effects correct.

### Bounded effect-state capture

`RECOMP_NV2A_EFFECT_DIAG=1` logs named `[GPU-EFFECT]` snapshots for these three
instruction families. When unset it follows `RECOMP_NV2A_TARGET_DIAG`; explicit
`RECOMP_NV2A_EFFECT_DIAG=0` disables effect snapshots even with target inventory
enabled. Both diagnostics remain off by default.

Capture is limited to 32 deduplicated records per effect. Keys include target
layout, vertex program, raster/depth/stencil/alpha/blend state, texture metadata
and selected stage constants quantized to eight intervals. Geometry and
viewport matrices are not in the deduplication key: this is a bounded binding
investigation, not a full draw or animation trace.

The same diagnostic also captures `projected-target` samples: a square linear
texture up to 1024 pixels across, sampled in PROJECTIVE2D mode from a retained
color/depth target with matching pitch. These are rendering-pattern candidates,
not named or proven shadow shaders. Capture is capped at four records per source
set and 64 total, so one changing menu target cannot consume the entire budget.
Pixel signatures, depth format and polygon-offset enable/scale/bias join the
deduplication key; polygon offsets and texture-program words are also printed.
Depth-tested PROJECTIVE2D draws using square swizzled A8R8G8B8/X8R8G8B8
sources from 64 to 512 pixels are captured as `projected-source` when no
retained source matches. These are candidates, not identified shadow maps.
PROJECT3D fixed-depth sources are also eligible for capture. Records include
the shadow comparison function in their output and deduplication key.
The four-record source budget also includes the pixel signature. Records print
RGB/alpha combiner words and an 8x8 decoded probe of swizzled RGBA sources
(alpha range and row RGB means); failed decoding is reported explicitly.
Probes run only with effect diagnostics enabled, without adding native target
readback.
Padded LIN_X8R8G8B8 target regressions cover alternating rows, point-center
bilinear samples, non-unit Q division, midpoint filtering, opaque alpha and
GPU sampling without CPU texture decoding.

Records include host pointers (not guest VAs), actual combiner control,
selected shared/per-stage factors, texture formats and samplers, relevant
vertex constants, and three **referenced** vertices' live attributes,
source indices and CPU-evaluated positions/texture coordinates.
For triangle lists, sampling skips leading triangles with repeated indices
when a distinct-index triangle is available; it does not test geometric area.
CPU evaluation is diagnostic only and never substitutes a rendered result; evaluation errors
are reported explicitly. Family matching normalizes equivalent final
passthroughs and constant-sharing flags; it is not a shader-cache identity.

For manual investigation, reproduce cloak activation/deactivation and shadows
with capture enabled. Compare actual scene/membrane/mask/base bindings, fade
and cutoff factors, v5/v6 skinning inputs, projected coordinates, target clip,
and depth/stencil/alpha/blend state before changing translation semantics.

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

The existing hardware smoke executable has a focused publication mode:

```powershell
cmake --build .\build-xr --config Release --target nv2a_shader_smoke --parallel 2
$env:NV2A_GPU_PUBLICATION_ONLY = "1"
.\build-xr\src\d3d\d3d8_smoke\Release\nv2a_shader_smoke.exe
Remove-Item Env:\NV2A_GPU_PUBLICATION_ONLY
```

It verifies D16/D24S8 packing, CPU mutations, retained dirty writes, reachable
and unreachable stencil operations, disjoint scissors and row padding. It
also measures 256 synchronized draws per 640x480 pitched D16/D24S8 workload
and a `NEVER`-depth workload. Reports before and after the latter distinguish
completion time from readback obligations. The renderer must use hardware;
the mode does not select WARP, capture frames or automate title input.
These synthetic timings are not gameplay-FPS measurements.

A more exhaustive depth-download mode runs those correctness checks, then
round-trips every D16 and D24 integer code through hardware targets. D24 also
covers all 256 stencil values (not every depth/stencil Cartesian combination).
Odd widths, SIMD tails, unaligned guest pitches, row padding, unchanged color
and last-pixel CPU mutations are checked:

```powershell
$env:NV2A_GPU_DEPTH_DOWNLOAD_ONLY = "1"
.\build-xr\src\d3d\d3d8_smoke\Release\nv2a_shader_smoke.exe
Remove-Item Env:\NV2A_GPU_DEPTH_DOWNLOAD_ONLY
```

This mode additionally measures fully occluded and visible publication
workloads. `NV2A_GPU_DEPTH_DOWNLOAD_BENCH_ONLY=1` skips the non-benchmark
checks; benchmark output and padding assertions still run.
`NV2A_GPU_BENCH_CONTIGUOUS=1` uses tightly packed color/D24 targets instead
of the default padded workload. D16 depth still uses the color target's
larger pitch.

`RECOMP_NV2A_TARGET_COMPARE_BULK=1` opts into a contiguous-target comparison
experiment; it is **off by default**. For valid, clean snapshots whose guest
pitch equals the active row size, one full-block comparison replaces per-row
comparisons. Unchanged color targets return before scanning rows; changed
color targets still use the existing changed-rectangle upload. Padded targets,
dirty output, depth packing, readback and fence ordering are unchanged.
The `target comparison` diagnostic counts full-block comparisons. This does
not eliminate pixel comparisons or establish CPU-access coherence.

Texture lookup first checks the last matching resource for each of the four
texture stages. This small lookaside is **on by default**; set
`RECOMP_NV2A_TEXTURE_LOOKASIDE=0` for the fallback-only reference path. A hit
requires the complete existing layout match, then the same full source-snapshot
comparison. It does not assume a texture is immutable or bypass target-alias
synchronization. FIFO serials, not pointers into the texture vector, survive
growth and front erasure; every eviction advances the serial in both indexed
and linear modes.

The fallback texture-cache lookup experiment is separately controlled by
`RECOMP_NV2A_TEXTURE_LOOKUP_INDEX=1` and is **off by default**. Its source-address
index narrows layout candidates, but still checks width/height/depth, format,
pitch, linear/cube layout, face stride, mip count and complete source extent.
Every matching resource still compares all source bytes before reuse; mutations
retain the existing upload path. Target-alias checks and synchronization occur
before lookup, as before.

FIFO serials identify indexed entries without storing vector pointers or
rebuilding the index when vector storage moves or its front is erased.
The existing 1024-entry/256 MiB cache limits and FIFO policy are unchanged.
The `texture lookup` diagnostic reports lookups, layout candidates (including
lookaside probes) and indexed evictions. The `texture validation` diagnostic
separates lookup and source-comparison time and reports recent layout
hits/misses. A layout hit can still require a content update.

The historical `hash` timer is lookup plus exact `memcmp`, not a texture hash.
Its `hashed GiB` counts requested source extents, including cache misses.
The new comparison-extents counter counts only calls to `memcmp`; an early
mismatch can read fewer bytes than the requested extent. These are nested
timings, not additional costs to add to texture/draw time.

```powershell
$env:NV2A_GPU_TEXTURE_CACHE_ONLY = "1"
$env:RECOMP_NV2A_TEXTURE_LOOKUP_INDEX = "1"
.\build-xr\src\d3d\d3d8_smoke\Release\nv2a_shader_smoke.exe
Remove-Item Env:\NV2A_GPU_TEXTURE_CACHE_ONLY
Remove-Item Env:\RECOMP_NV2A_TEXTURE_LOOKUP_INDEX
```

This selector verifies four independent stage lookasides, vector growth and
FIFO eviction with 1100 sources, including previously remembered entries,
same-source pitch/byte-extent variants, mutations outside the decoded pixel,
cached-pixel mutations, mip counts, cube faces, volume slices and compressed
texture updates. It measures two workloads of 32768 cached draws each across
512 resident one-texel sources: round-robin (no recent-binding locality) and
16-draw material clusters. Both verify reuse and final hardware output. Set
`NV2A_GPU_TEXTURE_CACHE_BENCH_ONLY=1` to skip the non-benchmark checks;
the benchmark still checks reuse and final pixels. Use a fresh process with
`RECOMP_NV2A_TEXTURE_LOOKUP_INDEX=0` for the linear-lookup comparison.

Five interleaved fresh-process pairs, with no overlapping assistant GPU tests,
gave these medians with the fallback index disabled:

| Workload | Lookaside off | Lookaside on |
| --- | ---: | ---: |
| Round-robin | 126.835 ms | 124.322 ms |
| 16-draw clusters | 107.037 ms | 96.332 ms |

The clustered workload improved 10.0%; the round-robin difference is small
relative to run variation and is not a claimed gain. Combined layout
candidates fell from 16,941,680 to 9,127,169 (46.1%), with 30,722 recent hits.
Indexed-mode medians were essentially unchanged: 115.643/115.889 ms for
round-robin and 97.046/96.594 ms for clusters (off/on). This is a lookup
microbenchmark, not evidence of a gameplay FPS increase or stable 30 FPS.

An additional scalar/SSE2/AVX2 fused-publication experiment wrote guest RAM
and snapshots from each loaded pixel. Exactness and guard-page tests passed,
but both the simple and unrolled versions regressed CPU benchmarks. Three
interleaved hardware pairs also regressed: D16 median 292.865 -> 300.666 ms,
D24 median 375.657 -> 387.115 ms. The candidate and its runtime flag were
removed; the existing publication kernels and synchronization remain intact.
The pure SIMD `--bench` mode now includes the retained split color/D24
publication baselines and validates both complete outputs.

#### Batch-prepared NV2A vertex attributes

Native vertex translation now resolves float-array source bases and extents,
uniform/default values, and the existing color/texcoord slot selection once
per command batch rather than once per vertex. Each float-array fetch still
checks its exact 64-bit address/extent. Rejected extents use the original
decoder's diagnostic path. Immediate vertices and non-float formats retain
the original decoder. Constant values are refreshed every batch, and source
bytes are always read anew; this is not a cross-batch vertex-content cache.

Float1/2/3/4 decoding uses shared fixed-size byte copies, preserving unaligned
access and missing-component defaults `(0,0,0,1)`. Programmable vertex
preparation iterates a prebuilt live-input list rather than testing all 16
slots for every vertex. The program's input dependency mask is cached until
an instruction upload or start-address change invalidates it.

Prepared attribute fetching is on by default. Set
`RECOMP_NV2A_VERTEX_PREPARED=0` to retain per-fetch source/uniform resolution;
the live-input list and dependency cache remain. The `prepared vertex fetch`
report counts float-array attributes handled by the fast path.

Targeted correctness runs passed with prepared fetching off/on and with
full-attribute fetch plus canonical uploads. New command-path assertions
cover float1/2/3/4 defaults, float3 homogeneous W, unaligned source/stride,
source mutation, uniform changes between batches, and program-start changes.
Existing shader-mutation tests cover instruction-upload invalidation.
These runs executed the native command translator, not merely direct backend
draws. No synthetic performance benchmark or gameplay speedup claim was made
for this change. Release Ghost has been rebuilt.

#### Opt-in fixed indexed stream coalescing

`RECOMP_NV2A_STREAM_COALESCE=1` enables a separate dynamic buffer with both
vertex and index bind flags for fixed/pretransformed indexed draws. Vertices
and R32 indices occupy nonoverlapping, four-byte-aligned ranges and are
uploaded in one map/unmap pair. The combined stream starts at 1 MiB and
retains the existing discard/no-overwrite append policy. Draw ordering,
index validation, vertex layouts, and completion/publication are unchanged.
Nonindexed draws and programmable vertex inputs retain their separate streams.
The `coalesced streams` diagnostic counts indexed draws using this path.

This experiment is **off by default**. Combining programmable streams
regressed small-draw tests, even after increasing buffer capacity, so that
variant was excluded. Five interleaved fresh-process pairs for the fixed-only
variant measured:

| Indexed workload | Separate | Coalesced flag on |
| --- | ---: | ---: |
| Fixed, 4096 draws of 96 vertices | 61.687 ms | 56.032 ms |
| Fixed, 128 draws of 6144 vertices | 77.789 ms | 61.408 ms |
| Program, 4096 draws of 96 vertices | 14.371 ms | 16.787 ms |
| Program, 128 draws of 6144 vertices | 17.897 ms | 19.411 ms |

The programmed controls still use unchanged separate streams; their variation
limits attribution of the fixed-workload gains. No title FPS gain is
established, and the flag must not become default-on from these measurements.

The packed-vertex selector also runs these indexed benchmarks, verifies
complete color output against a nonindexed reference, and exercises repeated
stream wrap. Canonical/packed checks, eager/resident coherence,
scalar/SSE2/auto clipping/depth-transition checks, and full shader smoke pass
with coalescing enabled. Release Ghost has been rebuilt with the opt-in path.

Focused packed-input checks can be run on hardware:

```powershell
$env:NV2A_GPU_VERTEX_PACKED_ONLY = "1"
$env:RECOMP_NV2A_VERTEX_PACKED = "1"
.\build-xr\src\d3d\d3d8_smoke\Release\nv2a_shader_smoke.exe
Remove-Item Env:\NV2A_GPU_VERTEX_PACKED_ONLY
Remove-Item Env:\RECOMP_NV2A_VERTEX_PACKED
```

Repeat in a fresh process with `RECOMP_NV2A_VERTEX_PACKED=0` for the canonical
comparison. The selector covers existing guest vertex/state programs, sparse
low/high inputs, all sixteen inputs, constant-only shaders, NaN-poisoned unused
attributes, live-input mutations, exact color/D24S8 output, indexed quads and
switching back to fixed-function layouts. It also measures 128 draws with 6144
vertices each and checks final visible output after GPU completion.
`NV2A_GPU_VERTEX_PACKED_BENCH_ONLY=1` skips the other checks. Timed draws disable
color writes to isolate submission/upload costs; this is not a gameplay workload.

`NV2A_GPU_DEPTH_PRECISION_ONLY=1` independently runs the exhaustive D16/D24
round trips, stencil coverage, padded rows and last-pixel CPU-mutation checks,
without the other publication workloads or benchmarks.

`NV2A_GPU_BENCH_SEMAPHORE=1` makes the occluded and visible publication
benchmarks synchronize through `nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE)`
instead of the external-caller reason. It includes rejected-output completion
waits and retains all exact color/depth/stencil and padding checks. This tests
the backend boundary, not RAMHT descriptor decoding or the guest semaphore store.

Persistent-cache regression checks can be run separately:

```powershell
cmake --build .\build-xr --config Release --target nv2a_shader_cache_smoke
.\build-xr\src\d3d\d3d8_smoke\Release\nv2a_shader_cache_smoke.exe
```

They compare cached bytecode with direct compiler output, verify complete
source/macro/entry/profile/flag invalidation and persisted reuse, exercise
corruption/size/request checks, and check disabled/unavailable caches and the
exact 256 MiB budget. Temporary test entries are removed afterward. The hardware
publication mode can be repeated with cold, warm and disabled caches to check
that native output remains identical.

### October 2026 Performance Investigation

A 90-second Release title run's last periodic snapshot attributed 18.391
seconds to semaphore releases and 2.535 seconds to idle waits. Semaphores
accounted for about 88% of attributed synchronization time, 12,207 color and
8,186 depth readbacks; notifications and flips had no pending work. It reported
325,998 hardware batches and zero CPU rasterization fallback. This is one
workload, not proof that those proportions hold for every scene.

The rejected-output regression workload added 256 completion waits and zero
color/depth readbacks, with exact publication checks passing. The title run
did not exercise that rejection path, so it establishes no title-FPS gain.
Separate depth upload/download staging and a single batched readback event
wait were also compared; neither showed a reliable improvement, and neither
runtime experiment was retained. The larger unresolved optimization is
CPU-access coherence for GPU-resident targets, not bypassing semaphores or
publishing their values before rendering completes.

Four unattended 60-second Release title runs with the same binary and no input
automation measured persistent-cache behavior:

| Cache mode | Whole-run average FPS | Last reported vertex/pixel shader setup |
| --- | ---: | ---: |
| Cold dedicated directory | 9.74 | 8.527 s |
| Warm directory | 10.47 | 0.047 s |
| Cache disabled | 8.65 | 8.530 s |
| Repeated warm directory | 11.38 | 0.045 s |

Both warm runs recorded 61 cache hits, zero misses and zero cache errors.
All four ended at the configured watchdog, without logged renderer rejection
or readback failure. Shader variants and animated-scene work differed slightly,
and warm runs spent less of the fixed duration compiling. These averages
therefore demonstrate reduced startup/first-use stalls, not an isolated
steady-state scene-rendering speedup. The identical focused hardware
publication workload passed cold/warm/disabled; total cold/warm test time was
3.03/1.35 seconds, including compilation and validation.

Further publication experiments did not establish a reliable title-speed
improvement:

- Occlusion-predicate publication suppression passed its focused checks, but
  256 synchronized, fully occluded 640x480 draws took
  476.621/493.772/498.194 ms without queries versus
  659.357/678.924/544.285 ms with queries. Lower publication counts did not
  compensate for query overhead. The query/snapshot prototype was removed.
- Float-based compute packing into a readback buffer preserved all D16 codes,
  but changed 2,097,152 D24 codes by one on the tested hardware. Raw texture
  readback preserved all 16,777,216 D24 codes exactly. The compute path was
  removed; float sampling must not replace bit-preserving depth publication.
- Fusing integer D24 conversion stores into both snapshot and guest memory
  passed the all-code checks, but median visible-workload time regressed from
  447.197 to 471.476 ms. That prototype was also removed.

The opt-in contiguous-target comparison experiment passed the all-code,
CPU-mutation, scissor and output-reachability checks. Three interleaved runs
per mode produced these medians for 256 synchronized 640x480 draws:

| Workload | Row comparisons | Bulk comparisons |
| --- | ---: | ---: |
| Fully occluded D24 | 453.142 ms | 439.139 ms |
| Visible D16 | 344.188 ms | 333.456 ms |
| Visible D24 | 455.006 ms | 435.019 ms |

Four unattended 60-second Release title runs, using the same binary and warmed
shader cache in off/on/on/off order, gave whole-run averages:

| Bulk comparison | Average FPS | Presented frames |
| --- | ---: | ---: |
| Off | 10.71 | 641 |
| On | 9.37 | 561 |
| On | 11.18 | 669 |
| Off | 11.24 | 673 |

Each ended at the watchdog, with 60 or 61 shader-cache hits, no misses or cache
errors, and no logged native-renderer rejection or readback failure. The
enabled runs exercised thousands of bulk comparisons. Scene/work counts and
frame rates varied; the small synthetic benefit is not a verified title-FPS
improvement. The experiment therefore remains opt-in, and default rendering
continues using the established publication/comparison behavior.

The subsequent source-address texture index passed focused checks in both
modes, including 603 FIFO evictions. The final fresh-cache benchmark's three
interleaved runs per mode measured:

| Lookup mode | 32768-draw times | Median |
| --- | --- | ---: |
| Linear | 176.806 / 109.407 / 123.548 ms | 123.548 ms |
| Indexed | 159.301 / 110.602 / 110.964 ms | 110.964 ms |

Both performed the same source-byte comparisons and reused every decoded
resource. The diagnostic's layout-candidate count fell from 8,536,248 to
32,769, including warmup and the final visible draw. This intentionally
metadata-heavy one-texel workload does not represent the title's large-texture
memory-scanning cost; timings also varied appreciably between runs.

The title batch completed one linear run and two indexed runs before the
last launch was closed after 2.449 seconds. Only the completed, warmed-cache
60-second runs are included:

| Texture lookup | Average FPS | Presented frames |
| --- | ---: | ---: |
| Linear | 9.33 | 559 |
| Indexed | 8.15 | 488 |
| Indexed | 9.00 | 539 |

All completed runs recorded 61 shader-cache hits, no misses or cache errors,
and no logged native-renderer rejection or readback failure. The indexed run's
last snapshot examined 354,675 layout candidates for 277,094 lookups, versus
27,768,460 candidates for 301,937 linear lookups. Full source comparisons
still covered about 30-33 GiB. Less metadata search work did not establish a
whole-title FPS improvement, so this experiment remains off by default.
Further optimization iterations use one bounded title run compared with
saved baseline logs, with detailed/repeated validation left to offscreen tests.

The next target is stable **25-30 FPS in a manually prepared gameplay scene**,
not the unattended startup/menu path. The baseline session closed during the
attempted 30-second scene capture. Its final per-second samples ranged from
7.95 to 10.90 FPS; the whole-run 10.33 FPS average includes scene setup and is
not a scene-only result. The last reporting-interval deltas included 2.911
seconds of native backend draw handling and 3.754 seconds of synchronization,
of which 3.285 seconds were semaphore-related. Synchronization included 2.341
seconds of map/wait and 1.377 seconds of publication. These nested timings must
not be added to their parent categories.

The packed guest-input experiment passed canonical and packed hardware checks,
including the exact reflected masks/strides: `0x0009` and `0x8080` at 32 bytes,
`0xFFFF` at 256 bytes, and `0x0000` at 16 bytes. Related target-publication checks
also passed with packing enabled. Three interleaved offscreen runs per mode
measured the same 128-draw, 6144-vertex sparse-input workload:

| Vertex upload | Times | Median |
| --- | --- | ---: |
| Canonical | 98.249 / 98.796 / 105.899 ms | 98.796 ms |
| Packed | 42.462 / 39.434 / 42.321 ms | 42.321 ms |

Median workload time decreased about 57%, while the payload report, including
warmup and final visible output, decreased from 305.368 to 24.188 MiB (about
92%). This upload-heavy benchmark excludes executor-side vertex preparation
and disables timed color writes; it does not establish a gameplay FPS gain.
At that stage the packed path remained opt-in. No second game run was launched in that
iteration. The 25-30 FPS target remains unmet, and the next candidate check
should use one manually prepared scene run against the saved baseline.

Two subsequent readback experiments were removed after hardware measurement:

- SSE4.1 streaming loads from mapped readback buffers passed visible-output
  checks but did not improve total synchronized workload time. Across three
  interleaved runs per mode, D16 medians were 391.130 ms with ordinary loads
  versus 409.190 ms with streaming loads; D24 medians were 484.321 versus
  502.062 ms. Lower publication time in some runs did not offset the total cost.
- Revised GPU D24 packing used exact power-of-two scaling followed by an
  upper-half integer correction, preserving the midpoint:
  `uint(depth * 16777216.0f) - uint(depth > 0.5f)`. Stencil reads used the green
  component of `X24_TYPELESS_G8_UINT`. This passed all 16,777,216 depth codes
  and all 256 stencil values on the tested hardware, unlike the earlier
  multiply-by-16777215/round formula. Precision success was not a speed gain:
  three interleaved visible-workload runs had medians of 490.863 ms for native
  staging versus 516.489 ms for GPU packing, including a 778.463 ms packed
  outlier. The dispatch, views and extra readback resources were removed.

Neither experiment changes the retained renderer or adds a runtime flag.
The raw depth staging path and exact CPU integer rotation remain authoritative.
The exhaustive precision-only test selector remains available for future work.

A bounded nonblocking-map experiment was also rejected. It explicitly
submitted queued readback copies, checked readiness using
`D3D11_MAP_FLAG_DO_NOT_WAIT` for at most 250 microseconds, then used the existing
blocking map when still busy. It never skipped publication or signaled a
semaphore before completion. Six interleaved hardware runs exercised the
semaphore synchronization reason, including 771 color and 771 depth readbacks
and 256 rejected-output waits per process. Exact output checks passed.

| Semaphore benchmark | Blocking-map median | Bounded-poll median |
| --- | ---: | ---: |
| Fully occluded | 526.967 ms | 506.100 ms |
| Visible D16 | 422.966 ms | 417.079 ms |
| Visible D24 | 503.493 ms | 526.942 ms |
| Rejected output | 353.651 ms | 361.926 ms |

First-run timing variation was substantial. Polling performed roughly
375,000-377,000 map checks, yet 770-771 of 1542 readbacks still fell back to
blocking; almost every first color map missed the polling deadline. The D24
workload regressed about 4.7%, so the polling helper, counters, submission
changes and runtime control were removed. The semaphore benchmark selector
remains for future validation. CPU-access coherence for GPU-resident targets
remains the larger architectural opportunity.

The packed-vertex candidate gameplay run enabled packed vertices, with
bulk target comparison and indexed texture lookup disabled. All 30 logged
one-second samples were captured in a complete 30.006-second interval:

| Candidate scene metric | Result |
| --- | ---: |
| Mean of logged one-second FPS samples | 19.05 FPS |
| Minimum / maximum sample | 13.96 / 22.94 FPS |
| Samples below the 25 FPS floor | 30 of 30 |

The earlier scene's final samples ranged from 7.95 to 10.90 FPS, but the
baseline interval ended early and exact scene/workload equivalence was not
independently verified. Do not attribute the full difference to vertex packing
or confuse the sample mean with the whole-run exit average. The candidate
exercised real guest shaders with 64 disk-cache hits and no disk-cache errors,
and closed normally with its logs preserved.

The periodic snapshots surrounding capture showed 3556.654 MiB uploaded
versus 5199.888 MiB of equivalent canonical payload, a 31.6% reduction across
both guest-program and unchanged fixed-function draws. Snapshot deltas
included 2.666 seconds of vertex/state preparation, 8.285 seconds of backend
draw handling and 10.091 seconds of semaphore synchronization. Backend time
included 3.700 seconds of texture processing and 1.515 seconds of stream
processing. Readback map/wait increased 8.296 seconds and publication increased
3.933 seconds. These are periodic-report deltas, not precisely timestamped
scene-only timers, and overlapping categories must not be added.

Packed uploads are now the default after exact-output regression coverage,
repeatable upload-workload savings and the completed gameplay check. The
canonical path remains available through `RECOMP_NV2A_VERTEX_PACKED=0`.
Fixed-function payloads, semaphore ordering, GPU completion and CPU-visible
publication are unchanged. The 25-30 FPS stability target remains unmet;
further work must address synchronization/readback and CPU draw preparation,
not just smaller upload payloads.

### CPU SIMD Kernels

The native D3D11 backend now uses shared CPU kernels for:

- **Color mutation bounds:** after the existing CRT equality check detects a
  changed row, SSE2/AVX2 compares four/eight complete pixels at once while
  locating its first and last changed pixels. Unchanged rows retain the CRT
  `memcmp` fast path, and pitch padding is not compared or uploaded.
- **Index validation:** SSE2/AVX2 checks four/eight unsigned indices per block.
  Sign-bit biasing preserves rejection of values such as `0x80000000` and
  `UINT32_MAX`; trailing indices are checked without overreading.
- **Finite float4 validation:** SSE2 checks exponent bits for positions and
  active texture coordinates. Positive/negative infinity and all NaNs are
  rejected, while signed zeros, subnormals and finite values retain their
  original bits. No floating-point arithmetic or approximation is introduced.
- **D24S8 conversion:** the pre-existing exact SSE2 integer rotation is shared
  with the tests. D16 remains a direct copy. A new AVX2 rotation candidate was
  removed because normal-RAM gains did not translate into repeatable mapped
  readback gains; this does not reintroduce the rejected streaming-load path.

`RECOMP_NV2A_CPU_SIMD` selects the kernels once per process:

| Value | Selection |
| --- | --- |
| unset, empty, `auto` | AVX2 when supported, otherwise SSE2, otherwise scalar |
| `0`, `scalar` | Scalar reference implementations, without explicit SIMD |
| `sse2` | SSE2 mutation bounds, indices, float4 checks and D24 rotation |
| `avx2` | AVX2 mutation bounds/indices; SSE2 float4 checks and D24 rotation |

The CRT and compiler can still use their own optimized instructions in scalar
mode. AVX2 has its own compilation unit, not a renderer-wide `/arch:AVX2`
requirement. Runtime detection checks CPUID, XSAVE/OSXSAVE, AVX, XMM/YMM OS
state and AVX2 support before entering it. AVX2 entry points cannot be inlined
into the baseline code under link-time optimization. Invalid controls and
explicit requests unsupported by the build/CPU/OS fail with a diagnostic.
Selection logs once as `[GPU-D3D11] CPU SIMD: ...`.

Three interleaved fresh-process CPU runs per mode produced these medians:

| CPU workload | Scalar reference | SSE2 | AVX2 selection |
| --- | ---: | ---: | ---: |
| Mixed changed-row bounds | 38.034 ms | 28.769 ms | 26.871 ms |
| Unsigned index validation | 63.894 ms | 41.718 ms | 23.816 ms |
| Strided finite float4 checks | 70.149 ms | 36.070 ms | 36.582 ms |
| Unchanged-row equality | 30.766 ms | 31.366 ms | 31.494 ms |

The three changed/validation kernels reduce the measured CPU workload time
by approximately 29%, 63% and 48% with AVX2 selection. All five benchmark
checksums match across all nine processes. Unchanged-row results are close,
not an additional SIMD win, and first-run outliers were present.

Final hardware semaphore-workload medians for scalar versus automatic
selection were 429.447/407.488 ms (occluded), 319.856/308.121 ms (D16),
408.212/412.480 ms (D24S8) and 298.015/279.168 ms (rejected output).
GPU wait variability was substantial, including an automatic occluded run
of 484.217 ms. These measurements do **not** establish a general readback or
gameplay FPS improvement; D24 was about 1% slower in this comparison.
Semaphore completion, readback ordering and GPU-residency controls are
unchanged.

The standalone kernel tests cover byte alignments 0-31, vector tails,
surrounding bytes, in-place conversion, guarded page ends, all float
exponents, random float bit patterns, unsigned index boundaries and every
D24 code with all stencil values represented. Hardware validation rejects
invalid indices/position/texture coordinates in every tested lane while
preserving prior pending color/depth output, and checks fragmented coverage.
Scalar, SSE2 and AVX2 selections all pass. Packed/default and canonical
vertex uploads, texture caching, exhaustive hardware depth/stencil publication,
and eager/resident coherence regressions pass. Release Ghost links the same
kernels and has been rebuilt.

```powershell
cmake --build build-xr --config Release --target nv2a_gpu_simd_smoke nv2a_shader_smoke
.\build-xr\src\d3d\d3d8_smoke\Release\nv2a_gpu_simd_smoke.exe
$env:RECOMP_NV2A_CPU_SIMD = "avx2" # or "scalar", "sse2", "auto"
.\build-xr\src\d3d\d3d8_smoke\Release\nv2a_gpu_simd_smoke.exe --bench
$env:NV2A_GPU_SIMD_ONLY = "1"
.\build-xr\src\d3d\d3d8_smoke\Release\nv2a_shader_smoke.exe
Remove-Item Env:\NV2A_GPU_SIMD_ONLY
Remove-Item Env:\RECOMP_NV2A_CPU_SIMD
```

The SIMD hardware selector now starts with a cold color-only -> depth-enabled
transition, then toggles depth off/on around a CPU stencil mutation. Before
depth-specializing the uncombined shaders, this sequence wrote `000000A5`
instead of `000032A5` and poisoned subsequent depth checks. Scalar, SSE2 and
automatic modes now pass, including disk-cache-disabled/canonical-upload
operation. This is a rendering fix, not a SIMD gain.

The former mixed-W smoke assertion incorrectly treated any negative-W vertex
as an invalid entire triangle, reflecting a CPU rasterizer shortcut rather
than homogeneous clipping. Adding that rejection to the GS caused visible
near-camera polygon holes, reported by the user at 21:05 on 2026-10-08.
That rejection has been removed; finite mixed-W primitives pass to D3D11's
homogeneous clipper, preserving their visible portions and neighboring
geometry. In the blended quad fixture, `9FBF0000` is the correct overlapping
clipped contribution, not the previously asserted `BF800000`.

The SIMD hardware selector now also compares exact full color and depth/stencil
images with and without the GS, using uniform colors so flat shading cannot
change the reference. It covers standalone/batched triangles, depth off/on,
one/two behind-camera vertices, fractional negative W and fully behind-camera
geometry. The regression failed with the added rejection and passes after
its removal. Nonfinite-position rejection remains unchanged. No controlled
SIMD gameplay run has been performed.

### Experimental GPU-Resident Targets

`RECOMP_NV2A_GPU_RESIDENT=1` enables a Windows-only, **off-by-default**
prototype. It retains eligible color/depth targets on the GPU across semaphore
boundaries, while still waiting for the completion event before the executor
stores the semaphore's exact value. It does not signal semaphores early or
allow unguarded CPU reads of stale target memory.

The guest-memory tracker registers the main RAM section and its CPU mirrors,
and the separate contiguous section and tiled alias. All registered views
start at section offset zero; truncated views guard only their mapped pages.
A private writable view of each section is reserved for publication. Guest
aliases of resident target pages become `PAGE_NOACCESS`, with their original
protections retained for restoration. Publication writes through the private
view, restores aliases only after all dirty output is copied, and wakes
waiting CPU threads. Guarding entire pages can also publish output when the
CPU accesses unrelated bytes in the same page.

Owner-thread access faults publish synchronously. Other-thread faults request
publication and wait for the render worker. Requests are serviced at worker
ticks, semaphore boundaries, and during long pushbuffer scans at approximately
256-word intervals (bulk index packets are indivisible). Guest protection and
query operations use coordinated wrappers and conservatively publish all
guarded targets before performing the Windows operation. Shutdown first
disables new residency, then drains publication on the owner thread before
removing the access handler and private views.

Mapping lookup uses bounds-checked thread-local mapping/view indices rather
than retaining pointers across registration or shutdown. An eight-entry
eligibility cache is used only for spans whose pages are already guarded and
whose saved permissions were verified writable for that exact CPU alias.
Publication invalidates entries using unique protection epochs, also assigned
on registration. Already-guarded spans avoid repeating protection work for
every mirror. Readonly aliases and mixed permission spans retain their checks;
unprotected spans are never assumed unchanged or cached as writable.

Target/texture addresses are canonicalized across registered aliases.
CPU-access, flip, invalidation and other non-semaphore boundaries retain
eager publication. Idle also remains eager unless the separate guarded-idle
experiment below is enabled. Unregistered targets, including contiguous storage that
fell back to `VirtualAlloc`, also publish eagerly, with a diagnostic and an
untracked-publication counter. Mapping, guard, restoration or publication
failures terminate explicitly rather than permitting incoherent memory.

Hardware tests use real shared sections, full and truncated CPU aliases, and
multiple sections. Coverage includes exact color/D24S8 output, CPU writes
after publication, scissor refresh, padding and page collateral bytes,
simultaneous readers, mixed tracked/untracked targets, texture-source aliases,
owner-thread decoder access, pushbuffer fault-request servicing and consumption,
coordinated protection queries/changes,
cross-thread shutdown with outstanding output, and eager operation afterward.
Both eager and resident modes pass. Default packed and forced-canonical
vertices, texture caching, publication, and exhaustive D16/D24/stencil
regressions also pass; Release Ghost has been rebuilt.
The broad smoke run also passes after depth specialization and correcting the
mixed-W clipping expectation described above.

Run each mode in a fresh process:

```powershell
$env:NV2A_GPU_RESIDENT_ONLY = "1"
$env:RECOMP_NV2A_GPU_RESIDENT = "0" # repeat with "1"
.\build-xr\src\d3d\d3d8_smoke\Release\nv2a_shader_smoke.exe
# To isolate the mapped publication benchmark:
$env:NV2A_GPU_RESIDENT_BENCH_ONLY = "1"
.\build-xr\src\d3d\d3d8_smoke\Release\nv2a_shader_smoke.exe
Remove-Item Env:\NV2A_GPU_RESIDENT_ONLY
Remove-Item Env:\NV2A_GPU_RESIDENT_BENCH_ONLY
Remove-Item Env:\RECOMP_NV2A_GPU_RESIDENT
```

Three interleaved runs per mode measured 256 synchronized 640x480 draws per
workload, including CPU depth mutation every 16 draws. Exact final pixels,
depth/stencil and padding are checked outside the timed interval.

| Mapped semaphore workload | Eager median | Resident median | Time reduction |
| --- | ---: | ---: | ---: |
| Visible D16 | 312.072 ms | 151.526 ms | 51.4% |
| Visible D24S8 | 403.452 ms | 159.263 ms | 60.5% |
| Rejected output (`NEVER`) | 284.574 ms | 279.451 ms | 1.8% |

The visible workloads reduced color and depth readbacks from 514 each to 34
each (93.4%), moving those publications to CPU-access boundaries. They still
executed 514 completion-event waits, with 514 resident boundaries, 1028 target
deferrals and zero untracked publications. The mixed-memory correctness test
separately exercises the eager fallback. Rejected-output work still requires
completion and offers no comparable publication saving.

These are offscreen measurements, **not gameplay FPS**. A subsequent resident
gameplay run exposed the eligibility regression described below; gameplay
after its repair has not yet been measured. Fault frequency, presenter/allocator page overlap,
debug protection-handler coexistence and real command-stream request latency
still need gameplay validation. The prototype must remain opt-in; the stable
30 FPS target is not yet established.

#### Populated-RAM eligibility regression and repair

The 22:04 resident gameplay capture averaged 6.11 FPS, with 8,664 target
deferrals but 36.574 seconds charged to the copy/eligibility block. Readback
wait and publication had fallen to 0.873/0.540 seconds. The old eligibility
check called `VirtualQuery` separately for every target page. On fully
populated mapped RAM, each query can scan a large same-protection region;
repeating it per page made the supposedly cheaper resident path expensive.

Eligibility now walks returned Windows regions, while already-protected
pages retain their saved original-permission checks. A shared checked
region-walk helper also serves alias protection and respects truncated views.
Readonly and other unsupported permissions still reject residency. Fences,
alias guards, CPU-access publication, and restoration ordering are unchanged.
The `residency tracking` diagnostic separates eligibility and protection time;
eligibility is nested inside the existing copy timer.

The regression fixture uses a fully populated 64 MiB section and repeatedly
checks a 300-page target. Sparse or small-section benchmarks did not expose
the same cost. Three fresh-process runs before and after the fix, with matching
benchmark flags and no overlapping assistant GPU tests, gave these medians:

| Workload | Before | After |
| --- | ---: | ---: |
| 256 residency eligibility checks | 30,248.261 ms | 101.131 ms |
| 256 visible D16 semaphore draws | 3,793.303 ms | 136.959 ms |
| 256 visible D24S8 semaphore draws | 3,847.306 ms | 160.393 ms |

Eligibility time fell about 99.7%. This reproduces and repairs the tracker
slowdown, but does not establish gameplay FPS after the fix.

Add `NV2A_GPU_RESIDENT_LARGE_MAPPING=1` to the resident selector above to
populate the 64 MiB fixture and run the eligibility benchmark. Without
`NV2A_GPU_RESIDENT_BENCH_ONLY`, all coherence checks also run. Permission
regressions cover unaligned spans, readonly region boundaries, saved readonly
versus writable alias permissions, and transitions through protected pages.
Both eager/resident modes, full large-mapping coherence, scalar/SSE2/auto
clipping/depth regressions, and full shader smoke pass. Release Ghost was
rebuilt with the repair; residency remains opt-in.

#### Opt-in guarded idle boundaries

`RECOMP_NV2A_GPU_RESIDENT_IDLE=1`, together with
`RECOMP_NV2A_GPU_RESIDENT=1`, extends target deferral to `NV097_WAIT_FOR_IDLE`.
It is **off by default** and does nothing without the main residency option.
When all dirty targets are eligible for deferral, no readback or known-clear
publication is needed, and no CPU-access request prevents residency, all CPU
aliases are guarded before submission and idle can return without a CPU wait.
`RECOMP_NV2A_GPU_ASYNC_IDLE=0` restores the prior completion wait; unset enables
this scheduling only within guarded idle. This does not enable residency itself.
Outstanding GPU work is retained separately from newly queued draws, so a
subsequent semaphore waits even with no intervening draw. Repeated completed
semaphores do not repeat the event wait. CPU access, notifications, external
flushes, invalidation and untracked/mixed targets retain completion and eager
publication where required. The report includes asynchronous boundary counts.
Eligible target aliases remain guarded; CPU reads/writes request exact
publication as in semaphore residency. Pending CPU-access requests prevent
deferral. Untracked targets still publish eagerly. Flip, external flush,
invalidation, and CPU-access behavior are unchanged.

The 09:40 gameplay capture had both residency and coalescing active, but
idle still accounted for 18.133 seconds and 20,091 color/3,731 depth
readbacks through 2,723 flips. Extending guards to idle targets this remaining
publication cost, not the completion wait itself.

Historical measurements of the earlier completion-wait implementation:
five interleaved fresh-process hardware benchmark pairs with main residency
on and idle deferral off/on measured 256 idle-synchronized draws per format:

| Idle workload | Eager idle median | Guarded idle median |
| --- | ---: | ---: |
| D16 | 301.427 ms | 86.398 ms |
| D24S8 | 388.010 ms | 90.340 ms |

The combined visible workloads reduced color/depth readbacks from 514 each
to 34 each. That version preserved GPU completion ordering through readback
maps on eager visible draws and completion-event waits on deferred draws.
These are historical synthetic results from the completion-wait version, not
measurements of asynchronous scheduling or verified gameplay gains.

Use the resident selector with `NV2A_GPU_BENCH_IDLE=1` to benchmark idle
instead of semaphore boundaries. This selector takes precedence over
`NV2A_GPU_BENCH_SEMAPHORE`. Regression coverage verifies the idle guard
states, exact CPU-visible color/depth/stencil, and alternating idle/semaphore
draws followed by cross-thread access. All residency/idle on/off combinations,
large populated mappings, scalar/SSE2/auto clipping/depth checks, and full
shader smoke pass. Ghost logs the idle option at startup and has been rebuilt.
Keep it experimental until gameplay, UI and mission-reload checks pass.

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
Each statistics update also logs the same caption to stderr with the
`[FBWIN]` prefix, including `0.00 FPS` when presentation stops. Ghost redirects
these lines to `Ghost-errors.log` beside its executable. Logging is once per
second, not once per frame, and uses the exact FPS/frame sample shown in the
window title.
At normal process exit, window close, guest HAL shutdown/bug check or watchdog
termination, the presenter logs one
`[FBWIN] Average <fps> FPS | Frame <count> | Elapsed <seconds> seconds` summary.
The average is total game presentations divided by wall time since the
framebuffer presenter started, including loading, video and stalled periods;
it is not an arithmetic mean of rounded per-second samples. Duplicate shutdown
notifications do not emit duplicate summaries. Forced external termination
or an abrupt crash may prevent the report.

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

- Floating-depth textures, non-projective volume modes, other shadow modes,
  BRDF, DOT_ZW and selected dot mappings.
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
