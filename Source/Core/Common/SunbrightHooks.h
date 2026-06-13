// Sunbright fork hooks — replaces the linker --wrap interception mechanism.
//
// Each Dolphin function the Sunbright runtime needs to intercept exposes a function-pointer
// hook slot here. The fork's function is a thin shim:
//
//     void Foo(args) {              // fork shim
//       if (sb_hook_Foo) { sb_hook_Foo(this, args); return; }
//       Foo_impl(args);            // original body (renamed)
//     }
//
// The runtime sets sb_hook_Foo at startup (sb_install_hooks(), runtime/sunbright_hooks.cpp).
// When the hook is null (offline tools sunbright-recomp / sunbright-jingle, or a standalone
// fork build), the shim runs the original body unchanged — so the fork still builds and works
// standalone. The hook function, where it used to call __real_Foo, calls Foo_impl(...) (exposed
// per shim) to run the original Dolphin path.
//
// Each slot is DEFINED (default-null) in the same translation unit as the function it shims, so
// the slot always links with the library that owns the function (Core / VideoCommon /
// AudioCommon). The runtime only DECLARES them (via this header) and assigns them.
//
// This header lives in Common so every Dolphin sublibrary and the Sunbright runtime can include
// it without a cross-lib dependency.

#pragma once

#include "Common/CommonTypes.h"

#include <cstddef>

// All slots take void* self and primitive args to avoid pulling heavy Dolphin headers into Common.

