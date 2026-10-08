# xbox_apu — MCPX APU Audio Emulation

Hardware emulation of the Xbox's MCPX APU (Audio Processing Unit), extracted from [xemu](https://github.com/xemu-project/xemu). The APU is a custom chip with three processors:

- **VP (Voice Processor)** — 256 hardware voices, ADPCM/PCM decode, pitch shifting, envelopes, HRTF 3D audio
- **GP (Global Processor)** — DSP for global effects (reverb, chorus). Currently **stubbed** — effects bypass.
- **EP (Encode Processor)** — DSP for AC3/DTS encoding. Currently **stubbed** — passthrough.

Audio output uses Windows XAudio2, with `waveOut` as a fallback, at 48kHz
stereo 16-bit.

## Output and DMA Integration

For integrations with separate memory banks, use
`mcpx_apu_init_standalone_mapped` with the contiguous-bank base and
`xbox_DmaPhysicalPointer`. Voice/descriptor allocations normally occupy that
bank, but payloads can occupy ordinary low RAM. `MmGetPhysicalAddress` records
page provenance so the resolver can select separate backing storage when a DMA
descriptor is programmed or first used. Audio retains that backing in
descriptor-scoped bindings instead of resolving every fetch through mutable
page provenance. Otherwise an unrelated allocation in the other bank can
redirect a resident sound whose numeric physical address has not changed.

Bindings cover voice/notification tables, frontend completion writes, SGE and
SSL sources, and diagnostic GP/EP scratch mailboxes. SGE copies split at page
boundaries, including non-adjacent physical pages; physically contiguous SSL
buffers retain their captured bank across the linear extent. Bound accesses
are checked against the bank span. Integrations must provide persistent,
contiguous host backing for each RAM bank; the current constructor assumes
64 MiB per bank.

Programming a table/base register again invalidates its descriptor bindings,
even when the numeric value is unchanged. Explicit SGE/SSL offset programming
also recaptures source backing for same-value reallocation. Changed descriptor
addresses written directly into bound table memory are detected on access.
VP reset invalidates its tables; GP/EP reset-register writes invalidate their
scratch bindings. Cache capacity is reused across resets and released at
shutdown. GP/EP register reads and instruction execution remain stubbed.

A direct guest descriptor write of the same physical value cannot identify a
bank change without an explicit programming/reset event. Unused descriptors
are bound on first access, not retroactively at allocation time. This is not a
unified physical-memory/alias model and does not change guest physical-address
encoding or GPU consumers.

The legacy `mcpx_apu_init_standalone` constructor retains flat-RAM behavior
for hosts with a single linear physical bank. Other integrations must supply
their actual physical backing or a mapper, not an unrelated virtual mapping.

The VP produces 32 samples per tick. Eight DSP slices assemble one 256-frame
stereo packet, submitted once every 5.33ms. The monitor preserves that packet
and mixes software voices into it; it must not clear hardware output or
generate 1024/2048 replacement frames each tick. Muting clears the packet,
and each tick clears only its current 32-frame slice before either the hardware
or inactive-hardware path runs. Temporary front-end traps, halts and counter
disables must not erase earlier slices in the packet. Preparing active slices
also prevents VP-monitor accumulation when the eight-slice buffer wraps.

XAudio2 queue saturation applies backpressure while releasing the APU lock.
The waveOut fallback also waits before reusing an in-flight buffer. Submission
failures and two-second queue stalls are reported and stop host output,
rather than silently dropping packets or overwriting a playing buffer.
Windows requests 1ms timer resolution for this audio cadence and balances
the request on monitor shutdown.

The Windows producer uses MMCSS Audio scheduling. Packet deadlines retain
up to eight packet intervals of catch-up rather than discarding time after
each delayed tick. Longer stalls rebase the deadline, and queue backpressure
still bounds submission.

