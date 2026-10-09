#include "apu_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static const hwaddr tops[] = {NV_PAPU_TVL2D, NV_PAPU_TVL3D, NV_PAPU_TVLMP};
static uint8_t alias_bank[1024 * 1024];
static uint8_t *const alias_pages = alias_bank + 0x60000;
static bool alias_selected;

static uint8_t *alias_mapper(uint64_t physical, uint32_t bytes)
{
    if (physical + bytes > 1024 * 1024) return NULL;
    if (alias_selected)
        return alias_bank + physical;
    return g_apu_ram_ptr + physical;
}

static void check(const char *name, bool passed)
{
    if (!passed) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

static uint32_t *voice_word(MCPXAPUState *d, uint16_t voice, hwaddr offset)
{
    return (uint32_t *)(d->ram_ptr + d->regs[NV_PAPU_VPVADDR] + voice * NV_PAVS_SIZE + offset);
}

static void setup_voice(MCPXAPUState *d, uint16_t voice)
{
    *voice_word(d, voice, NV_PAVS_VOICE_CFG_FMT) = NV_PAVS_VOICE_CFG_FMT_MULTIPASS | (31u << 16);
    *voice_word(d, voice, NV_PAVS_VOICE_TAR_VOLA) = 0xFFFF000Fu;
    *voice_word(d, voice, NV_PAVS_VOICE_TAR_VOLB) = UINT32_MAX;
    *voice_word(d, voice, NV_PAVS_VOICE_TAR_VOLC) = UINT32_MAX;
    *voice_word(d, voice, NV_PAVS_VOICE_TAR_PITCH_LINK) = 0xFFFF;
}

static void start_voice(MCPXAPUState *d, uint16_t voice, uint32_t antecedent)
{
    mcpx_apu_vp_write(d, NV1BA0_PIO_SET_ANTECEDENT_VOICE, antecedent, 4);
    mcpx_apu_vp_write(d, NV1BA0_PIO_VOICE_ON, voice, 4);
}

static bool unique_lists(MCPXAPUState *d, unsigned expected)
{
    bool seen[MCPX_HW_MAX_VOICES] = {false};
    unsigned count = 0;
    for (unsigned list = 0; list < 3; list++) {
        uint32_t voice = d->regs[tops[list]];
        while (voice != 0xFFFF) {
            if (voice >= MCPX_HW_MAX_VOICES || seen[voice]) return false;
            seen[voice] = true; count++;
            voice = *voice_word(d, (uint16_t)voice, NV_PAVS_VOICE_TAR_PITCH_LINK) & 0xFFFF;
        }
    }
    return count == expected;
}

static void check_level(MCPXAPUState *d, float expected)
{
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = {{0}};
    for (unsigned sample = 0; sample < NUM_SAMPLES_PER_FRAME; sample++) mixbins[31][sample] = 0.01f;
    mcpx_apu_vp_frame(d, mixbins);
    mcpx_apu_dsp_frame(d, mixbins);
    unsigned offset = (d->ep_frame_div++ % 8) * NUM_SAMPLES_PER_FRAME;
    for (unsigned sample = 0; sample < NUM_SAMPLES_PER_FRAME; sample++)
        if (fabsf(mixbins[0][sample] - expected) > 0.000001f) {
            fprintf(stderr, "FAIL: restarted voice level %.8f, expected %.8f\n", mixbins[0][sample], expected);
            failures++;
            return;
        }
    for (unsigned sample = 0; sample < NUM_SAMPLES_PER_FRAME; sample++)
        if (abs(d->monitor.frame_buf[offset + sample][0] -
                (int16_t)(expected * 32767.0f)) > 1) {
            check("restarts retain expected unclipped PCM output", false);
            return;
        }
}

static bool slice_matches(MCPXAPUState *d, unsigned slice, int left, int right)
{
    for (unsigned sample = 0; sample < NUM_SAMPLES_PER_FRAME; sample++) {
        unsigned offset = slice * NUM_SAMPLES_PER_FRAME + sample;
        if (abs(d->monitor.frame_buf[offset][0] - left) > 1 ||
            abs(d->monitor.frame_buf[offset][1] - right) > 1) return false;
    }
    return true;
}

static void check_packet_slices(MCPXAPUState *d)
{
    const uint32_t enabled_sectl = 1u << ctz32(NV_PAPU_SECTL_XCNTMODE);
    const uint32_t modes[] = {NV_PAPU_FECTL_FEMETHMODE_TRAPPED,
                             NV_PAPU_FECTL_FEMETHMODE_HALTED,
                             NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING};
    qemu_mutex_lock(&d->lock);
    d->monitor.point = MCPX_APU_DEBUG_MON_AC97;
    for (unsigned mode = 0; mode < ARRAY_SIZE(modes); mode++) {
        for (unsigned pause = 0; pause < 8; pause++) {
            d->ep_frame_div = 0;
            memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
            start_voice(d, 83, 1u << 16);
            for (unsigned slice = 0; slice < 8; slice++) {
                d->regs[NV_PAPU_SECTL] = mode == 2 && slice == pause
                    ? 0 : enabled_sectl;
                d->regs[NV_PAPU_FECTL] = slice == pause ? modes[mode]
                    : NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING;
                mcpx_apu_frame_tick(d);
                check("active and inactive ticks each advance one slice",
                      d->ep_frame_div == slice + 1);
                bool intact = true;
                for (unsigned previous = 0; previous <= slice; previous++)
                    intact &= slice_matches(d, previous, previous == pause ? 0 : 4095, 0);
                check("temporary trap/halt/disable preserves preceding packet slices", intact);
            }
        }
    }
    check("diagnostics count every injected inactive tick",
          d->vp.frontend_trapped_ticks == 8 &&
          d->vp.frontend_halted_ticks == 8 && d->vp.inactive_ticks == 24);
    d->regs[NV_PAPU_SECTL] = enabled_sectl;
    memcpy(voice_word(d, 84, 0), voice_word(d, 83, 0), NV_PAVS_SIZE);
    start_voice(d, 84, 1u << 16);
    mcpx_apu_vp_write(d, NV1BA0_PIO_VOICE_OFF, 83, 4);
    d->regs[NV_PAPU_FETFORCE1] = NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE;
    d->ep_frame_div = 0;
    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
    for (unsigned slice = 0; slice < 8; slice++) {
        if (!(slice & 1)) d->regs[NV_PAPU_FECTL] = NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING;
        mcpx_apu_frame_tick(d);
        check("stopped voice requests its normal idle trap",
              (d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE) ==
                  NV_PAPU_FECTL_FEMETHMODE_TRAPPED &&
              (d->regs[NV_PAPU_ISTS] & NV_PAPU_ISTS_FETINTSTS) != 0);
    }
    bool preserved = true;
    for (unsigned slice = 0; slice < 8; slice++)
        preserved &= slice_matches(d, slice, slice & 1 ? 0 : 4095, 0);
    check("real idle traps do not discard the playing voice's preceding slices", preserved);
    d->regs[NV_PAPU_FETFORCE1] = 0;
    mcpx_apu_vp_write(d, NV1BA0_PIO_VOICE_OFF, 84, 4);
    for (unsigned list = 0; list < 3; list++) d->regs[tops[list]] = 0xFFFF;
    d->regs[NV_PAPU_FECTL] = NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING;
    d->monitor.point = MCPX_APU_DEBUG_MON_VP;
    d->ep_frame_div = 0;
    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
    start_voice(d, 83, 1u << 16);
    for (unsigned packet = 0; packet < 32; packet++) {
        for (unsigned slice = 0; slice < 8; slice++) mcpx_apu_frame_tick(d);
        bool stable = true;
        for (unsigned slice = 0; slice < 8; slice++)
            stable &= slice_matches(d, slice, 4096, 4096);
        check("VP monitor does not accumulate previous packets", stable);
    }
    d->monitor.point = MCPX_APU_DEBUG_MON_AC97;
    qemu_mutex_unlock(&d->lock);
}

static void check_adpcm_source_diagnostics(MCPXAPUState *d)
{
    const uint32_t hashes[] = {0x4E7ED6E5, 0xFF9C1555, 0x25B7CA42};
    for (unsigned test = 0; test < 6; test++) {
        unsigned variant = test % 3;
        unsigned channels = variant == 0 ? 1 : 2;
        bool stream = test < 3;
        mcpx_apu_vp_reset(d);
        for (unsigned list = 0; list < 3; list++) d->regs[tops[list]] = 0xFFFF;
        setup_voice(d, 83);
        *voice_word(d, 83, NV_PAVS_VOICE_CFG_FMT) =
            (stream ? NV_PAVS_VOICE_CFG_FMT_DATA_TYPE : NV_PAVS_VOICE_CFG_FMT_LOOP) |
            NV_PAVS_VOICE_CFG_FMT_PERSIST |
            (channels == 2 ? NV_PAVS_VOICE_CFG_FMT_STEREO : 0) |
            ((channels - 1) << 16) |
            (NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24 << 28) |
            (NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM << 30);
        uint8_t block[72] = {0};
        block[1] = 0x10;
        if (channels == 2) block[5] = 0x20;
        if (variant == 2) block[6] = 255;
        hwaddr source = stream ? 0x60000 : 0x60FF0;
        if (stream) {
            memcpy(d->ram_ptr + source, block, sizeof(block));
        } else {
            memcpy(d->ram_ptr + source, block, 16);
            memcpy(d->ram_ptr + 0x65000, block + 16, sizeof(block) - 16);
            d->regs[NV_PAPU_VPSGEADDR] = 0x50000;
            *(uint32_t *)(d->ram_ptr + 0x50000) = 0x60000;
            *(uint32_t *)(d->ram_ptr + 0x50000 + NV_PSGE_SIZE) = 0x65000;
            *voice_word(d, 83, NV_PAVS_VOICE_CUR_PSL_START) = 0xFF0;
            *voice_word(d, 83, NV_PAVS_VOICE_PAR_NEXT) = 63;
        }
        d->regs[NV_PAPU_VPSSLADDR] = 0x70000;
        d->regs[NV_PAPU_FECV] = 83;
        mcpx_apu_vp_write(d, NV1BA0_PIO_SET_CURRENT_SSL, 0, 4);
        mcpx_apu_vp_write(d, NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET, 0x60000, 4);
        uint32_t descriptor = 1024 |
            (NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM << 16) |
            ((channels - 1) << 18) | (channels == 2 ? 1u << 23 : 0);
        mcpx_apu_vp_write(d, NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH, descriptor, 4);
        mcpx_apu_vp_write(d, NV1BA0_PIO_SET_VOICE_SSL_A, 1, 4);
        mcpx_apu_vp_write(d, NV1BA0_PIO_SET_VOICE_SSL_B, 0, 4);
        g_dbg.vp.v[83].adpcm_bad_headers_since_report = 0;
        start_voice(d, 83, 1u << 16);
        check_level(d, 0.125f);
        if (channels == 2)
            check("ADPCM diagnostics preserve right-channel decoding",
                  fabsf(d->vp.filters[83].resample_buf[0][1] - 0.25f) < 0.000001f);
        if (mcpx_apu_diagnostics_enabled()) {
            check("ADPCM fingerprint covers the exact fetched compressed block",
                  g_dbg.vp.v[83].adpcm_block_hash == hashes[variant] &&
                  g_dbg.vp.v[83].adpcm_block_bytes == 36 * channels &&
                  g_dbg.vp.v[83].source_physical == source &&
                  g_dbg.vp.v[83].source_storage == d->ram_ptr + source);
            check("invalid ADPCM step headers are diagnosed in either channel",
                  (g_dbg.vp.v[83].adpcm_bad_headers_since_report != 0) == (variant == 2));
        } else {
            check("disabled diagnostics do not fingerprint or scan ADPCM headers",
                  g_dbg.vp.v[83].adpcm_block_hash == 0 &&
                  g_dbg.vp.v[83].adpcm_block_bytes == 0 &&
                  g_dbg.vp.v[83].adpcm_bad_headers_since_report == 0);
        }
    }
}

static void check_dma_backing_case(MCPXAPUState *d, bool stream, bool pcm)
{
    uint32_t container = pcm ? NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B16
                             : NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM;
    d->physical_mapper = alias_mapper;
    alias_selected = false;
    mcpx_apu_vp_reset(d);
    for (unsigned list = 0; list < 3; list++) d->regs[tops[list]] = 0xFFFF;
    setup_voice(d, 83);
    *voice_word(d, 83, NV_PAVS_VOICE_CFG_FMT) =
        (stream ? NV_PAVS_VOICE_CFG_FMT_DATA_TYPE | NV_PAVS_VOICE_CFG_FMT_PERSIST
                : NV_PAVS_VOICE_CFG_FMT_LOOP) |
        ((pcm ? NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16
              : NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24) << 28) |
        (container << 30);
    *voice_word(d, 83, NV_PAVS_VOICE_CUR_PSL_START) = 0;
    *voice_word(d, 83, NV_PAVS_VOICE_PAR_NEXT) = pcm ? 1023 : 63;
    memset(d->ram_ptr + 0x60000, 0, 4096);
    for (unsigned offset = 1; offset < (pcm ? 4096u : 2u); offset += 2)
        d->ram_ptr[0x60000 + offset] = 0x10;
    memset(alias_pages, 0xFF, 8192);
    alias_pages[1] = 0x70;
    d->regs[NV_PAPU_VPSGEADDR] = 0x50000;
    mcpx_apu_vp_write(d, NV1BA0_PIO_SET_CURRENT_INBUF_SGE, 0, 4);
    mcpx_apu_vp_write(d, NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET, 0x60000, 4);
    d->regs[NV_PAPU_VPSSLADDR] = 0x70000;
    d->regs[NV_PAPU_FECV] = 83;
    mcpx_apu_vp_write(d, NV1BA0_PIO_SET_CURRENT_SSL, 0, 4);
    mcpx_apu_vp_write(d, NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET, 0x60000, 4);
    mcpx_apu_vp_write(d, NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH, 1024 | (container << 16), 4);
    mcpx_apu_vp_write(d, NV1BA0_PIO_SET_VOICE_SSL_A, 1, 4);
    mcpx_apu_vp_write(d, NV1BA0_PIO_SET_VOICE_SSL_B, 0, 4);
    g_dbg.vp.v[83].adpcm_bad_headers_since_report = 0;
    start_voice(d, 83, 1u << 16);
    alias_selected = true;
    check("unrelated translation really changes the unbound physical mapper",
          mcpx_apu_ram_address(0x60000, 4) == alias_pages);
    check_level(d, 0.125f);
    check("PCM/ADPCM SGE/SSL retains original storage after a backing-bank collision",
          !mcpx_apu_diagnostics_enabled() ||
          (g_dbg.vp.v[83].source_physical >= 0x60000 &&
           g_dbg.vp.v[83].source_physical < 0x61000 &&
           g_dbg.vp.v[83].source_storage == d->ram_ptr + g_dbg.vp.v[83].source_physical &&
           g_dbg.vp.v[83].adpcm_bad_headers_since_report == 0));
    memset(alias_pages, 0, 8192);
    for (unsigned offset = 1; offset < (pcm ? 4096u : 2u); offset += 2)
        alias_pages[offset] = 0x20;
    mcpx_apu_vp_write(d, stream ? NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET
                              : NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET, 0x60000, 4);
    start_voice(d, 83, 1u << 16);
    check_level(d, 0.25f);
    check("explicit SGE/SSL reprogramming binds the newly allocated backing",
          !mcpx_apu_diagnostics_enabled() ||
          (g_dbg.vp.v[83].source_physical >= 0x60000 &&
           g_dbg.vp.v[83].source_physical < 0x61000 &&
           g_dbg.vp.v[83].source_storage ==
               alias_pages + (g_dbg.vp.v[83].source_physical - 0x60000)));
    d->physical_mapper = NULL;
    alias_selected = false;
    mcpx_apu_vp_reset(d);
    check("VP reset invalidates cached descriptor ownership",
          !d->vp.sge_table.base.storage && !d->vp.ssl_table.base.storage &&
          !d->vp.sge_table.entries[0].storage && !d->vp.ssl_table.entries[0].storage);
}

static void check_dma_backing(MCPXAPUState *d)
{
    for (unsigned test = 0; test < 4; test++)
        check_dma_backing_case(d, (test & 1) != 0, (test & 2) != 0);
}

static void check_dma_tables(MCPXAPUState *d)
{
    MCPXAPUDmaTable *table = &d->vp.sge_table;
    d->physical_mapper = alias_mapper;
    alias_selected = false;
    mcpx_apu_dma_program_entry(d, table, 0x50000, 0, 0x60000);
    MCPXAPUDmaBinding view = mcpx_apu_dma_sge_address(d, table, 0x50000, 0);
    alias_selected = true;
    for (unsigned entry = 1; entry < 100; entry++)
        mcpx_apu_dma_program_entry(d, table, 0x50000, entry, 0x61000);
    check("descriptor growth preserves captured source views and ownership",
          view.storage == d->ram_ptr + 0x60000 &&
          mcpx_apu_dma_entry(d, table, 0x50000, 0)->storage == view.storage);
    stl_le_p(d->ram_ptr + 0x50000, 0x65000);
    check("direct descriptor changes select the changed source",
          mcpx_apu_dma_entry(d, table, 0x50000, 0)->storage == alias_bank + 0x65000);
    stl_le_p(d->ram_ptr + 0x51000, 0x60000);
    stl_le_p(alias_bank + 0x51000, 0x60000);
    check("table relocation discards old source bindings",
          mcpx_apu_dma_entry(d, table, 0x51000, 0)->storage == alias_pages);
    mcpx_apu_dma_program_entry(d, &d->vp.ssl_table, 0x70000, 0, 0x60000);
    alias_selected = false;
    stl_le_p(d->ram_ptr + 0x71000, 0x60000);
    d->regs[NV_PAPU_VPSSLADDR] = 0x71000;
    mcpx_apu_vp_write(d, NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH, 1024, 4);
    check("length-only SSL programming invalidates relocated table ownership",
          mcpx_apu_dma_entry(d, &d->vp.ssl_table, 0x71000, 0)->storage ==
              d->ram_ptr + 0x60000);
    mcpx_apu_write(d, NV_PAPU_VPSGEADDR, 0x51000, 4);
    check("rewriting a table base discards same-value payload ownership",
          mcpx_apu_dma_entry(d, table, 0x51000, 0)->storage == d->ram_ptr + 0x60000);
    mcpx_apu_write(d, NV_PAPU_FEMEMADDR, 0x60000, 4);
    alias_selected = true;
    mcpx_apu_write(d, NV_PAPU_FEMEMDATA, 0x12345678, 4);
    check("frontend completion writes retain programmed backing",
          ldl_le_p(d->ram_ptr + 0x60000) == 0x12345678);
    mcpx_apu_write(d, NV_PAPU_FEMEMADDR, 0x60000, 4);
    mcpx_apu_write(d, NV_PAPU_FEMEMDATA, 0x87654321, 4);
    check("frontend address reprogramming captures new backing",
          ldl_le_p(alias_pages) == 0x87654321 &&
          ldl_le_p(d->ram_ptr + 0x60000) == 0x12345678);
    alias_selected = false;
    mcpx_apu_vp_reset(d);
    for (unsigned list = 0; list < 3; list++) d->regs[tops[list]] = 0xFFFF;
    mcpx_apu_write(d, NV_PAPU_VPVADDR, 0x1000, 4);
    mcpx_apu_write(d, NV_PAPU_FENADDR, 0x40000, 4);
    setup_voice(d, 0);
    unsigned notify = 0x40000 + 16 * (MCPX_HW_NOTIFIER_BASE_OFFSET +
                                    MCPX_HW_NOTIFIER_SSLA_DONE) + 14;
    d->ram_ptr[notify] = alias_bank[notify] = 0;
    alias_selected = true;
    start_voice(d, 0, 1u << 16);
    check("voice table writes retain programmed backing",
          (*voice_word(d, 0, NV_PAVS_VOICE_PAR_STATE) &
           NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE) != 0 &&
          d->vp.voice_table.storage == d->ram_ptr + 0x1000);
    mcpx_apu_vp_write(d, NV1BA0_PIO_VOICE_OFF, 0, 4);
    check("completion notifications retain programmed backing",
          d->ram_ptr[notify] == 1 && alias_bank[notify] == 0);
    mcpx_apu_write(d, NV_PAPU_VPVADDR, 0x1000, 4);
    mcpx_apu_write(d, NV_PAPU_FENADDR, 0x40000, 4);
    d->ram_ptr[notify] = 0;
    mcpx_apu_vp_write(d, NV1BA0_PIO_VOICE_OFF, 0, 4);
    check("voice/notification table reprogramming captures new backing",
          d->vp.voice_table.storage == alias_bank + 0x1000 &&
          d->ram_ptr[notify] == 0 && alias_bank[notify] == 1);
    alias_selected = false;
    d->physical_mapper = NULL;
    mcpx_apu_vp_reset(d);
}

static void check_dsp_dma(MCPXAPUState *d)
{
    uint32_t *original_gp = (uint32_t *)(d->ram_ptr + 0x60810);
    uint32_t *original_ep = (uint32_t *)(d->ram_ptr + 0x61810);
    uint32_t *alternate_gp = (uint32_t *)(alias_pages + 0x810);
    uint32_t *alternate_ep = (uint32_t *)(alias_pages + 0x1810);
    d->physical_mapper = alias_mapper;
    alias_selected = false;
    stl_le_p(d->ram_ptr + 0x72000, 0x60003);
    stl_le_p(d->ram_ptr + 0x74000 + NV_PSGE_SIZE, 0x6100F);
    mcpx_apu_write(d, NV_PAPU_GPSADDR, 0x72000, 4);
    mcpx_apu_write(d, NV_PAPU_EPSADDR, 0x74000, 4);
    d->regs[NV_PAPU_GPSMAXSGE] = 0;
    d->regs[NV_PAPU_EPSMAXSGE] = 1;
    mcpx_apu_dsp_ack_frame(d);
    alias_selected = true;
    *original_gp = *original_ep = 0x12345678;
    *alternate_gp = *alternate_ep = 0x87654321;
    mcpx_apu_dsp_ack_frame(d);
    check("GP/EP mailboxes retain backing and mask page flags",
          !*original_gp && !*original_ep &&
          *alternate_gp == 0x87654321 && *alternate_ep == 0x87654321);
    *original_gp = *original_ep = 0x11111111;
    stl_le_p(alias_bank + 0x72000, 0x60003);
    stl_le_p(alias_bank + 0x74000 + NV_PSGE_SIZE, 0x6100F);
    mcpx_apu_write(d, NV_PAPU_GPSADDR, 0x72000, 4);
    mcpx_apu_write(d, NV_PAPU_EPSADDR, 0x74000, 4);
    mcpx_apu_dsp_ack_frame(d);
    check("same-value scratch table programming rebinds GP/EP sources",
          !*alternate_gp && !*alternate_ep &&
          *original_gp == 0x11111111 && *original_ep == 0x11111111);
    alias_selected = false;
    *alternate_gp = *alternate_ep = 0x22222222;
    mcpx_apu_mmio_write(d, 0x30000 + NV_PAPU_GPRST, 0, 4);
    mcpx_apu_mmio_write(d, 0x50000 + NV_PAPU_EPRST, 0, 4);
    mcpx_apu_dsp_ack_frame(d);
    check("GP/EP reset programming invalidates scratch ownership",
          !*original_gp && !*original_ep &&
          *alternate_gp == 0x22222222 && *alternate_ep == 0x22222222);
}

static void check_stream_diagnostics(MCPXAPUState *d)
{
    mcpx_apu_vp_reset(d);
    for (unsigned list = 0; list < 3; list++) d->regs[tops[list]] = 0xFFFF;
    for (unsigned sample = 0; sample < 1024; sample++) {
        d->ram_ptr[0x60000 + sample * 2] = 0;
        d->ram_ptr[0x60001 + sample * 2] = 0x10;
    }
    setup_voice(d, 83);
    *voice_word(d, 83, NV_PAVS_VOICE_CFG_FMT) =
        NV_PAVS_VOICE_CFG_FMT_DATA_TYPE | NV_PAVS_VOICE_CFG_FMT_PERSIST |
        (NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16 << 28) |
        (NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B16 << 30);
    d->regs[NV_PAPU_VPSSLADDR] = 0x70000;
    d->regs[NV_PAPU_FECV] = 83;
    mcpx_apu_vp_write(d, NV1BA0_PIO_SET_CURRENT_SSL, 0, 4);
    mcpx_apu_vp_write(d, NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET, 0x60000, 4);
    uint32_t descriptor = 1024 | (NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B16 << 16);
    mcpx_apu_vp_write(d, NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH, descriptor, 4);
    mcpx_apu_vp_write(d, NV1BA0_PIO_SET_VOICE_SSL_A, 1, 4);
    mcpx_apu_vp_write(d, NV1BA0_PIO_SET_VOICE_SSL_B, 0, 4);
    g_dbg.vp.v[83].format_mismatches_since_report = 0;
    start_voice(d, 83, 1u << 16);
    check_level(d, 0.125f);
    check("PCM reuse does not retain an ADPCM fingerprint",
          g_dbg.vp.v[83].adpcm_block_hash == 0 &&
          g_dbg.vp.v[83].adpcm_block_bytes == 0);
    check("matching SSL descriptor has no format warnings",
          g_dbg.vp.v[83].format_mismatches_since_report == 0);
    if (mcpx_apu_diagnostics_enabled()) {
        check("source diagnostics record format and resolved payload storage",
              g_dbg.vp.v[83].format == *voice_word(d, 83, NV_PAVS_VOICE_CFG_FMT) &&
              g_dbg.vp.v[83].ssl_segment_format == descriptor &&
              g_dbg.vp.v[83].source_physical >= 0x60000 &&
              g_dbg.vp.v[83].source_physical < 0x60800 &&
              g_dbg.vp.v[83].source_storage ==
                  d->ram_ptr + g_dbg.vp.v[83].source_physical);
    } else {
        check("disabled diagnostics do not capture payload storage",
              g_dbg.vp.v[83].source_storage == NULL);
    }
    mcpx_apu_vp_write(d, NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH, descriptor | (1u << 23), 4);
    start_voice(d, 83, 1u << 16);
    check_level(d, 0.125f);
    check("mismatched SSL stereo metadata is diagnosed without altering decoding",
          (g_dbg.vp.v[83].format_mismatches_since_report > 0) ==
              mcpx_apu_diagnostics_enabled());
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--diagnostics") == 0 &&
        (strcmp(argv[2], "0") == 0 || strcmp(argv[2], "1") == 0)) {
        check("audio diagnostic setting", mcpx_apu_diagnostics_enabled() == (argv[2][0] == '1'));
        printf("apu_diagnostics: %s\n", failures ? "FAILED" : "ALL PASS");
        return failures ? 1 : 0;
    }
    bool dsp_dma = argc == 2 && strcmp(argv[1], "--dsp-dma") == 0;
    if (argc != 1 && !dsp_dma) {
        fprintf(stderr, "Usage: apu_voice_restart_test [--diagnostics 0|1 | --dsp-dma]\n");
        return 2;
    }
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    uint8_t *ram = (uint8_t *)calloc(1, 1024 * 1024);
    if (!d || !ram) { fprintf(stderr, "APU restart fixture allocation failed\n"); return 2; }
    d->ram_ptr = ram; d->ram_size = 1024 * 1024;
    g_apu_ram_ptr = ram; g_state = d;
    qemu_mutex_init(&d->lock); qemu_cond_init(&d->cond);
    mcpx_apu_vp_reset(d);
    d->regs[NV_PAPU_VPVADDR] = 0x1000;
    d->regs[NV_PAPU_FENADDR] = 0x40000;
    if (dsp_dma) {
        check_dsp_dma(d);
        goto cleanup;
    }
    for (unsigned list = 0; list < 3; list++) d->regs[tops[list]] = 0xFFFF;
    setup_voice(d, 80); setup_voice(d, 81); setup_voice(d, 82);

    start_voice(d, 80, 1u << 16);
    check("first start forms one acyclic voice", unique_lists(d, 1));
    check_level(d, 0.01f);
    for (unsigned restart = 0; restart < 100; restart++) {
        mcpx_apu_vp_write(d, NV1BA0_PIO_VOICE_OFF, 80, 4);
        check("stop clears active state", !(*voice_word(d, 80, NV_PAVS_VOICE_PAR_STATE) & NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE));
        check("stop retains membership for guest idle handling", unique_lists(d, 1));
        start_voice(d, 80, 1u << 16);
        check("repeated stop/start retains one acyclic voice", unique_lists(d, 1));
        check_level(d, 0.01f);
    }
    start_voice(d, 81, 1u << 16);
    start_voice(d, 82, 1u << 16);
    check("three independent voices remain linked", unique_lists(d, 3));
    check_level(d, 0.03f);
    start_voice(d, 81, 1u << 16);
    check("active middle-voice restart preserves other voices", unique_lists(d, 3));
    check_level(d, 0.03f);
    start_voice(d, 81, 3u << 16);
    check("moving voice to multipass list does not duplicate it", unique_lists(d, 3));
    check_level(d, 0.03f);
    start_voice(d, 81, 80);
    check("inherited insertion preserves unique lists", unique_lists(d, 3));
    check_level(d, 0.03f);
    check("inherited insertion follows its antecedent",
          (*voice_word(d, 80, NV_PAVS_VOICE_TAR_PITCH_LINK) & 0xFFFF) == 81);
    start_voice(d, 81, 1u << 16);
    check("tail-to-head reuse preserves all voices", unique_lists(d, 3));
    start_voice(d, 81, 1u << 16);
    check("active head reuse preserves all voices", unique_lists(d, 3));
    uint32_t saved_link = *voice_word(d, 81, NV_PAVS_VOICE_TAR_PITCH_LINK);
    start_voice(d, 81, 81);
    check("linked self antecedent restarts in place",
          d->regs[NV_PAPU_TVL2D] == 81 &&
          *voice_word(d, 81, NV_PAVS_VOICE_TAR_PITCH_LINK) == saved_link &&
          unique_lists(d, 3));
    check_level(d, 0.03f);
    *voice_word(d, 82, NV_PAVS_VOICE_TAR_PITCH_LINK) |= 0x12340000;
    *voice_word(d, 80, NV_PAVS_VOICE_TAR_PITCH_LINK) |= 0x56780000;
    start_voice(d, 80, 81);
    check("relink preserves selected and predecessor pitch",
          (*voice_word(d, 80, NV_PAVS_VOICE_TAR_PITCH_LINK) >> 16) == 0x5678 &&
          (*voice_word(d, 82, NV_PAVS_VOICE_TAR_PITCH_LINK) >> 16) == 0x1234);
    check_level(d, 0.03f);
    check("valid reuse has no list errors", d->vp.voice_list_errors == 0);

    mcpx_apu_vp_reset(d);
    for (unsigned list = 0; list < 3; list++) d->regs[tops[list]] = 0xFFFF;
    setup_voice(d, 83);
    *voice_word(d, 83, NV_PAVS_VOICE_CFG_FMT) =
        NV_PAVS_VOICE_CFG_FMT_LOOP |
        (NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16 << 28) |
        (NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B16 << 30);
    *voice_word(d, 83, NV_PAVS_VOICE_PAR_NEXT) = 1023;
    d->regs[NV_PAPU_VPSGEADDR] = 0x50000;
    *(uint32_t *)(ram + 0x50000) = 0x60000;
    for (unsigned sample = 0; sample < 1024; sample++)
        ((int16_t *)(ram + 0x60000))[sample] = 4096;
    start_voice(d, 83, 1u << 16);
    check_level(d, 0.125f);
    uint32_t baseline_cbo = *voice_word(d, 83, NV_PAVS_VOICE_PAR_OFFSET) & 0xFFFFFF;
    check("PCM baseline advances its source", baseline_cbo != 0);
    hwaddr notification = d->regs[NV_PAPU_FENADDR] +
        16 * (MCPX_HW_NOTIFIER_BASE_OFFSET + 83 * MCPX_HW_NOTIFIER_COUNT +
              MCPX_HW_NOTIFIER_SSLA_DONE) + 15;
    for (unsigned restart = 0; restart < 100; restart++) {
        ram[notification] = 0xCD; ram[notification - 1] = 0xCD;
        mcpx_apu_vp_write(d, NV1BA0_PIO_VOICE_OFF, 83, 4);
        check("stop preserves completion notification",
              ram[notification] == NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS &&
              ram[notification - 1] == 1);
        start_voice(d, 83, 1u << 16);
        check("PCM restart resets pending resampler samples",
              d->vp.filters[83].resample_count == 0 &&
              !d->vp.filters[83].resample_current_valid &&
              !d->vp.filters[83].resample_source_ended);
        check_level(d, 0.125f);
        check("PCM restart advances exactly as first playback",
              (*voice_word(d, 83, NV_PAVS_VOICE_PAR_OFFSET) & 0xFFFFFF) == baseline_cbo);
    }
    check("PCM reuse has no list errors", d->vp.voice_list_errors == 0);
    check("PCM reuse is accounted for", d->vp.voice_relinks == 100);
    d->regs[NV_PAPU_FECV] = 83;
    mcpx_apu_vp_write(d, NV1BA0_PIO_VOICE_LOCK, 1, 4);
    start_voice(d, 83, 1u << 16);
    check("reuse preserves guest voice lock", (d->vp.voice_locked[83 / 64] & (1ULL << (83 % 64))) != 0);
    check_level(d, 0.0f);
    mcpx_apu_vp_write(d, NV1BA0_PIO_VOICE_LOCK, 0, 4);
    check_level(d, 0.125f);
    *voice_word(d, 83, NV_PAVS_VOICE_CFG_FMT) &= ~NV_PAVS_VOICE_CFG_FMT_LOOP;
    *voice_word(d, 83, NV_PAVS_VOICE_PAR_NEXT) = 3;
    start_voice(d, 83, 1u << 16);
    float final_mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = {{0}};
    mcpx_apu_vp_frame(d, final_mixbins);
    check("natural PCM completion stops the voice",
          !(*voice_word(d, 83, NV_PAVS_VOICE_PAR_STATE) & NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE));
    *voice_word(d, 83, NV_PAVS_VOICE_CFG_FMT) |= NV_PAVS_VOICE_CFG_FMT_LOOP;
    *voice_word(d, 83, NV_PAVS_VOICE_PAR_NEXT) = 1023;
    start_voice(d, 83, 1u << 16);
    check("naturally completed voice can be reused without duplication", unique_lists(d, 1));
    check_level(d, 0.125f);
    check_packet_slices(d);
    check_adpcm_source_diagnostics(d);
    check_stream_diagnostics(d);
    check_dma_backing(d);
    check_dma_tables(d);

    mcpx_apu_vp_reset(d);
    for (unsigned list = 0; list < 3; list++) d->regs[tops[list]] = 0xFFFF;
    setup_voice(d, 80); setup_voice(d, 81);
    start_voice(d, 80, 1u << 16); start_voice(d, 81, 1u << 16);
    *voice_word(d, 80, NV_PAVS_VOICE_TAR_PITCH_LINK) = 81;
    check_level(d, 0.02f);
    check("cycle is reported and each voice mixes once", d->vp.voice_list_errors == 1);
    *voice_word(d, 80, NV_PAVS_VOICE_TAR_PITCH_LINK) = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 81;
    check_level(d, 0.02f);
    check("cross-list duplicate is reported without remixing", d->vp.voice_list_errors == 2);
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    d->regs[NV_PAPU_TVL3D] = MCPX_HW_MAX_VOICES;
    check_level(d, 0.02f);
    check("invalid list handle is reported before reading voice RAM", d->vp.voice_list_errors == 3);
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    start_voice(d, 82, 82);
    check("unlinked self antecedent is rejected explicitly",
          d->vp.voice_list_errors == 4 && unique_lists(d, 2));
    mcpx_apu_vp_reset(d);
    check("hardware reset clears list diagnostics",
          d->vp.voice_relinks == 0 && d->vp.voice_list_errors == 0 &&
          d->vp.frontend_trapped_ticks == 0 &&
          d->vp.frontend_halted_ticks == 0 && d->vp.inactive_ticks == 0);

cleanup:
    qemu_cond_destroy(&d->cond); qemu_mutex_destroy(&d->lock);
    mcpx_apu_vp_finalize(d);
    mcpx_apu_dsp_finalize(d);
    g_apu_ram_ptr = NULL; g_state = NULL; free(ram); free(d);
    printf("apu_voice_restart: %s\n", failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