extern "C" {

// ── PixelEngine (VideoCommon) ────────────────────────────────────────────────────────────────
// PixelEngineManager::SetToken — loss-free draw-sync token capture (pe_token_wrap.cpp).
// self = PixelEngineManager*. Hook records the token then calls the *_impl to run Dolphin's body.
extern void (*sb_slot_pe_set_token)(void* self, u16 token, bool interrupt, int cycles_into_future);
void sb_pe_set_token_impl(void* self, u16 token, bool interrupt, int cycles_into_future);

// ── JIT dispatch (Core) ──────────────────────────────────────────────────────────────────────
// JitTrampoline (JitCommon/JitBase.cpp) — the block-dispatch seam. Hook returns true if it ran a
// recompiled block (skip Dolphin's JIT), false to let Dolphin JIT the block. jit = JitBase*.
// This is the last former --wrap symbol, now a fork hook so the build works under ld64/macOS too.
extern bool (*sb_slot_jit_trampoline)(void* jit, u32 em_address);

// ── Indexed XF load (VideoCommon, XFStructs.cpp LoadIndexedXF) ─────────────────────────────────
// The 60fps render-only interpolation seam. When set and it returns true, LoadIndexedXF uses the
// hook-supplied `out` words (big-endian, guest format) for the XF write INSTEAD of reading guest
// RAM — letting the runtime substitute interpolated pos-matrices (lerp of the current and previous
// frame's matrix, read-only) during an in-between replay. Writes only XF/GPU state, never guest RAM.
//   array  = CPArray (12 = XF_A pos-matrix), base/stride = g_main_cp_state for that array,
//   index  = the indexed-load index, size = words to produce, out = caller buffer[size].
extern bool (*sb_slot_xf_indexed)(u32 array, u32 base, u32 stride, u32 index, u32 size, u32* out);

// ── GPFifo (Core) ────────────────────────────────────────────────────────────────────────────
// GPFifoManager::Write8/16/32/64 — foreign gather-pipe write funnel (gpfifo_wrap.cpp).
extern void (*sb_slot_gpfifo_write8)(void* self, u8 v);
extern void (*sb_slot_gpfifo_write16)(void* self, u16 v);
extern void (*sb_slot_gpfifo_write32)(void* self, u32 v);
extern void (*sb_slot_gpfifo_write64)(void* self, u64 v);
void sb_gpfifo_write8_impl(void* self, u8 v);
void sb_gpfifo_write16_impl(void* self, u16 v);
void sb_gpfifo_write32_impl(void* self, u32 v);
void sb_gpfifo_write64_impl(void* self, u64 v);

// ── DSP (Core) ───────────────────────────────────────────────────────────────────────────────
// DSPManager::GenerateDSPInterrupt — native AID ownership (aid_native.cpp).
extern void (*sb_slot_dsp_gen_interrupt)(void* self, u64 dsp_int_type, s64 cycles_late);
void sb_dsp_gen_interrupt_impl(void* self, u64 dsp_int_type, s64 cycles_late);
// DSPManager::UpdateAudioDMA — AID claim seam (aid_native.cpp).
extern void (*sb_slot_dsp_update_audio_dma)(void* self);
void sb_dsp_update_audio_dma_impl(void* self);
// DSPManager::GenerateDSPInterruptFromDSPEmu — DSP-mail interrupt capture (aid_native.cpp).
extern void (*sb_slot_dsp_gen_interrupt_from_emu)(void* self, int type, int cycles_into_future);
void sb_dsp_gen_interrupt_from_emu_impl(void* self, int type, int cycles_into_future);

// ── CoreTiming (Core) ────────────────────────────────────────────────────────────────────────
// CoreTimingManager::ScheduleEvent/RemoveEvent — memcard EXI trace (coretiming_trace.cpp).
extern void (*sb_slot_ct_schedule_event)(void* self, s64 cycles, void* event_type, u64 userdata,
                                         int from);
void sb_ct_schedule_event_impl(void* self, s64 cycles, void* event_type, u64 userdata, int from);
extern void (*sb_slot_ct_remove_event)(void* self, void* event_type);
void sb_ct_remove_event_impl(void* self, void* event_type);

// ── Mixer (AudioCommon) ──────────────────────────────────────────────────────────────────────
// Mixer::PushSamples / Mix / PushStreamingSamples / Set*RateDivisor / SetStreamingVolume
// — native audio sink feed + flow meter (mixer_trace.cpp).
extern void (*sb_slot_mixer_push_samples)(void* self, const s16* samples, std::size_t n);
void sb_mixer_push_samples_impl(void* self, const s16* samples, std::size_t n);
extern std::size_t (*sb_slot_mixer_mix)(void* self, s16* samples, std::size_t n);
std::size_t sb_mixer_mix_impl(void* self, s16* samples, std::size_t n);
extern void (*sb_slot_mixer_push_streaming)(void* self, const s16* samples, std::size_t n);
void sb_mixer_push_streaming_impl(void* self, const s16* samples, std::size_t n);
extern void (*sb_slot_mixer_set_dma_divisor)(void* self, u32 divisor);
void sb_mixer_set_dma_divisor_impl(void* self, u32 divisor);
extern void (*sb_slot_mixer_set_stream_divisor)(void* self, u32 divisor);
void sb_mixer_set_stream_divisor_impl(void* self, u32 divisor);
extern void (*sb_slot_mixer_set_streaming_volume)(void* self, u32 lvolume, u32 rvolume);
void sb_mixer_set_streaming_volume_impl(void* self, u32 lvolume, u32 rvolume);

// ── DSPHLE UCodes (Core) ─────────────────────────────────────────────────────────────────────
// UCodeFactory — native SMS DAC ucode selection (zelda_ucode_native.cpp). The hook returns
// non-null to override; null return means "fall through to the original factory".
// Returns a raw UCodeInterface* (ownership transferred to the caller's unique_ptr); a null
// hook pointer OR a null return = use Dolphin's UCodeFactory.
extern void* (*sb_slot_ucode_factory)(u32 crc, void* dsphle, bool wii);

// ZeldaAudioRenderer::FetchVPB — voice-parameter tracer (vpb_trace.cpp). Called AFTER the
// original body has filled *vpb (the tracer only reads). self = ZeldaAudioRenderer*.
extern void (*sb_slot_zelda_fetch_vpb)(void* self, u16 voice_id, void* vpb);

}  // extern "C"

// Install all hooks. Defined in runtime/sunbright_hooks.cpp; called once at runtime startup.
void sb_install_hooks();