`NV_PAPU_XGSCNT` counts processed 48kHz samples (32 per VP slice), not
100ns clock ticks. The counter wraps as an unsigned 32-bit sample count.

Set `RECOMP_APU_DIAG=1` to log accepted output blocks, stereo frames, nonzero
scalar samples, nonzero hardware samples, peak amplitude, test-tone and mute
state. It also logs voice starts and interval source/routed-contribution
peaks, retaining short-lived effects until the next report. Routed peaks are
per-source contributions, not the final combined DSP output.
Unset disables diagnostics in the runtime; `0` explicitly disables them across
the VP, output report and XAudio2 backend. Ghost currently defaults the setting
to `1` while its mission-reload degradation is under investigation.
Route reports include the current voice format/base, most recently resolved
payload physical address and backing pointer, raw SSL descriptor format and interval format-mismatch
counts. These fields help distinguish bad source data/mapping from filter,
mixdown and queue faults; collecting them does not change the decoding rules.
`adpcm_hash` is a 32-bit FNV-1a fingerprint of the latest fetched compressed
block, with its length in `adpcm_bytes`. It can be compared against local XWB
blocks without recording audio payloads. PCM and multipass voices report zero
length. `adpcm_bad_headers` counts fetched channel headers whose raw step index
exceeds 88 during the reporting interval; existing decoder clamping is unchanged.
The hash/address describe the latest fetch, whereas interval peaks and error
counts can span multiple sounds that reuse one hardware voice.
`RECOMP_APU_TRACE=1` traces MMIO. Nonzero samples alone do not establish
correct DMA mapping, effects fidelity or a complete soundtrack.
GP/EP remain passthrough stubs; AC97-ready and DSP-ack diagnostic overrides are
not DSP56300 emulation.

### Voice Reuse and Mission Restarts

`VOICE_OFF` preserves linked-list membership and completion notifications for
guest idle-voice handling. `VOICE_ON` now removes an existing occurrence before
reinserting that hardware handle, including moves between the 2D, 3D and
multipass lists. Inherited insertion preserves other voices and the pitch bits
sharing the link word. A linked self antecedent restarts in place; an unlinked
self antecedent is rejected with a diagnostic. Guest voice locks and normal
envelope, stream and filter resets are preserved.

Previously, stopping and starting a still-linked head could create a self-loop.
An offscreen regression reproduced a quiet 0.01 signal becoming approximately
2.56 after the walker mixed the same voice 256 times. Looping PCM reuse also
advanced the source repeatedly and drove the combined signal above full scale.
This can cause severe clipping without an output-device or queue failure.

The frame walker now processes each handle at most once across all three lists,
reporting invalid handles, cycles and cross-list duplicates as `[APU-LIST]`.
Reports are rate-limited after the first eight (then powers of two); total counts
remain visible in `RECOMP_APU_DIAG=1` output as `list_errors`. `relinks` counts
starts that detached an existing entry. `fe_trap_ticks` and `fe_halt_ticks` count
lightweight ticks caused by those front-end modes, while `inactive_ticks`
counts all ticks skipping the hardware pipeline (including counter-off and
test-tone operation). These counters reset with the VP.

The `apu_voice_restart` regression in `tests/apu_mixdown` exercises 100
stop/start cycles each for multipass and PCM sources, stable source advancement,
unclipped PCM output, active head/middle/tail reuse, list migration, inherited
and self-antecedent insertion, pitch preservation, notifications, guest locks,
natural completion and malformed-list guards. It also invokes the actual core
frame tick to check a trap, halt or counter disable at every packet phase,
stopped-voice idle-trap interrupts, and 32 VP-monitor packets without
accumulation. It uses real VP methods and the
DSP passthrough without an output device or background audio thread.
This reproduces and fixes a hardware-voice accumulation bug; StarCraft: Ghost
mission-reload audio still requires a controlled gameplay verification.

### Diagnostic DSP Command Completion

