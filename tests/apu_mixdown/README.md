# apu_mixdown

`mcpx_apu_dsp_frame` used to read mixbins 0 and 1 and discard 2..31. On
hardware the GP and EP mix the submixes down; here they are stubs, so thirty
of thirty-two bins were computed correctly and thrown away every frame, with
no counter anywhere to say so.

Titles route their 3D positional voices — their sound effects — to bins above
1. Measured on Jet Set Radio Future over one 200 s gameplay run:

    [APU-BIN] 2D heard=557466 lost=0
              3D heard=0      lost=377768
              lost by bin: 6,7,8,9,10

Music is on 2D voices and lands in bins 0 and 1, which is why it was always
audible while no effect ever was.

## Running

    cmake -S tests/apu_mixdown -B build/apu-mixdown
    cmake --build build/apu-mixdown
    ctest --test-dir build/apu-mixdown

The two mixdown tests both matter. `apu_mixdown_all` puts a signal only in bins 6..10
and requires it to reach the frame buffer. `apu_mixdown_two_bins` requires the
same signal to be *lost* with the switch off — without that arm, a mixdown
that ignored its own switch would pass.

`apu_voice_restart` also tests the real VP list insertion and processing path.
It repeats stopped multipass and looping PCM voices 100 times each and requires
stable amplitude, unclipped PCM output and identical PCM source advancement.
It covers active head/middle/tail reuse, list migration, inherited insertion,
self antecedents, pitch preservation, completion notifications, guest locks,
natural completion and cycle/invalid-handle/cross-list duplicate detection.
No audible output or running game is needed.

Core frame-tick regressions inject a trap, halt or counter disable at every one
of the eight packet phases and preserve all preceding slices. They also use
real stopped-voice idle traps and require 32 VP-monitor packets to retain
constant amplitude without accumulating the preceding packet.

The voice fixture runs with diagnostics both off and on. Streaming PCM cases
verify source format/backing capture and SSL descriptor mismatch counts without
changing decoded output. `apu_diagnostics_off`, `apu_diagnostics_on` and
`apu_diagnostics_unset` verify the cached runtime switch in separate processes.
Mono/stereo streaming and non-streaming ADPCM cases verify exact compressed-block fingerprints,
invalid step-index detection in the second channel, unchanged decoded samples,
and clearing of fingerprints when a voice is reused for PCM. Non-streaming
blocks cross an SGE page boundary into a non-adjacent physical page.

DMA collision cases switch the mapper to a second full RAM bank after source
programming. Both PCM and ADPCM, through SGE and SSL descriptors, must retain
the original 0.125 signal and backing rather than decode alternate data.
Explicitly programming the same source address must intentionally select the
new bank and produce 0.25. Additional cases cover descriptor-cache growth,
direct changed-address writes, table relocation, length-only SSL programming,
same-value table-base programming, VP reset, frontend completion writes,
voice state and completion notifications.

`apu_dsp_dma` uses a separate process with
`RECOMP_APU_DSP_ACK=gp:0x810,ep:0x1810`, because acknowledgement configuration is
cached. It checks original GP/EP scratch-table and mailbox ownership after the
mapper switches banks, low-bit page flags, same-value base-register rebinding,
and DSP reset-register invalidation. It tests the diagnostic handshake only,
not DSP instruction execution or effect results. No test opens an audio device.