`RECOMP_APU_DSP_ACK` accepts comma-separated aligned physical addresses,
`gp:<byte offset>` or `ep:<byte offset>`. The processor-relative forms resolve
through the programmed scratch scatter/gather table (`GPSADDR`/`EPSADDR`),
with its SGE limit checked before accessing the selected page. These registers
point to tables, not directly to the command buffer.

Command offsets depend on the guest's DSP protocol and are not universal
runtime defaults. Fixed mailbox addresses can become stale when allocation
order changes; processor-relative addressing follows the programmed scratch
allocation. An explicitly empty setting disables acknowledgement.

Diagnostic command acknowledgement runs in the regular APU frame loop after
throttling, independently of voice-front-end execution. Gating acknowledgement
on voice processing can leave a guest audio worker polling the mailbox while
holding a lock after voices stop. Emulator pause still suspends frame servicing.
Acknowledgement scheduling does not add DSP instruction execution.

This mode explicitly logs **diagnostic passthrough** and clears command words
without executing their DSP programs. It does not implement effects or prove
correct command results. Guest audio state transitions, sustained playback,
spatial/effects fidelity and soundtrack coverage require separate validation.
Audible source output alone does not establish complete audio compatibility.

## Files

| File | LOC | Purpose |
|------|-----|---------|
| `apu.h` | 70 | **Public API** — init, shutdown, MMIO, mixer voices |
| `apu_state.h` | 525 | APU state struct (VP, GP, EP, DSP core, 256 voices) |
| `apu_regs.h` | 366 | MCPX APU register definitions and field masks |
| `apu_core.c` | 762 | MMIO handlers, frame thread, waveOut output, test tone |
| `apu_vp.c` | 1,247 | Voice Processor — decode, resample, mix, envelopes, HRTF |
| `apu_dsp.c` | 90 | GP/EP stubs (mixbin 0/1 passthrough) |
| `apu_mmio_hook.c` | 253 | VEH x86-64 instruction decoder for 0xFE800000+ MMIO |
| `apu_shim.h` | 431 | QEMU compatibility (threads, atomics, clocks → Win32) |
| `apu_debug.h` | 104 | Debug logging macros |
| `fpconv.h` | 70 | IEEE 754 float/int conversion utilities |

## Quick Start

```c
#include "apu.h"

// Initialize APU with pointer to Xbox RAM
// (ram_ptr = native pointer to Xbox VA 0x00000000)
MCPXAPUState *apu = mcpx_apu_init_standalone(ram_ptr);

// Test: play a 440Hz sine tone
mcpx_apu_play_test_tone(apu);

// --- OR use the software mixer for direct PCM playback ---

// Allocate a mixer voice
int voice = apu_mixer_alloc_voice();
APUMixerVoice *v = apu_mixer_get_voice(voice);

// Configure it
v->pcm_data = my_pcm_buffer;     // 16-bit signed PCM
v->pcm_bytes = buffer_size;
v->num_channels = 2;              // 1=mono, 2=stereo
v->sample_rate = 44100;           // Source sample rate
v->volume = 0.8f;                 // 0.0 - 1.0

// Play
apu_mixer_play(voice, 1);  // 1 = looping

// Stop and free
apu_mixer_stop(voice);
apu_mixer_free_voice(voice);

// Shutdown
mcpx_apu_shutdown(apu);
```

## Architecture

```
┌─────────────────────────────────────────────────┐
│                    Xbox Game                     │
│  DirectSound API calls (IDirectSound8)           │
└────────────────────┬────────────────────────────┘
                     │ writes to MMIO registers
                     ▼
┌─────────────────────────────────────────────────┐
│              APU Register Space                  │
│  0xFE800000 - 0xFE87FFFF (512 KB)               │
│  Intercepted via VEH (apu_mmio_hook.c)           │
└────────┬───────────┬───────────┬────────────────┘
         │           │           │
         ▼           ▼           ▼
   ┌──────────┐ ┌─────────┐ ┌─────────┐
   │    VP    │ │   GP    │ │   EP    │
   │256 voices│ │  (stub) │ │  (stub) │
   │ ADPCM/  │ │  effects│ │  encode │
   │  PCM    │ │  bypass │ │  bypass │
   └────┬─────┘ └────┬────┘ └────┬────┘
        │             │           │
        └──────┬──────┘───────────┘
               │ mixed samples
               ▼
   ┌──────────────────────────────┐
   │ XAudio2 / waveOut, 48kHz     │
   │ 256-frame stereo packets    │
   └──────────────────────────────┘
```

## Two Audio Paths

### 1. Hardware APU Emulation (MMIO)

For games that program the APU directly. The game writes to MMIO registers at 0xFE800000+, which are intercepted by the VEH hook and routed to the APU emulation:

```c
// Game writes: voice 5, set buffer address
*(uint32_t*)(0xFE802000 + 5 * voice_stride + VOICE_BUF_OFFSET) = buffer_addr;

// VEH intercepts → mcpx_apu_mmio_write() → VP processes voice
```

The VP runs in a separate thread, mixing active voices at the hardware frame rate.

### 2. Software Mixer (Direct API)

For games where you've decoded the audio format yourself and just want to play PCM. The mixer provides 64 voice slots with:

- 16-bit signed PCM input (mono or stereo)
- 16.16 fixed-point resampling (any rate → 48kHz)
- Per-voice volume control
- Looping support

```c
int apu_mixer_alloc_voice(void);                    // Returns slot 0-63, or -1
void apu_mixer_free_voice(int slot);
APUMixerVoice *apu_mixer_get_voice(int slot);       // Configure voice params
void apu_mixer_play(int slot, int looping);          // Start playback
void apu_mixer_stop(int slot);                       // Stop playback
```

## MMIO Hook

The APU MMIO hook (`apu_mmio_hook.c`) uses Windows Vectored Exception Handling to intercept memory accesses to the APU register range:

```
Access to 0xFE800000+
  → Page fault (no physical memory there)
  → VEH handler catches EXCEPTION_ACCESS_VIOLATION
  → Decodes the faulting x86-64 instruction (MOV, ADD, XOR, etc.)
  → Routes to mcpx_apu_mmio_read() or mcpx_apu_mmio_write()
  → Advances RIP past the instruction
  → Resumes execution
```

This allows recompiled code to access APU registers using normal memory operations, exactly as the original Xbox code did.

## Voice Processor Details

The VP processes 256 hardware voices per frame:

- **ADPCM decode**: Xbox ADPCM (4-bit, 64-sample blocks)
- **PCM formats**: 8-bit unsigned, 16-bit signed, 24-bit
- **Pitch**: Log2 fixed-point (4.12 format). Pitch 0 = 48kHz native rate.
- **Envelopes**: Multi-segment amplitude envelopes (attack, decay, sustain, release)
- **HRTF**: Head-Related Transfer Function for 3D positional audio
- **Mixbins**: 32 output channels, voices route to any combination
- **Filters**: Per-voice low-pass/high-pass (IIR biquad)

## Register Map (Key Registers)

```
Offset      Name                   Description
──────────────────────────────────────────────────
0x1000      NV_PAPU_ISTS           Interrupt status
0x1100      NV_PAPU_FECTL          Frontend control
0x2000      NV_PAPU_SECTL          Synth engine control
0x202C      NV_PAPU_VPVADDR        VP voice address base
0x3000+     NV_PAPU_GP*            Global Processor registers
0x4000+     NV_PAPU_EP*            Encode Processor registers
```

Full register definitions in `apu_regs.h`.

## Dependencies

- `qemu_shim.h` (from `../nv2a/`) — QEMU type abstraction layer
- `kernel32.lib` — Win32 threading
- `winmm.lib` — waveOut audio output
- `xaudio2_8.lib`, `ole32.lib` — primary Windows audio output
