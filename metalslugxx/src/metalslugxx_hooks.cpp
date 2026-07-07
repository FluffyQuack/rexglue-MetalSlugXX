// metalslugxx - ReXGlue Recompiled Project
//
// Mid-ASM hooks and function-level fixups for places the static lift could not
// fully resolve (the GoldenEye `ge_hooks.cpp` analogue), plus deliberate game
// patches. See include/rex/hook.h for the authoring API.

#include <rex/hook.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/logging/api.h>
#include <rex/logging/macros.h>

#include "metalslugxx_settings.h"

namespace {

bool g_playfield_scene_filter_scope = false;

int MsxxRequestedPlayfieldFilter() {
  const std::string& filter = metalslugxx::config().game_upscale_filter;
  if (filter == "point") {
    return 0;
  }
  if (filter == "linear") {
    return 1;
  }
  return -1;
}

void MsxxApplyRequestedPlayfieldFilter(PPCRegister& filter_reg) {
  const int filter = MsxxRequestedPlayfieldFilter();
  if (filter < 0) {
    return;
  }
  filter_reg.u32 = static_cast<uint32_t>(filter);
}

}  // namespace

// ---------------------------------------------------------------------------
// [Game] SkipLogos — skip the boot logo sequence.
//
// sub_82571500 (ms_boot_logo_sequence) is the opening scene step that, after
// some setup, plays three logos back to back, each as "load the texture, then a
// `while (tick() == 996 /*busy*/)` fade-in/hold/fade-out wait loop":
//   * XBLA "Arcade" logo  (ms_load_arcade_logo  0x8256D288, tick 0x8256F420)
//   * ESRB logo, US only  (ms_load_esrb_logo    0x8256D438, tick 0x8256F700)
//   * SNK Playmore logo   (ms_load_vendor_logo  0x8256D490, tick 0x8256FA20)
// then runs teardown/transition code (0x82571700..) that sets up the next scene.
//
// We do NOT jump over the whole display region: each logo's load and its first
// tick initialize draw state that the later loading-screen/title transition
// reuses (jumping straight to 0x82571700 crashed with a null-deref in the
// "Loading…" render). Instead, three [[midasm_hook]]s (metalslugxx_config.toml)
// sit on the loop-back `beq` after each tick; when this returns true the
// recompiler jumps just past that loop, so each logo runs its load + exactly one
// tick (alpha≈0 at t≈0, so it's effectively invisible, and all per-logo init
// runs) and then bails out of the multi-second fade wait. When false the logos
// play exactly as on hardware (the hook is a cheap bool read per frame).
//
// The codegen emits `extern bool msxx_skip_boot_logos();` in the generated TU,
// so this must be a global-scope (non-namespaced, non-extern-"C") definition
// whose name matches the hooks' `name`.
bool msxx_skip_boot_logos() {
  return metalslugxx::config().skip_logos;
}

// ---------------------------------------------------------------------------
// [Graphics] Scoped playfield filtering.
//
// GameUpscaleFilter used to drive the SDK's global sampler override, which made
// menus and other non-playfield UI inherit point filtering. Keep the override in
// the places where the game builds/presents the 320x240 playfield: the scene
// render into the native target, and the two playfield upscale quads.
void msxx_begin_playfield_scene_filter_scope() {
  if (MsxxRequestedPlayfieldFilter() >= 0) {
    g_playfield_scene_filter_scope = true;
  }
}

void msxx_end_playfield_scene_filter_scope() {
  g_playfield_scene_filter_scope = false;
}

void msxx_force_playfield_scene_sampler_filter(PPCRegister& filter) {
  if (!g_playfield_scene_filter_scope) {
    return;
  }
  MsxxApplyRequestedPlayfieldFilter(filter);
}

void msxx_force_playfield_upscale_sampler_filter(PPCRegister& filter) {
  MsxxApplyRequestedPlayfieldFilter(filter);
}

// ---------------------------------------------------------------------------
// [Game] Integer playfield scale.
//
// ms_present_playfield_upscale builds the final quad for the 640x480
// intermediate playfield texture. Stock code letterboxes the original 320x240
// content to 864x648 inside the 1280x720 frame (2.7x from the original pixel
// buffer). When the user requests point filtering, force the quad to a centered
// 960x720 rectangle so the combined 320x240 -> screen scale is exactly 3.0x.
void msxx_force_integer_playfield_scale(PPCRegister& left, PPCRegister& top,
                                        PPCRegister& right, PPCRegister& bottom) {
  if (metalslugxx::config().game_upscale_filter != "point") {
    return;
  }

  left.f64 = 160.0;
  top.f64 = 0.0;
  right.f64 = 1120.0;
  bottom.f64 = 720.0;
}

// ===========================================================================
// 60fps interpolation — STEP A: capture/replay "dry-run" (measurement only)
// ===========================================================================
//
// Gated behind [Game] FpsReplayDryRun (off by default). This stands up the
// capture/replay plumbing the eventual off-pass interpolator needs, but with
// ZERO interpolation math — positions are replayed verbatim (phase 0). With
// phase 0 the off-pass frame must be pixel-identical to the pass-0 frame, which
// proves the capture is complete (every sprite AND the HUD survives the rebuild)
// and that a clear+replay produces a flushable list. See
// MSXX_60FPS_FUNCTION_TARGETS.md (NEXT STEP, task A) for the full rationale.
//
// The pass model (decoded in the MD): ms_update_and_render_frame runs the frame
// in [0x82D29BA2] (=2) passes. The scene/render list is built once, on pass 0
// (do_scene = (pass==0 ? 255 : 0)); pass >=1 normally just re-presents pass 0's
// framebuffer. Here, on the off-pass we instead clear the list, replay the
// captured producer calls, and force do_scene so the rebuilt list is flushed.
//
// Threading: the producer walk runs on the cooperatively-scheduled render-task
// fiber, which shares the OS thread with ms_update_and_render_frame, so all of
// the below is effectively single-threaded — no locking needed.

namespace {

constexpr size_t kMaxCapturedEntryVisuals = 16;

// One captured render-list producer call, recorded in call order so the replay
// reproduces the exact interleaving the build used (insertion order matters
// within a layer bucket).
struct CapturedOp {
  enum class Kind : uint8_t { Entry, Seal, ResolvedSprite } kind;
  // ResolvedSprite: the off-pass sprite op. Captured at the object-free ms_render_list_insert_sprite
  //            chokepoint (not at a producer), so the off-pass never touches volatile object memory (the
  //            despawn/reuse crash class dissolves — plan §2/§9). a[0..6] carry the baked insert_sprite
  //            args: a[0]=sortkey(r3), a[1]=record(r4), a[2]=screenX(r5, low16=int16), a[3]=screenY(r6),
  //            a[4]=xsub(r7), a[5]=ysub(r8), a[6]=flag9|flag10<<8. resolvedLerpDX/DY + hudFixed drive the
  //            screen-space lerp; submitState reproduces the ambient alpha/anchor.
  // Entry    : a[0..6] = ms_render_list_add_entry(r3..r9), replayed verbatim (no lerp)
  // Seal     : a[0]    = ms_render_list_sort_seal_batch(r3) — sorts the pending sprite
  //         batch into the flush array; producers call it between layer groups, so it
  //         must be replayed in-order or the sprite counters (0x5400/0x5402) corrupt.
  uint32_t a[7];

  // Ambient render-submit state read by ms_render_list_insert_sprite, but NOT encoded in the
  // add_sprite / anim-stream arguments. The real object walk primes these immediately before each
  // handler (and handlers can clear them before individual emits); replaying only the final producer
  // call otherwise enters insert_sprite with stale/default alpha/anchor inputs. Snapshot at producer
  // entry and restore around the replayed emit so the regenerated list carries the same visual params
  // the real walk baked.
  bool submitStateValid = false;
  uint32_t alphaScaleGate = 0;  // 0x82DAA628
  uint32_t anchorEnable = 0;    // 0x82DAA620
  uint16_t anchorParam = 0;     // 0x82DCEA54
  uint16_t anchorScreenX = 0;   // 0x82DCEA56
  uint32_t anchorObj = 0;       // 0x82DCEA58
  uint16_t anchorScreenY = 0;   // 0x82DCEA5C

  // Final render-list visual state captured at ms_render_list_add_entry. r7 points at an
  // already-baked 0x68 entry block, r5 is the block count, entry+0x1c carries each entry's object
  // alpha bits, and r6 becomes the layer alpha byte. Patching these values back on replay covers
  // short-lived fade sources that are gone by the time the off-pass regenerates the block.
  uint8_t entryVisualCount = 0;
  uint32_t entryPackedParams[kMaxCapturedEntryVisuals] = {};
  uint8_t entryLayerAlpha[kMaxCapturedEntryVisuals] = {};

  // Camera-anchored HUD flag (the bottom-left weapon-selection indicator). True for ResolvedSprite ops
  // captured while inside a weapon-HUD producer — the icon strip ms_hud_weapon_select_draw_icon_strip
  // (0x82228B18, caps 139/141 + weapon icons) or the slot-range selection draw
  // ms_hud_weapon_slot_range_update_draw (0x82229BA8) — see g_in_weapon_hud_producer. Both pass
  // xoff = cameraX + const, yoff = cameraY + const, so the camera is ALREADY cancelled inside the baked
  // screenX (r5) captured at insert_sprite. hudFixed therefore just forces the screen-space lerp delta to
  // 0 (replay the baked screen position verbatim) so the indicator stays put. See CaptureResolvedSprite.
  bool hudFixed = false;

  // ResolvedSprite ops only. Per-emit SCREEN-space motion delta in px (cur - prev in screen coords), the
  // motion the off-pass lerps back toward the previous tick. Unlike the producer path (which lerps the pose
  // and re-runs the transform), the resolved path lerps the already-baked screenX/Y directly by this delta,
  // so it must be expressed in SCREEN space: screenX = worldX - camX (DX = world cur-prev) but screenY =
  // camY - worldY (DY = world PREV-cur, i.e. inverted) — see CaptureResolvedSprite. Getting DY's sign wrong
  // makes vertical motion interpolate backward. hudFixed ops force 0 (camera-anchored -> screen-fixed).
  int32_t resolvedLerpDX = 0, resolvedLerpDY = 0;

  // ResolvedSprite ops only. A verbatim snapshot of the CALLER-FRAME input window the recompiled
  // insert_sprite reads back from its caller's stack. The producer stashes ten values there just before
  // `bl insert_sprite` (recomp.2.cpp:19180-19198: caller_sp + 0x57/0x5F/0x66/0x6E/0x77/0x7F/0x87/0x8E/
  // 0x97/0x9C), and insert_sprite reloads them on its draw path (0.cpp:38450/38491/38499/38507 + the quad
  // build at :39000+). They carry, among others:
  //   +0x97 = [obj+0x2B] ALPHA/gate byte — MUST be nonzero or insert_sprite branches to its epilogue
  //           (loc_82203124) and inserts NOTHING (the log-303 blank-off-frame bug: Gate F misread that
  //           early-out as a skippable "cosmetic rescale"). Also packed into the entry alpha (:38612).
  //   +0x66/+0x6E = [obj+0x20]/[obj+0x22] scale halves — also must be nonzero or it early-outs (:38497/38505).
  //   +0x9C = obj back-ptr, used ONLY for the mode read [[obj+0x58]+0x0C] (:38508) — a shallow, always-
  //           mapped static-pool read (NOT the wild [obj+0x38] lwzx that crashed the producer re-run), so
  //           re-issuing it on a possibly-despawned obj can't fault (worst case a stale mode, still identity).
  //   the H-FLIP / draw flags (facing direction etc.) live in the +0x77/+0x7F/+0x87/+0x8E slots — capturing
  //           the WHOLE window (not just alpha/scale/obj) is why the off-pass mirrors correctly.
  // Snapshotting the entire [kFrameWinLo, kFrameWinLo+kFrameWinLen) byte window (producer-agnostic,
  // endian-safe byte copy) and writing it back into the replay frame reproduces every flag byte-exactly, so
  // insert_sprite runs its real draw path. Bytes insert_sprite doesn't read are harmless; the ones it
  // self-initializes early (its own r3/r9/r10 arg stores at +0x16/0x47/0x4F) sit BELOW this window.
  bool frameValid = false;
  static constexpr uint32_t kFrameWinLo = 0x50;   // covers the lowest producer stash (+0x57)
  static constexpr uint32_t kFrameWinLen = 0x50;  // through +0x9C+4 (the obj u32) = [0x50,0xA0)
  uint8_t frameWin[kFrameWinLen] = {};
};

// Double-buffered capture. The producer walk that builds a frame's list runs
// (on the render fiber, pumped by ms_render_frame) and fills `g_capture_accum`,
// but it does so in the window that ENDS at the next frame's begin_frame -- i.e.
// the walk that built the list currently on screen finishes BEFORE begin_frame,
// and the off-pass that should replay it runs AFTER begin_frame. So begin_frame
// PROMOTES accum -> ready (instead of discarding); the off-pass replays `ready`,
// which is exactly the set that built the displayed frame. (Diagnosed 2026-06-24
// from the empty-capture log: captures happened, tid was single-threaded, yet the
// vector was empty at the off-pass -> begin_frame was wiping them post-walk.)
std::vector<CapturedOp> g_capture_accum;
std::vector<CapturedOp> g_capture_ready;

// --- "Resolved sprite" off-pass seam (MSXX_60FPS_RESOLVED_SPRITE_REFACTOR_PLAN.md) ----------------
// The off-pass capture/replay seam lives at the object-free ms_render_list_insert_sprite chokepoint,
// NOT at the two producers (add_sprite / anim-stream emit_sprite). The producer entry hooks only open
// the suppression bracket + set g_emit_obj; CaptureResolvedSprite (insert_sprite ENTRY) pushes a
// Kind::ResolvedSprite op into g_capture_accum, interleaved in call order with the unchanged Entry/Seal
// ops. Every arg on a ResolvedSprite op is already resolved (record ptr + baked screen XY + ambient
// submit state), so the replay (msxx_fps_replay_resolved) re-issues insert_sprite directly and never
// touches volatile object memory: the despawn/reuse crash class (log 293) is gone by construction, and a
// producer emit the engine internally skipped simply never reaches insert_sprite so it is never captured
// (the old issue-#3 off-frame skip dissolves). Gates B/C/F all passed (logs 295-297) and the switch was
// validated on real gameplay (log 301) before the classic producer-replay path was deleted (step 5); the
// steps-1/2 parallel diagnostic stream is retired, replaced by the compact Gate-F "does the rescale ever
// MOVE a sprite" tripwire folded into CaptureResolvedSprite.
//
// insert_sprite reloads FOUR values from its caller's frame (the producer stashes them just before the
// `bl`, recomp.2.cpp:19180-19198): the gate/alpha byte caller_sp+0x97 (= [obj+0x2B]), the scale halves
// +0x66/+0x6E (= [obj+0x20]/[obj+0x22]), and the obj back-ptr +0x9C. If the gate byte OR either scale half
// is 0 it branches to loc_82203124 — its EPILOGUE — and inserts NOTHING (recomp 0.cpp:38466-38505). The
// gated block (0.cpp:38470+) is the real draw path; it rereads the obj from +0x9C only to fetch the mode
// [[obj+0x58]+0x0C] for a rescale that Gate F proved is a 320/240 VALUE identity. Earlier this seam FORCED
// caller_sp+0x97=0 believing that "skipped a cosmetic rescale" — it actually early-outed the whole draw
// (blank off-frames, log 303). The replay now WRITES BACK the four captured caller-frame values (op.frame*,
// see msxx_fps_call_insert_sprite) so insert_sprite runs its real, byte-exact draw path. The tripwire below
// warns if any scene's rescale would actually reposition — that scene would need the rescale reproduced.
constexpr uint32_t kRescaleDimX = 0x82DCEA4Cu;  // insert_sprite rescale: screenX divisor-pair (vs 320)
constexpr uint32_t kRescaleDimY = 0x82DCEA4Eu;  // insert_sprite rescale: screenY divisor-pair (vs 240)
int g_gatef_warn_budget = 40;  // Gate-F tripwire: budgeted warnings if the rescale ever moves a sprite

// True only while we are re-issuing captured calls, so the capture hooks ignore
// the calls the replay itself makes (they would otherwise re-append/recurse).
bool g_replaying = false;

// True while inside ms_render_list_add_sprite (between its entry hook 0x82236BB8
// and its single converged exit 0x82236D1C). add_sprite calls insert_sprite,
// which funnels into the low-level append ms_render_list_add_entry (0x821FC4E8) --
// those nested appends must NOT be captured as standalone Entry ops (the Sprite
// op regenerates them on replay). Only top-level (UI/glyph) add_entry calls, made
// when this is false, are captured. See the add_sprite/add_entry hooks below.
bool g_in_sprite = false;

// Active producer <-> nested add_entry association. During capture, nested add_entry calls record
// final baked alpha state into this producer op. During replay, the same hook patches the regenerated
// entries from the op's captured sequence.
size_t g_active_producer_capture_idx = SIZE_MAX;
const CapturedOp* g_replay_entry_visual_op = nullptr;
uint8_t g_replay_entry_visual_idx = 0;

// True while inside a weapon-HUD sprite producer — the icon strip (0x82228B18) or the slot-range
// selection draw (0x82229BA8) — opened/closed by those functions' entry/exit bracket hooks. add_sprite
// ops captured in this window are the camera-anchored weapon indicator and get tagged
// CapturedOp::hudFixed. (Type-based tagging via obj+0x40 was tried and rejected: the log proved the
// widget is type 45, but 45 is reused by world objects too — see git history — so a type match would
// wrongly pin world sprites. Function identity is the reliable classifier.) Single-threaded (render
// fiber); reset defensively at begin_frame.
bool g_in_weapon_hud_producer = false;

// The object whose sprite is currently being emitted, stashed by the producer capture hooks so the
// insert_sprite entry hook can read its live pose delta for the ResolvedSprite capture (and for the #4
// position probe). Set during the REAL walk only; the off-pass replay is object-free.
uint32_t g_emit_obj = 0;

// Interpolation phase numerator/denominator: phase = g_pass / g_pass_count (pass 0
// -> 0.0 = prev tick; pass 1 of 2 -> 0.5 = midpoint). Set by the off-pass driver
// (msxx_fps_render_frame_driver, which runs at 1/2) and read by the replay wrapper.
uint32_t g_pass = 0;
uint32_t g_pass_count = 2;

// Present index within the current 30Hz tick, reset at begin_frame, bumped at each
// ms_render_list_flush entry. Diagnostic only (the flush-cadence probe); index 0 =
// the first present of the tick, index 1 = the second.
uint32_t g_flush_index = 0;

// FpsReplayDryRun = capture/replay at phase 0 (off-pass must be pixel-identical;
// validates the plumbing). FpsInterpolate = the real thing (off-pass re-emitted
// at the inter-tick midpoint). Either one arms the capture/replay machinery.
bool FpsInterpEnabled() { return metalslugxx::config().fps_interpolate; }
bool FpsActive() { return metalslugxx::config().fps_replay_dryrun || FpsInterpEnabled(); }

// --- Diagnostics (temporary, budgeted log counters) ------------------------
int g_renderframe_drv_dbg = 40;  // budget for the in-context ms_render_frame off-pass driver
int g_capture_class_dbg = 30;    // budget: classify captured ops (sprite-with-pose vs nopose vs entry)

uint64_t ThisTid() {
  return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

// --- Fiber-path probe (env MSXX_FPS_PROBE=1, 2026-06-25) --------------------
// Log 189 proved the pass-loop off-pass driver only runs in a brief boot window,
// while gameplay renders on the render-task fiber (no driver). KEY unknown before
// relocating the driver: during gameplay, do the producer hooks fire on the fiber,
// and does the producer walk run every PRESENT or once per 30Hz TICK? These
// throttled counters span the whole run (boot + gameplay) so the post-boot rate
// answers it: add_sprite per begin_frame ~constant + begin_frame ~= presents =>
// producer runs per present (lerp-in-place works); else baked-list reuse =>
// capture/replay must move to the fiber. tid identifies the thread (fiber vs pass).
bool FpsProbeEnabled() {
  static const bool e = [] {
    const char* s = std::getenv("MSXX_FPS_PROBE");
    return s && s[0] && s[0] != '0';
  }();
  return e;
}
uint64_t g_probe_add_sprite = 0;
uint64_t g_probe_add_entry = 0;
uint64_t g_probe_begin_frame = 0;
uint64_t g_probe_flush = 0;
uint64_t g_probe_upscale = 0;

// Per-present (60Hz) upscale index within the tick, reset at begin_frame, bumped
// at each ms_upscale_playfield_to_640x480 (0x823E6DF0) entry. Log 187's resolve
// cadence says the upscale chain runs 2x/tick (it is the real per-present seam,
// unlike the 30Hz flush); this probe confirms that on the fiber and tells us
// which call (index 0 = real present, 1 = off-pass) carries the 2nd present.
uint32_t g_upscale_index = 0;

// --- ms_render_frame IN-CONTEXT off-pass driver -----------------------------------------
// The chosen direction (MSXX_60FPS_OFFPASS_FEED_OPTIONS.md DECISION, 2026-06-25). Every prior
// attempt drove the lerped re-render from the WRONG place — the 0x823E6DF0 upscale hook, which
// runs MID-UPSCALE: there setup_playfield_target binds the scene surface to a DIFFERENT EDRAM
// tile than the real scene (the host RT cache maps it against the upscale's register state), so
// ms_scene_render scribbles into the upscale's RT -> corruption -> black (log 205).
//
// Relocate the re-render INTO ms_render_frame's own scene context: a new hook at 0x823f9cbc,
// right after the real do-scene trio (setup 0x823E6F60 -> scene_render 0x82250390 -> finish
// 0x823E7018) returns and BEFORE the frame-counter bump, while the device is still in scene-
// render state. There setup_playfield_target rebinds the 320x240 scene RT CORRECTLY (same tile
// as the real render). The lerped EDRAM is then carried to the off-pass present by the guest-side
// buffer-B redirect (see EnsureBufferBTexture + msxx_fps_redirect_scene_source), leaving the
// real present untouched. This path is armed by [Game] FpsInterpolate (or FpsReplayDryRun for
// validation), with no extra env gate required to make the 60fps mod visible.

// --- Guest-side source redirect -----------------------------------------------------------
// The pivot away from the dead host buffer-B path (MSXX_60FPS_GUEST_REDIRECT_PLAN.md). Instead of
// matching the off-pass source by physical base (defeated by the allocator reusing the scene address
// within a tick -> logs 209/210/214 garbage), we clone a real game texture object B from the scene
// texture state+0x14, fill it via the engine's own finish-resolve (a temporary [state+0x14]=B swap),
// and on the off-pass present rewrite the SetTexture source (r5 @ 0x823E6DA8) -> B. The object carries
// its own correct dims/pitch/format, so there is no wrong-stride/alias problem. B_tex is created lazily
// (EnsureBufferBTexture), filled in the render-frame driver, and consumed by the source redirect.

// --- #4 verify tool: insert_sprite final-position probe (env MSXX_FPS_ISPROBE=1) -----------------
// Logs the FINAL baked sprite position (post per-part offset) at the insert_sprite chokepoint for both
// the engine's real walk (CAP) and the off-pass replay (REP), tagged by object + pose. This is what
// proved the player torso/legs collapse and then confirmed the per-emit pose-capture fix (REP torso
// curY/rawSY now tracks CAP, e.g. 269/99, instead of the legs' 244/124). Pure measurement, budgeted,
// sampled in short bursts. See msxx_fps_probe_insert_sprite and MSXX_60FPS_GUEST_REDIRECT_PLAN.md #4.
bool FpsIsProbeEnabled() {
  static const bool e = [] {
    const char* s = std::getenv("MSXX_FPS_ISPROBE");
    return s && s[0] && s[0] != '0';
  }();
  return e;
}
int g_isprobe_remaining = 8000;     // budget for the insert_sprite final-position probe (gameplay window)

// Playfield upscale state block (g_playfield_upscale_state). Base = lis -32064 (0x82C00000) + addi
// -19964 in ms_present_playfield_upscale/ms_finish (823e7324/2c) => 0x82BFB204 (the config comment's
// "0x82C0B204" is wrong). +0x14 scene tex, +0x1C 640x480 inter, +0x2C source WIDTH (320), +0x30
// source HEIGHT (240) — passed straight to ms_d3d_create_texture2d as r3/r4 (the +0x1C path passes
// literal 640/480). See the disasm map in MSXX_60FPS_GUEST_REDIRECT_PLAN.md.
constexpr uint32_t kPlayfieldState = 0x82BFB204u;
constexpr uint32_t kRenderDevice = 0x82D41304u;
constexpr uint32_t kSetAlphaBlendEnableFn = 0x82436A88u;

// Guest handle of the cloned scene texture that serves as off-pass buffer B (0 = not yet created).
uint32_t g_fps_b_tex = 0;

// --- Issue #5: background (PAGE pass) interpolation -------------------------------------------------
// The stepping backdrop is drawn by the page-entry queue flushes (0x821F9038/9230/90F8 -> per-record
// ms_render_draw_sprite_page_entry 0x82206E68), confirmed by the bgprobe (MSXX_FPS_BGPROBE_SKIP=page
// flickered the backdrop; tile=HUD, effect=transparency). scene_render reads the world-scroll globals
// ONLY for culling — the backdrop is UV-scrolled fixed quads, so the scroll lives in each page record's
// texture UVs (geometry static), not in any global. The off-pass driver re-runs the WHOLE scene_render,
// so it redraws each page record at the CURRENT tick's baked UVs -> steppy backdrop while sprites lerp.
//
// FIX (two coupled parts, both below): (1) snapshot each page record's UV+geometry fields per tick keyed
// by record address and draw the off-pass at lerp(prev,cur,t) — see msxx_fps_bg_shift_page_entry; and
// (2) interpolate the int16 camera the sprite producers project against so sprites share the backdrop's
// midpoint — see the camera block in msxx_fps_render_frame_driver. (An earlier global-scroll-shift
// approach was abandoned: there is no single position global to override.)

// --- Issue #5 DIAGNOSTIC (env MSXX_FPS_BG_DIAG=1) -------------------------------------------------
// Logs each page record's per-tick prev->cur field delta (px) alongside the int16 camera (cam7450 X /
// cam7454 Y). Confirms the lerp input (a stable slot shows a small smooth UV delta; a reassigned slot
// shows a huge jump) and lets a vertical-scroll run compare the bg's Y delta against cameraY's.
bool BgDiagEnabled() {
  static const bool e = [] {
    const char* s = std::getenv("MSXX_FPS_BG_DIAG");
    return s && s[0] && s[0] != '0';
  }();
  return e;
}
int g_bg_tick = 0;          // per-tick counter (bumped at begin_frame)
int g_bg_diag_rec_idx = 0;  // per-tick page-record index (reset at begin_frame)
int g_bg_diag_budget = 600; // total fps-bgdiag line budget

// The 8 interpolatable page-record fields (raw 32-bit float bits): 4 texture-UV floats (+0x00/04/30/34)
// + 4 geometry floats (+0x0C/10/3C/40). DIAG (logs 256/257) showed the background is UV-scrolled fixed
// quads — the UVs slide smoothly with the camera while the geometry is static — so the UVs carry the
// scroll; the geometry is lerped too but is a harmless no-op when static. All 8 are floats in the draw's
// 4096/px space. Indices [0..3]=UV, [4..7]=XY.
constexpr uint32_t kPageFieldOff[8] = {0x00u, 0x04u, 0x30u, 0x34u, 0x0Cu, 0x10u, 0x3Cu, 0x40u};

// Per-slot (record address) snapshot for the off-pass lerp. The page queues rebuild a STABLE array of
// records each tick (same address = same screen cell, verified: addrs constant across 540 ticks while
// the UVs scroll), so we can treat addr as identity: prev = last tick's fields, cur = this tick's. On
// the off-pass we draw each record at lerp(prev,cur,t) — a direct measurement of the real motion, so it
// needs no scroll global and auto-handles per-row parallax. A snap guard covers texture wraps / scene
// cuts (a slot whose field jumps too far this tick is drawn at cur, no lerp, for one frame).
struct PageSlot {
  bool seeded = false;
  uint32_t prev[8] = {};
  uint32_t cur[8] = {};
};
std::unordered_map<uint32_t, PageSlot> g_page_slots;

// Off-pass restore slot. ms_render_draw_sprite_page_entry does not recurse, so one suffices: the entry
// hook writes the lerped fields into the live record and stashes the originals (cur) here; the exit hook
// restores them so the engine's queue memory is pristine for the next tick's real render.
struct PageShiftSave {
  bool active = false;
  uint32_t rec = 0;
  uint32_t cur[8] = {};
};
PageShiftSave g_page_shift;

// Background-interp diagnostic parameters (cached once). Interpolation and camera sync themselves
// are controlled by [Game] FpsInterpolate. MSXX_FPS_BG_SNAPPX=N sets the per-tick snap threshold
// (px; a record field that jumps more than this is drawn at cur, no lerp — covers texture wraps /
// scene cuts); MSXX_FPS_BG_DEBUGPX=N forces a constant N-px geometry X shift on the off-pass
// (ignoring the lerp) to verify the field offsets are visible.
struct BgInterpCfg {
  int debug_px;   // forced constant geometry X shift in px (0 = real per-slot lerp)
  float snap_px;  // per-tick snap threshold in px (default 48)
};
const BgInterpCfg& BgCfg() {
  static const BgInterpCfg c = [] {
    BgInterpCfg v;
    const char* d = std::getenv("MSXX_FPS_BG_DEBUGPX");
    v.debug_px = d ? std::atoi(d) : 0;
    const char* sp = std::getenv("MSXX_FPS_BG_SNAPPX");
    v.snap_px = sp ? static_cast<float>(std::atof(sp)) : 48.0f;
    return v;
  }();
  return c;
}

// Off-pass camera interpolation (issue #5 sprite/background sync). Both sprite producers (add_sprite
// @0x82236C28, emit_sprite @0x82238B28) subtract the int16 camera at 0x82D74750/4754 (the integer-pixel
// high half of the 16.16 renderer camera) when projecting pose->screen. On the off-pass the backdrop
// sits at camera_mid (its UV is lerped) but the sprites would still use camera_N, so they slide half a
// step relative to the backdrop. The driver temporarily sets this int16 camera to the inter-tick
// midpoint around the replay so the sprites project with the same camera the backdrop is drawn at.
constexpr uint32_t kCameraX = 0x82D74750u;
constexpr uint32_t kCameraY = 0x82D74754u;
int g_cam_prev_x = 0, g_cam_prev_y = 0;  // previous tick's full 16.16 camera
bool g_cam_seeded = false;
int g_cam_save_x = 0, g_cam_save_y = 0;  // this tick's cur camera, restored after the off-pass
bool g_cam_applied = false;              // true between apply and restore

// Render-submit globals consumed inside ms_render_list_insert_sprite. They are scoped by the object
// draw dispatcher / handler, not passed as producer args, so captured producers must carry them too.
constexpr uint32_t kSubmitAnchorEnable = 0x82DAA620u;
constexpr uint32_t kSubmitAlphaScaleGate = 0x82DAA628u;
constexpr uint32_t kSubmitAnchorParam = 0x82DCEA54u;
constexpr uint32_t kSubmitAnchorScreenX = 0x82DCEA56u;
constexpr uint32_t kSubmitAnchorObj = 0x82DCEA58u;
constexpr uint32_t kSubmitAnchorScreenY = 0x82DCEA5Cu;

// Big-endian guest memory access for the interpolation lerp (pose fields are
// 32-bit 16.16, stored big-endian). Returns 0 / no-op if the guest isn't mapped.
uint8_t* GuestBase() {
  auto* ks = rex::system::kernel_state();
  return (ks && ks->memory()) ? ks->memory()->virtual_membase() : nullptr;
}
uint32_t GuestR32(uint32_t addr) {
  uint8_t* base = GuestBase();
  if (!base || !addr) return 0;
  const uint8_t* p = base + addr;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void GuestW32(uint32_t addr, uint32_t v) {
  uint8_t* base = GuestBase();
  if (!base || !addr) return;
  uint8_t* p = base + addr;
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
uint8_t GuestR8(uint32_t addr) {
  uint8_t* base = GuestBase();
  if (!base || !addr) return 0;
  return base[addr];
}
void GuestW8(uint32_t addr, uint8_t v) {
  uint8_t* base = GuestBase();
  if (!base || !addr) return;
  base[addr] = v;
}
// Big-endian 16-bit read. Pose XY (pose+0x18/0x1c, prev +0x20/0x24) are 16.16 fixed; both producers
// take only the integer part via `lhz` of the HIGH halfword, so the screen transform uses exactly this.
int16_t GuestR16(uint32_t addr) {
  uint8_t* base = GuestBase();
  if (!base || !addr) return 0;
  const uint8_t* p = base + addr;
  return static_cast<int16_t>((uint16_t(p[0]) << 8) | p[1]);
}
void GuestW16(uint32_t addr, uint16_t v) {
  uint8_t* base = GuestBase();
  if (!base || !addr) return;
  uint8_t* p = base + addr;
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}

constexpr uint32_t kObjHandler = 0x08u;
constexpr uint32_t kIdleMovePromptRightState = 0x82CF6664u;
constexpr uint32_t kIdleMovePromptLeftState = 0x82CF6680u;
constexpr uint32_t kIdleMovePromptRightBeginDelay = 0x82221440u;
constexpr uint32_t kIdleMovePromptLeftBeginDelay = 0x82227268u;
bool g_go_popup_skip_logged = false;

void LogGoPopupSkipOnce() {
  if (g_go_popup_skip_logged) return;
  g_go_popup_skip_logged = true;
  REXLOG_INFO("go-popups: DisableGoPopups=True; suppressing idle GO prompts");
}

void DisableIdleMovePromptGlobals() {
  GuestW16(kIdleMovePromptRightState, 0);
  GuestW16(kIdleMovePromptLeftState, 0);
}

bool GoPopupsDisabled() {
  if (!metalslugxx::config().disable_go_popups) return false;
  DisableIdleMovePromptGlobals();
  LogGoPopupSkipOnce();
  return true;
}

bool DisableIdleMovePromptIfConfigured(uint32_t obj, uint32_t beginDelayHandler) {
  if (!GoPopupsDisabled()) return false;
  if (obj) GuestW32(obj + kObjHandler, beginDelayHandler);
  return true;
}

void CaptureSubmitState(CapturedOp& op) {
  op.submitStateValid = true;
  op.alphaScaleGate = GuestR32(kSubmitAlphaScaleGate);
  op.anchorEnable = GuestR32(kSubmitAnchorEnable);
  op.anchorParam = static_cast<uint16_t>(GuestR16(kSubmitAnchorParam));
  op.anchorScreenX = static_cast<uint16_t>(GuestR16(kSubmitAnchorScreenX));
  op.anchorObj = GuestR32(kSubmitAnchorObj);
  op.anchorScreenY = static_cast<uint16_t>(GuestR16(kSubmitAnchorScreenY));
}

void CaptureEntryVisualState(CapturedOp& op, uint32_t entry, uint32_t entryCount,
                             uint32_t layerAlpha) {
  if (!entry || op.entryVisualCount >= kMaxCapturedEntryVisuals) return;
  const uint32_t count = std::min<uint32_t>(entryCount & 0xFFu,
                                           kMaxCapturedEntryVisuals - op.entryVisualCount);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t idx = op.entryVisualCount++;
    op.entryPackedParams[idx] = GuestR32(entry + i * 0x68u + 0x1Cu);
    op.entryLayerAlpha[idx] = static_cast<uint8_t>(layerAlpha);
  }
}

void CaptureActiveProducerEntryVisualState(uint32_t entry, uint32_t entryCount,
                                           uint32_t layerAlpha) {
  if (g_active_producer_capture_idx >= g_capture_accum.size()) return;
  CaptureEntryVisualState(g_capture_accum[g_active_producer_capture_idx], entry, entryCount,
                          layerAlpha);
}

void ApplyReplayEntryVisualState(PPCRegister& entryCount, PPCRegister& layerAlpha,
                                 PPCRegister& entry) {
  const CapturedOp* op = g_replay_entry_visual_op;
  if (!op || !entry.u32 || g_replay_entry_visual_idx >= op->entryVisualCount) return;
  const uint8_t firstIdx = g_replay_entry_visual_idx;
  const uint32_t count = entryCount.u32 & 0xFFu;
  for (uint32_t i = 0; i < count && g_replay_entry_visual_idx < op->entryVisualCount; ++i) {
    const uint8_t idx = g_replay_entry_visual_idx++;
    GuestW32(entry.u32 + i * 0x68u + 0x1Cu, op->entryPackedParams[idx]);
  }
  if (g_replay_entry_visual_idx != firstIdx)
    layerAlpha.u32 = (layerAlpha.u32 & ~0xFFu) | op->entryLayerAlpha[firstIdx];
}

// Gate-F tripwire (resolved off-pass). insert_sprite's forced-flag-0 replay is byte-exact ONLY while the
// object-dependent rescale is a 320/240 identity no-op (proven in logs 296/297). This re-runs the recomp's
// mode-3 position math (0.cpp:38538-38565) for the emitting object during the LIVE walk and warns
// (budgeted) if it would actually MOVE the sprite — meaning that scene set the screen-dim globals to
// something other than 320/240 and the replay must reproduce the rescale there. g_emit_obj is the live
// emitting object here so the reads are safe.
void GateFTripwire(int32_t screenX, int32_t screenY) {
  if (!g_emit_obj || g_gatef_warn_budget <= 0) return;
  const uint8_t f2b = GuestR8(g_emit_obj + 0x2Bu);
  const uint16_t s20 = static_cast<uint16_t>(GuestR16(g_emit_obj + 0x20u));
  const uint16_t s22 = static_cast<uint16_t>(GuestR16(g_emit_obj + 0x22u));
  if (!f2b || !s20 || !s22) return;  // rescale block skipped by the game too -> nothing to reproduce
  const uint32_t o58 = GuestR32(g_emit_obj + 0x58u);
  if (!o58 || GuestR16(o58 + 0x0Cu) != 3) return;  // only mode 3 rescales the position
  const int32_t dimX = static_cast<int16_t>(GuestR16(kRescaleDimX));
  const int32_t dimY = static_cast<int16_t>(GuestR16(kRescaleDimY));
  const int32_t newX = (screenX - 160) * dimX / 320 + 160;
  const int32_t newY = (screenY - 120) * dimY / 240 + 120;
  if (newX == screenX && newY == screenY) return;  // identity (dims=320/240) -> forced-flag-0 is exact
  --g_gatef_warn_budget;
  REXLOG_INFO(
      "fps-resolved: GATE_F TRIPWIRE — rescale MOVES sprite obj={:#010x} dims=({},{}) "
      "screenXY=({},{})->({},{}); forced-flag-0 replay is NOT byte-exact this scene",
      g_emit_obj, dimX, dimY, screenX, screenY, newX, newY);
}

// Capture one resolved insert_sprite op — the off-pass sprite capture (armed by INI FpsInterpolate via
// FpsActive). Called from the insert_sprite ENTRY hook during the REAL producer walk (g_in_sprite,
// !g_replaying), where g_emit_obj is the live emitting object — so the ONE object read here (its pose
// delta) is always safe. r3..r10 are the fully-resolved insert_sprite args; nothing on the op re-derives
// from volatile object memory on a later tick, so the replay is object-free. Pushes into the capture
// stream (g_capture_accum), interleaved in call order with Entry/Seal ops, and points
// g_active_producer_capture_idx at itself so insert_sprite's nested add_entry attaches its baked
// entry-visual to this op.
void CaptureResolvedSprite(uint32_t callerSp, uint32_t a3, uint32_t r4, uint32_t r5, uint32_t r6,
                           uint32_t r7, uint32_t r8, uint32_t r9, uint32_t r10) {
  CapturedOp op{};
  op.kind = CapturedOp::Kind::ResolvedSprite;
  op.a[0] = a3;                                           // sort/layer key (r3)
  op.a[1] = r4;                                           // resolved cel/frame-record ptr (r4)
  op.a[2] = r5;                                           // baked screen X (low 16 = int16)
  op.a[3] = r6;                                           // baked screen Y
  op.a[4] = r7;                                           // x sub-offset
  op.a[5] = r8;                                           // y sub-offset
  op.a[6] = (r9 & 0xFFu) | ((r10 & 0xFFu) << 8);          // flag9 | flag10<<8
  // Snapshot the caller-frame input window insert_sprite reloads from ITS caller's stack (see frameWin doc
  // — alpha/scale/obj/flip flags). The probe hook runs at insert_sprite ENTRY, BEFORE its `stwu r1,-1616`
  // (recomp 0.cpp:38444), so callerSp is the PRODUCER's live frame here — copy the exact bytes insert_sprite
  // will consume (producer-agnostic, byte-exact). callerSp==0 (no r1) -> leave frameValid false so the
  // replay falls back to the old force-0 (blank) rather than writing garbage.
  if (callerSp) {
    op.frameValid = true;
    for (uint32_t i = 0; i < CapturedOp::kFrameWinLen; ++i)
      op.frameWin[i] = GuestR8(callerSp + CapturedOp::kFrameWinLo + i);
  }
  op.hudFixed = g_in_weapon_hud_producer;                 // camera-anchored HUD -> screen-fixed (no lerp)
  // SCREEN-space per-emit motion delta (cur - prev), from the LIVE pose of the emitting object (pose+0x18/
  // 0x1C = cur X/Y, +0x20/+0x24 = prev X/Y, 16-bit world px in the high half). The camera term cancels in
  // cur-prev. NOTE the Y INVERSION: the producer bakes screenX = worldX - camX (same sign) but screenY =
  // camY - worldY (recomp.2.cpp:23576/23578), so the SCREEN Y delta is the NEGATIVE of the world Y delta.
  // The replay lerps the baked SCREEN position by this delta directly (it does not re-run the producer, so
  // it can't rely on the producer to re-apply the inversion). X = cur-prev; Y = prev-cur. Getting the Y
  // sign wrong makes vertical motion (jumps/falls) interpolate BACKWARD (the log-303 stutter). hudFixed
  // (camera-anchored) ops stay put -> zero delta.
  if (!op.hudFixed) {
    const uint32_t pose = g_emit_obj ? GuestR32(g_emit_obj + 0x88u) : 0;
    if (pose) {
      op.resolvedLerpDX = (static_cast<int32_t>(GuestR32(pose + 0x18u)) >> 16)
                        - (static_cast<int32_t>(GuestR32(pose + 0x20u)) >> 16);
      op.resolvedLerpDY = (static_cast<int32_t>(GuestR32(pose + 0x24u)) >> 16)   // prev - cur (screen Y is
                        - (static_cast<int32_t>(GuestR32(pose + 0x1Cu)) >> 16);  // inverted vs world Y)
    }
  }
  CaptureSubmitState(op);  // ambient alpha/anchor state insert_sprite reads (its final value at this call)
  GateFTripwire(static_cast<int16_t>(r5 & 0xFFFFu), static_cast<int16_t>(r6 & 0xFFFFu));
  g_active_producer_capture_idx = g_capture_accum.size();  // route the nested add_entry's visual to this op
  g_capture_accum.push_back(op);
}

}  // namespace

bool msxx_disable_go_popups() {
  return GoPopupsDisabled();
}

bool msxx_disable_idle_move_prompt_right(PPCRegister& r3) {
  return DisableIdleMovePromptIfConfigured(r3.u32, kIdleMovePromptRightBeginDelay);
}

bool msxx_disable_idle_move_prompt_left(PPCRegister& r3) {
  return DisableIdleMovePromptIfConfigured(r3.u32, kIdleMovePromptLeftBeginDelay);
}

// Typed callables used by the off-pass replay. The auto-isolating call form
// (operator()(Args...)) fetches ctx/base from the current thread and runs the
// recompiled function against the live global render-list manager, so replayed
// calls rebuild the same list the producers built. Declared at global scope:
// REX_IMPORT references the recompiled symbols (weak aliases to __imp__*).
REX_IMPORT(ms_render_list_add_entry, msxx_fps_replay_add_entry,
           void(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t));
REX_IMPORT(ms_render_list_clear, msxx_fps_replay_render_list_clear, void());
REX_IMPORT(ms_render_list_zero_buckets, msxx_fps_replay_zero_buckets, void());
REX_IMPORT(ms_render_list_sort_seal_batch, msxx_fps_replay_sort_seal_batch, void(uint32_t));
// The object-free chokepoint, called DIRECTLY by the resolved-sprite off-pass replay (step 3) with the
// baked args. NOT via the auto-isolate operator()(args...) form — see msxx_fps_call_insert_sprite for why
// (it needs a wide isolation frame + a zeroed rescale-gate byte at caller_sp+0x97). r3..r10 (8 args).
REX_IMPORT(ms_render_list_insert_sprite, msxx_fps_replay_insert_sprite,
           void(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t));

// The ms_render_frame do_scene trio (setup 320x240 target -> draw+flush -> finish),
// re-invoked by the off-pass render-frame driver to re-draw the lerped sprite list into
// the 320x240 scene EDRAM. This is exactly the subset ms_render_frame runs on pass 0
// MINUS the frame-counter bump + scheduler/pump that follow (those would re-enter the
// fiber loop). All three are void() at the call site (no args set up). The lerped EDRAM
// is carried to the off-pass present via the guest-side buffer-B redirect (see
// msxx_fps_redirect_scene_source); the real present keeps the engine's own resolve.
REX_IMPORT(ms_setup_playfield_target, msxx_fps_setup_playfield_target, void());
REX_IMPORT(ms_scene_render, msxx_fps_scene_render, void());
REX_IMPORT(ms_finish_playfield_render, msxx_fps_finish_playfield_render, void());

static void SetAlphaBlendEnable(uint32_t device, bool enabled) {
  if (!device) return;
  auto* ts = rex::runtime::ThreadState::Get();
  auto* ks = rex::system::kernel_state();
  if (!ts || !ts->context() || !ks || !ks->memory()) return;

  PPCFunc* fn = rex::runtime::ResolveIndirectFunction(kSetAlphaBlendEnableFn);
  if (!fn) return;

  PPCContext* parentCtx = ts->context();
  uint8_t* base = ks->memory()->virtual_membase();
  PPCContext ctx{};
  ctx.r1 = parentCtx->r1;
  ctx.r1.u32 -= 0x70;
  ctx.r13 = parentCtx->r13;
  ctx.fpscr = parentCtx->fpscr;
  ctx.last_indirect_target = kSetAlphaBlendEnableFn;
  ctx.r3.u64 = device;
  ctx.r4.u64 = enabled ? 1u : 0u;
  fn(ctx, base);
  parentCtx->fpscr = ctx.fpscr;
}

// Guest-side redirect (plan MD step 1): clone the scene texture state+0x14 into buffer B.
// ms_d3d_create_texture2d(factory, packedWH, 1, 1, 0, 0x18280186, 0, 3) — the exact args
// ms_create_playfield_upscale_targets (0x823E6C70) uses to build +0x14.
REX_IMPORT(ms_d3d_create_texture2d, msxx_fps_create_texture2d,
           uint32_t(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t));

// Lazily clone the 320x240 scene texture (state+0x14) into g_fps_b_tex, once the state block's
// create factory (+0x2C) and packed size (+0x30) are populated. Called from the in-context driver,
// which runs on the render fiber (valid context for the create call). No-op after the first success.
void EnsureBufferBTexture() {
  if (g_fps_b_tex) return;
  const uint32_t width = GuestR32(kPlayfieldState + 0x2Cu);   // 320
  const uint32_t height = GuestR32(kPlayfieldState + 0x30u);  // 240
  if (!width || !height) return;  // targets not created yet -> try again next tick.
  g_fps_b_tex = msxx_fps_create_texture2d(width, height, 1, 1, 0, 0x18280186u, 0, 3);
  REXLOG_INFO("fps-guest-redirect: created buffer-B texture handle={:#x} (w={} h={})",
              g_fps_b_tex, width, height);
}

// [[midasm_hook]] at ms_render_list_begin_frame (0x82236B60) entry — the
// once-per-frame point where the render list is zeroed. Reset the capture buffer
// here so it accumulates exactly the calls that build this frame's list.
void msxx_fps_capture_begin_frame() {
  if (FpsProbeEnabled() && !g_replaying) {
    if ((++g_probe_begin_frame % 120u) == 0)
      REXLOG_INFO("fps-probe: begin_frame#{} add_sprite={} add_entry={} tid={:#x}",
                  g_probe_begin_frame, g_probe_add_sprite, g_probe_add_entry, ThisTid());
  }
  if (g_replaying) return;
  // New tick -> the next flush/upscale is present #0 of this tick. Reset
  // unconditionally (independent of FpsActive) so the cadence probe works even
  // with interpolation off.
  g_flush_index = 0;
  g_upscale_index = 0;
  if (!FpsActive()) return;
  // Promote the just-completed frame's captures to the replay set, then start a
  // fresh accumulation. swap keeps both buffers' capacity (no per-frame alloc).
  g_capture_ready.swap(g_capture_accum);
  g_capture_accum.clear();
  g_in_sprite = false;                // defensive: never carry a stale bracket across frames.
  g_in_weapon_hud_producer = false;   // ditto for the weapon-HUD producer bracket.
  g_active_producer_capture_idx = SIZE_MAX;

  // Issue #5: advance the per-tick counters the page-pass snapshot-lerp + diag key off of.
  ++g_bg_tick;            // diag tick counter
  g_bg_diag_rec_idx = 0;  // diag: new tick -> first page record is #0 again
}

// [[midasm_hook]] at ms_render_list_add_sprite (0x82236BB8) ENTRY.
//
// This is the HIGH-LEVEL producer: it transforms the object's pose to screen and
// calls ms_render_list_insert_sprite (0x82200170), which funnels into the SAME
// low-level append ms_render_list_add_entry (0x821FC4E8) that UI/glyphs use
// (nested bl at 0x82202FF4/0x82203108). We don't capture at THIS level any more (the
// object-free ResolvedSprite op is captured at insert_sprite instead); this hook only
// opens the suppression bracket (g_in_sprite, cleared at the exit hook) so the nested
// add_entry calls are attributed to the resolved op rather than captured as standalone
// UI entries, and records g_emit_obj so CaptureResolvedSprite can read its live pose.
void msxx_fps_capture_add_sprite(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5,
                                 PPCRegister& r6) {
  (void)r4;
  (void)r5;
  (void)r6;
  if (FpsProbeEnabled() && !g_replaying) {
    if ((++g_probe_add_sprite % 2000u) == 0)
      REXLOG_INFO("fps-probe: add_sprite#{} begin_frame={} add_entry={} tid={:#x}",
                  g_probe_add_sprite, g_probe_begin_frame, g_probe_add_entry, ThisTid());
  }
  if (!FpsActive() || g_replaying) return;
  g_emit_obj = r3.u32;  // the emitting object — CaptureResolvedSprite reads its live pose delta
  g_in_sprite = true;   // suppress the nested add_entry captures until the exit hook
  // The ResolvedSprite op is captured at insert_sprite entry; g_active_producer_capture_idx is pointed at
  // it there so the nested add_entry attaches its visual to it. A producer that never reaches insert_sprite
  // (its emit was internally skipped) contributes nothing — the old issue-#3 off-frame skip can't recur.
  g_active_producer_capture_idx = SIZE_MAX;
}

// [[midasm_hook]] at ms_render_list_add_sprite's converged EXIT (0x82236D1C, the
// addi r1 just before the epilogue `b`, reached by both the normal and early-out
// paths). insert_sprite (and its nested appends) have completed by here, so this
// closes the suppression bracket opened at entry.
void msxx_fps_capture_add_sprite_end() {
  if (!FpsActive() || g_replaying) return;
  g_in_sprite = false;
  g_active_producer_capture_idx = SIZE_MAX;
}

// [[midasm_hook]] brackets on the two weapon-HUD sprite producers: the icon strip (0x82228B18, ENTRY ->
// converged EXIT 0x82228C94) and the slot-range selection draw (0x82229BA8, ENTRY -> two converged EXITs
// 0x8222A548 / 0x8222A56C). Both emit the bottom-left weapon-selection indicator via ms_render_list_add_
// sprite with xoff=cameraX+const, yoff=cameraY+const, so the indicator is screen-fixed. While inside
// either, the ResolvedSprite ops captured at insert_sprite are tagged CapturedOp::hudFixed (the camera is
// already cancelled in the baked screenX, so the off-pass just skips their screen-space lerp). Gated on
// !g_replaying (the producers only run in the engine's real walk). A single shared flag covers both —
// they never nest.
void msxx_fps_weapon_hud_enter() {
  if (!FpsActive() || g_replaying) return;
  g_in_weapon_hud_producer = true;
}
void msxx_fps_weapon_hud_leave() {
  if (!FpsActive() || g_replaying) return;
  g_in_weapon_hud_producer = false;
}

// [[midasm_hook]] at ms_obj_anim_stream_emit_sprite (0x822389E0) ENTRY — the SECOND high-level
// sprite producer. The main animated entities draw through this anim-stream interpreter. Like the
// add_sprite hook, this no longer captures here (the object-free ResolvedSprite op is captured at
// insert_sprite instead); it only records the emitting object and opens the g_in_sprite suppression
// bracket so the nested add_entry insert_sprite makes is attributed to the resolved op, not captured as
// a standalone UI entry. Closed at the converged exit hook below.
void msxx_fps_capture_anim_stream(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
  (void)r4;
  (void)r5;
  if (!FpsActive() || g_replaying) return;
  g_emit_obj = r3.u32;  // the emitting object — CaptureResolvedSprite reads its live pose delta
  g_in_sprite = true;   // suppress the nested add_entry capture until the exit hook
  g_active_producer_capture_idx = SIZE_MAX;  // pointed at the resolved op when insert_sprite fires
}

// [[midasm_hook]] at ms_obj_anim_stream_emit_sprite's converged EXIT (0x82238C10, the `mr r3,r22`
// reached by both the normal post-insert_sprite path and every early-out `beq`). Closes the
// suppression bracket opened at entry.
void msxx_fps_capture_anim_stream_end() {
  if (!FpsActive() || g_replaying) return;
  g_in_sprite = false;
  g_active_producer_capture_idx = SIZE_MAX;
}

// [[midasm_hook]] at ms_render_list_add_entry (0x821FC4E8) entry — the SINGLE
// low-level append both UI/glyphs (direct) and sprites (via insert_sprite) reach.
// We capture only TOP-LEVEL calls (g_in_sprite == false) as standalone Entry ops: those are UI/glyph
// entries, replayed verbatim (no lerp). Calls nested inside producers are regenerated by replaying the
// producer op, but we still sample their final packed alpha/layer alpha here so replay can patch those
// baked visual values onto the regenerated entry.
void msxx_fps_capture_add_entry(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6,
                                PPCRegister& r7, PPCRegister& r8, PPCRegister& r9) {
  if (FpsProbeEnabled() && !g_replaying) ++g_probe_add_entry;
  if (!FpsActive()) return;
  if (g_replaying) {
    ApplyReplayEntryVisualState(r5, r6, r7);
    return;
  }
  if (g_in_sprite) {
    CaptureActiveProducerEntryVisualState(r7.u32, r5.u32, r6.u32);
    return;
  }
  CapturedOp op{};
  op.kind = CapturedOp::Kind::Entry;
  op.a[0] = r3.u32;
  op.a[1] = r4.u32;
  op.a[2] = r5.u32;
  op.a[3] = r6.u32;
  op.a[4] = r7.u32;
  op.a[5] = r8.u32;
  op.a[6] = r9.u32;
  CaptureEntryVisualState(op, r7.u32, r5.u32, r6.u32);
  g_capture_accum.push_back(op);
}

// [[midasm_hook]] at ms_render_list_sort_seal_batch (0x82236798) entry — the call
// that seals the pending sprite batch into the flush's sorted array. Producers
// issue it between layer groups; capture its r3 arg (the sort key/range) so the
// replay reproduces the submit/seal interleaving (otherwise the sprite counters
// diverge and the flush walks a malformed list -> crash).
void msxx_fps_capture_seal(PPCRegister& r3) {
  if (!FpsActive() || g_replaying) return;
  CapturedOp op{};
  op.kind = CapturedOp::Kind::Seal;
  op.a[0] = r3.u32;
  g_capture_accum.push_back(op);
}

struct SubmitStateSave {
  bool active = false;
  uint32_t alphaScaleGate = 0;
  uint32_t anchorEnable = 0;
  uint16_t anchorParam = 0;
  uint16_t anchorScreenX = 0;
  uint32_t anchorObj = 0;
  uint16_t anchorScreenY = 0;
};

static SubmitStateSave ApplyCapturedSubmitState(const CapturedOp& op) {
  if (!op.submitStateValid) return {};
  SubmitStateSave save;
  save.active = true;
  save.alphaScaleGate = GuestR32(kSubmitAlphaScaleGate);
  save.anchorEnable = GuestR32(kSubmitAnchorEnable);
  save.anchorParam = static_cast<uint16_t>(GuestR16(kSubmitAnchorParam));
  save.anchorScreenX = static_cast<uint16_t>(GuestR16(kSubmitAnchorScreenX));
  save.anchorObj = GuestR32(kSubmitAnchorObj);
  save.anchorScreenY = static_cast<uint16_t>(GuestR16(kSubmitAnchorScreenY));

  GuestW32(kSubmitAlphaScaleGate, op.alphaScaleGate);
  GuestW32(kSubmitAnchorEnable, op.anchorEnable);
  GuestW16(kSubmitAnchorParam, op.anchorParam);
  GuestW16(kSubmitAnchorScreenX, op.anchorScreenX);
  GuestW32(kSubmitAnchorObj, op.anchorObj);
  GuestW16(kSubmitAnchorScreenY, op.anchorScreenY);
  return save;
}

static void RestoreSubmitState(const SubmitStateSave& save) {
  if (!save.active) return;
  GuestW32(kSubmitAlphaScaleGate, save.alphaScaleGate);
  GuestW32(kSubmitAnchorEnable, save.anchorEnable);
  GuestW16(kSubmitAnchorParam, save.anchorParam);
  GuestW16(kSubmitAnchorScreenX, save.anchorScreenX);
  GuestW32(kSubmitAnchorObj, save.anchorObj);
  GuestW16(kSubmitAnchorScreenY, save.anchorScreenY);
}

template <typename EmitFn>
static void msxx_fps_replay_with_entry_visual_state(const CapturedOp& op, EmitFn&& emit) {
  const CapturedOp* prevOp = g_replay_entry_visual_op;
  const uint8_t prevIdx = g_replay_entry_visual_idx;
  g_replay_entry_visual_op = &op;
  g_replay_entry_visual_idx = 0;
  emit();
  g_replay_entry_visual_op = prevOp;
  g_replay_entry_visual_idx = prevIdx;
}

// Apply the op's captured ambient submit state + route its nested add_entry visuals, run the emit, then
// restore. Resolved ops don't touch object memory, so there is no obj-render-state save/restore here.
template <typename EmitFn>
static void msxx_fps_replay_with_captured_state(const CapturedOp& op, EmitFn&& emit) {
  const SubmitStateSave save = ApplyCapturedSubmitState(op);
  msxx_fps_replay_with_entry_visual_state(op, emit);
  RestoreSubmitState(save);
}

// Call ms_render_list_insert_sprite DIRECTLY for the resolved off-pass replay. The recompiled
// insert_sprite reloads FOUR values from its caller's stack that the producer stashed just before the
// `bl` (recomp 0.cpp:38450/38491/38499/38507 read caller_sp+0x97/+0x66/+0x6E/+0x9C): the alpha/gate byte,
// the two scale halves, and the obj back-ptr. If the gate byte OR either scale half is 0, insert_sprite
// branches straight to its epilogue (loc_82203124) and inserts NOTHING — the log-303 blank-off-frame bug
// (Gate F misread that early-out as a skippable "cosmetic rescale"; it is actually the whole draw path's
// guard). The auto-isolate call form (r1-=0x70) leaves those slots in the PARENT frame; instead build a
// WIDE isolation frame (r1-=0x120, so the slots land in our own scratch, clear of the parent's locals) and
// WRITE BACK the values captured verbatim from the producer's frame at capture time (op.frame*). Gate F
// proved the resulting rescale is a 320/240 value identity, so reproducing it is byte-exact; the only obj
// touch is the shallow, always-mapped [obj+0x58] mode read (not the wild [obj+0x38] lwzx that crashed the
// producer re-run). If the op has no captured frame (frameValid==false, e.g. a run without the widened
// hook), fall back to force-0 (the sprite won't draw, but nothing is corrupted).
void msxx_fps_call_insert_sprite(const CapturedOp& op, uint32_t a3, uint32_t record, uint32_t screenX,
                                 uint32_t screenY, uint32_t sub7, uint32_t sub8, uint32_t flag9,
                                 uint32_t flag10) {
  auto* ts = rex::runtime::ThreadState::Get();
  auto* ks = rex::system::kernel_state();
  if (!ts || !ts->context() || !ks || !ks->memory()) return;
  PPCContext* parentCtx = ts->context();
  uint8_t* base = ks->memory()->virtual_membase();

  PPCContext ctx{};
  ctx.r1 = parentCtx->r1;
  ctx.r1.u32 -= 0x120;  // wide frame: caller_sp+0x66/+0x6E/+0x97/+0x9C land in our scratch, not the parent's
  ctx.r13 = parentCtx->r13;
  ctx.fpscr = parentCtx->fpscr;
  if (op.frameValid) {
    // Reproduce the producer's caller-frame stash verbatim (alpha/scale/obj/flip) so insert_sprite runs its
    // real draw path with the correct facing direction and flags.
    for (uint32_t i = 0; i < CapturedOp::kFrameWinLen; ++i)
      GuestW8(ctx.r1.u32 + CapturedOp::kFrameWinLo + i, op.frameWin[i]);
  } else {
    GuestW8(ctx.r1.u32 + 0x97u, 0);  // no captured frame -> skip the block (draws nothing, safe)
  }

  msxx_fps_replay_insert_sprite(ctx, base, a3, record, screenX, screenY, sub7, sub8, flag9, flag10);
  parentCtx->fpscr = ctx.fpscr;
}

// Replay one resolved-sprite op (step 3). Object-free: re-issues the baked insert_sprite args at the
// inter-tick midpoint. screenX/Y are lerped back toward the previous tick by the captured pose delta
// (resolvedLerpDX/DY) and reprojected onto the off-pass midpoint camera, then insert_sprite is called
// directly. No liveness/identity/pose guard, no obj-render/pose-rewrite dance — the whole despawn/reuse
// crash class is gone by construction (plan §3/§9).
void msxx_fps_replay_resolved(const CapturedOp& op) {
  int32_t sx = static_cast<int16_t>(op.a[2] & 0xFFFFu);
  int32_t sy = static_cast<int16_t>(op.a[3] & 0xFFFFu);

  if (!op.hudFixed && FpsInterpEnabled() && g_pass_count > 0 && g_pass > 0) {
    const int32_t dX = op.resolvedLerpDX;
    const int32_t dY = op.resolvedLerpDY;
    // Snap (draw at cur, no lerp) on a teleport / scene cut — same ±48 px threshold the producer path uses.
    const bool snap = dX > 48 || dX < -48 || dY > 48 || dY < -48;
    if (!snap) {
      // remaining fraction toward prev = (pass_count - pass)/pass_count (pass 1/2 -> half a tick back).
      const int64_t rem = static_cast<int64_t>(g_pass_count - g_pass);
      sx -= static_cast<int32_t>((static_cast<int64_t>(dX) * rem) / g_pass_count);
      sy -= static_cast<int32_t>((static_cast<int64_t>(dY) * rem) / g_pass_count);
      // Camera term (issue #5): the baked screen pos has cam_cur baked in, but the off-pass camera is set
      // to cam_mid, so reproject onto cam_mid so resolved sprites track the lerped backdrop. The producer
      // re-run gets this implicitly by projecting the lerped pose through the mid camera; here the screen
      // pos is pre-baked so we add it explicitly. Skipped if the camera snapped (cam_mid == cam_cur -> 0).
      // Same Y inversion as the pose delta: screenX = worldX - camX so +(cam_cur - cam_mid), but screenY =
      // camY - worldY so the Y term is the NEGATIVE, +(cam_mid - cam_cur).
      if (g_cam_applied) {
        sx += g_cam_save_x - static_cast<int32_t>(GuestR16(kCameraX));
        sy += static_cast<int32_t>(GuestR16(kCameraY)) - g_cam_save_y;
      }
    }
  }

  const uint32_t rx = (op.a[2] & 0xFFFF0000u) | static_cast<uint16_t>(sx);
  const uint32_t ry = (op.a[3] & 0xFFFF0000u) | static_cast<uint16_t>(sy);
  const uint32_t flag9 = op.a[6] & 0xFFu;
  const uint32_t flag10 = (op.a[6] >> 8) & 0xFFu;

  // Apply the captured submit state + route the nested add_entry's visuals to this op.
  msxx_fps_replay_with_captured_state(op, [&] {
    msxx_fps_call_insert_sprite(op, op.a[0], op.a[1], rx, ry, op.a[4], op.a[5], flag9, flag10);
  });
}

// Dispatch one captured op to the right replay path. Single point of truth for the off-pass driver so a
// new op kind is handled everywhere at once.
void msxx_fps_replay_op(const CapturedOp& op) {
  switch (op.kind) {
    case CapturedOp::Kind::ResolvedSprite:
      msxx_fps_replay_resolved(op);
      break;
    case CapturedOp::Kind::Entry:
      msxx_fps_replay_with_entry_visual_state(op, [&] {
        msxx_fps_replay_add_entry(op.a[0], op.a[1], op.a[2], op.a[3], op.a[4], op.a[5], op.a[6]);
      });
      break;
    case CapturedOp::Kind::Seal:
      msxx_fps_replay_sort_seal_batch(op.a[0]);
      break;
  }
}

// [[midasm_hook]] at ms_render_list_flush (0x821FC5E8) ENTRY — the FIBER
// per-present seam (the relocated interpolation trigger; see the MD "LOCKED PLAN"
// from log 190). The flush drains the baked sprite list to GPU quads once per
// present; in gameplay it runs on the render-task fiber ~2x per 30Hz tick. We
// number presents within the tick (g_flush_index, reset at begin_frame): index 0
// is the first present, index 1 the second (tween).
//
// On each of the first two presents we REBUILD the live list from the captured
// producer calls at the present's interpolation phase BEFORE the flush walks it:
//   present #0 -> phase 0.0  = prev tick (N-1)
//   present #1 -> phase 0.5  = midpoint(N-1, N)
// Interpolating BOTH (not just the tween) keeps the displayed sequence monotonic
// (...N-1, mid, N, mid, N+1...) — leaving the real present at cur(N) and only
// tweening the off-pass produces a back-and-forth stutter. The lerp reads the
// LIVE pose pair (cur = pose+0x18/0x1c, prev = pose+0x20/0x24) so no future state
// is needed; the cost is the standard 1-tick interpolation latency.
//
// Rebuild source = g_capture_accum (THIS tick's object list; complete after the
// walk, which precedes the scene flush). Reusing the proven clear + replay +
// seal(0) + zero_buckets machinery, just driven from here instead of the pass loop.
void msxx_fps_flush_driver() {
  if (g_replaying) return;  // our own replay never calls flush, but be defensive.

  const uint32_t idx = g_flush_index++;

  if (FpsProbeEnabled()) {
    if ((++g_probe_flush % 120u) == 0)
      REXLOG_INFO(
          "fps-probe: flush#{} idx_in_tick={} begin_frame={} accum={} ready={} tid={:#x}",
          g_probe_flush, idx, g_probe_begin_frame, g_capture_accum.size(),
          g_capture_ready.size(), ThisTid());
  }

  // ⛔ REBUILD INTENTIONALLY REMOVED — the flush-seam hypothesis is DEAD (logs
  // 191/193). The probe proved ms_render_list_flush runs ~ONCE per 30Hz tick
  // (flush# ≈ begin_frame#, NOT 2x/tick), so there is no "tween present" at this
  // seam — the 60Hz second present is the per-present UPSCALE (0x823E6DF0/
  // 0x823E7310) re-sampling the single scene buffer 0x1F214000, not a second
  // flush. Also, at flush time accum=1 while ready≈18-24: the flush draws the
  // PREVIOUS tick's completed list (ready); the current tick's walk runs AFTER
  // this flush. Rebuilding from accum here wiped the list to ~1 sprite (log 193:
  // after=0/0/0, all sprites vanished, motion still 30fps). The interpolator must
  // move to the per-present upscale seam and re-feed a lerped scene (via the
  // host re-resolve), using g_capture_ready. This hook is now PROBE-ONLY.
}

// [[midasm_hook]] at ms_upscale_playfield_to_640x480 (0x823E6DF0) ENTRY — PROBE
// for the corrected (per-present) architecture. Log 187 shows the upscale chain
// resolves at 60Hz (2x/tick) while the sprite flush is 30Hz (logs 191/193), so
// the upscale is the real per-present seam where an off-pass interpolated scene
// must be produced. This counts upscale calls within the tick (g_upscale_index,
// reset at begin_frame) so we can confirm 2x/tick on the fiber and identify the
// off-pass call (index 1). accum/ready sizes are logged to confirm the capture
// set state at present time (expect ready = the complete displayed list).
void msxx_fps_upscale_probe() {
  if (g_replaying) return;
  const uint32_t idx = g_upscale_index++;
  if (FpsProbeEnabled()) {
    ++g_probe_upscale;
    // Log the first ~12 calls UNTHROTTLED so the per-tick idx alternation (0,1,0,1
    // => 2x/tick) is visible, then a coarse odd-interval sample (121, so parity
    // keeps flipping rather than sticking on one idx like the even-120 flush probe).
    if (g_probe_upscale <= 12 || (g_probe_upscale % 121u) == 0)
      REXLOG_INFO(
          "fps-probe: upscale#{} idx_in_tick={} begin_frame={} flush={} accum={} ready={} tid={:#x}",
          g_probe_upscale, idx, g_probe_begin_frame, g_probe_flush, g_capture_accum.size(),
          g_capture_ready.size(), ThisTid());
  }

  // One-time echo of the gate states, so a run's log unambiguously shows whether
  // the fps envs are actually set (log 196 ambiguity: it turned out the envs were
  // NOT set, i.e. run.bat was used instead of the fps run script).
  static bool s_echoed = false;
  if (!s_echoed) {
    s_echoed = true;
    REXLOG_INFO(
        "fps-gate: FpsActive={} FpsInterp={} OFFPASS_PIPELINE={} ISPROBE={}",
        FpsActive(), FpsInterpEnabled(), FpsActive(), FpsIsProbeEnabled());
  }
}

// [[midasm_hook]] at 0x823F9CBC in ms_render_frame (0x823F9C78) — the CHOSEN off-pass driver
// (MSXX_60FPS_OFFPASS_FEED_OPTIONS.md DECISION). This sits right after the real do-scene trio
// (setup 0x823E6F60 -> scene_render 0x82250390 -> finish 0x823E7018) returns and BEFORE the
// frame-counter bump (0x823f9cbc: `lis r9,-32045` ... `[0x82D2A860]++`), i.e. while the device
// is still in scene-render state. r31 holds the saved do_scene arg (mr r31,r3 @ 0x823f9c8c); the
// trio only ran when r31 != 0, so we re-render only then (do_scene==0 calls did no scene work).
//
// Why HERE and not the upscale hook: invoked in-context, setup_playfield_target rebinds the
// 320x240 scene RT to the SAME EDRAM tile as the real render (the upscale-hook driver bound a
// DIFFERENT tile -> black, log 205). We rebuild the captured displayed-frame list at the inter-
// tick midpoint, re-bind, and re-draw it into the hot scene EDRAM, then resolve THAT lerped EDRAM
// into guest buffer B by calling finish (0x823E7018) with [state+0x14]
// temporarily pointed at B_tex — so the real present's resolve to 0x1F214000 is untouched and the
// lerped frame lands in B. msxx_fps_redirect_scene_source then points present #0's upscale source
// at B, so the displayed sequence leads with the midpoint (prev->mid->cur, issue #2 direction fix).
void msxx_fps_render_frame_driver(PPCRegister& do_scene_saved) {
  if (!FpsActive() || g_replaying) return;
  if ((do_scene_saved.u32 & 0xFFu) == 0) return;  // trio skipped this call -> nothing to redo.
  if (g_capture_ready.empty()) return;            // no captured list yet (boot) -> leave it.

  // Guest-side redirect: lazily clone the buffer-B texture once (filled below, consumed by the
  // present #0 source redirect in msxx_fps_redirect_scene_source).
  EnsureBufferBTexture();

  // DIAGNOSTIC (budgeted): classify the captured list by op kind. The interpolated draws are the
  // ResolvedSprite ops (object-free, lerped via the baked screenX/Y delta); Entry/Seal replay verbatim.
  if (FpsProbeEnabled() && (++g_capture_class_dbg % 60) == 0) {
    size_t resolved = 0, ent = 0, seal = 0;
    for (const CapturedOp& op : g_capture_ready) {
      if (op.kind == CapturedOp::Kind::ResolvedSprite) ++resolved;
      else if (op.kind == CapturedOp::Kind::Entry) ++ent;
      else if (op.kind == CapturedOp::Kind::Seal) ++seal;
    }
    REXLOG_INFO("fps-capclass: total={} resolved={} entry={} seal={}",
                g_capture_ready.size(), resolved, ent, seal);
  }

  // Off-pass phase = midpoint (0.5): prev = pose+0x20/0x24 (tick start), cur = pose+0x18/0x1c.
  // Under FpsReplayDryRun this is phase 0 (verbatim) for the pixel-identical sanity pass.
  g_pass = 1;
  g_pass_count = 2;

  // Rebuild the captured displayed-frame list, lerped. Native makes a list flushable by sealing
  // pending batches and then running zero_buckets; doing zero_buckets before replay is a no-op after
  // clear() and leaves stale bucket/order state in the synthetic frame.

  // Issue #5 sync: set the int16 camera to the inter-tick midpoint so the off-pass sprites project with
  // the same camera the lerped backdrop uses (else sprites stay at camera_N while the backdrop is at
  // camera_mid -> they slide a half-step). Snapshot cur (restored after the off-pass), roll prev. Snap
  // to cur on a large jump (scene cut) so we don't project sprites through a wild interpolated camera.
  g_cam_applied = false;
  if (FpsInterpEnabled() && g_pass_count) {
    // Lerp the FULL 16.16 camera (low half = the sub-pixel fraction that the float bg UV scroll
    // tracks), THEN floor the high half ONCE — exactly what the engine's own lhz read would yield for
    // a real frame rendered at the midpoint. Because sprites only read the high 16 bits, we write just
    // that half (GuestW16) and leave cur's sub-pixel fraction in the low half untouched; restoring the
    // saved high half fully reverts it. Snap to cur on a large jump (scene cut).
    const int32_t cur_x = static_cast<int32_t>(GuestR32(kCameraX));
    const int32_t cur_y = static_cast<int32_t>(GuestR32(kCameraY));
    if (!g_cam_seeded) {
      g_cam_prev_x = cur_x;
      g_cam_prev_y = cur_y;
      g_cam_seeded = true;
    }
    const int64_t dx = static_cast<int64_t>(cur_x) - g_cam_prev_x;
    const int64_t dy = static_cast<int64_t>(cur_y) - g_cam_prev_y;
    const int64_t snap = static_cast<int64_t>(BgCfg().snap_px) << 16;
    int32_t mid_x = cur_x, mid_y = cur_y;  // 16.16
    if (dx <= snap && dx >= -snap && dy <= snap && dy >= -snap) {
      mid_x = g_cam_prev_x + static_cast<int32_t>(dx * g_pass / g_pass_count);
      mid_y = g_cam_prev_y + static_cast<int32_t>(dy * g_pass / g_pass_count);
    }
    g_cam_save_x = GuestR16(kCameraX);  // original high half (px), restored after the off-pass
    g_cam_save_y = GuestR16(kCameraY);
    GuestW16(kCameraX, static_cast<uint16_t>(mid_x >> 16));  // arithmetic floor of the 16.16 midpoint
    GuestW16(kCameraY, static_cast<uint16_t>(mid_y >> 16));
    g_cam_prev_x = cur_x;
    g_cam_prev_y = cur_y;
    g_cam_applied = true;
  }

  g_replaying = true;
  msxx_fps_replay_render_list_clear();
  for (const CapturedOp& op : g_capture_ready)
    msxx_fps_replay_op(op);
  msxx_fps_replay_sort_seal_batch(0);
  msxx_fps_replay_zero_buckets();

  // Re-bind the 320x240 scene RT (the linchpin: in-context this binds the correct tile) and draw
  // the lerped list into the hot scene EDRAM. setup clears+binds; scene_render draws border +
  // flushes the list. No finish() -> no resolve to 0x1F214000.
  msxx_fps_setup_playfield_target();
  const uint32_t offpassDevice = GuestR32(kRenderDevice);
  if (offpassDevice) {
    // The real scene pass ends by disabling alpha blend in finish_playfield_render. The synthetic
    // off-pass runs after that finish, so re-enable the native blend state before flushing sprites.
    SetAlphaBlendEnable(offpassDevice, true);
  }
  msxx_begin_playfield_scene_filter_scope();
  msxx_fps_scene_render();
  msxx_end_playfield_scene_filter_scope();
  if (offpassDevice) {
    SetAlphaBlendEnable(offpassDevice, false);
  }

  // Guest-side redirect: fill buffer B via the engine's OWN resolve. ms_finish_playfield_render
  // (0x823E7018) resolves the live scene EDRAM into [state+0x14] (a DYNAMIC dest: lwz r6,[state+0x14]
  // @0x823e7070) and sets the present-ready byte. Temporarily point [state+0x14] at B_tex around the
  // finish call, so the LERPED EDRAM we just drew resolves into B (not the real scene texture / not
  // 0x1F214000). The real frame already resolved to its own [state+0x14] in the trio, so present #0's
  // resolve is untouched; msxx_fps_redirect_scene_source then samples B for the leading present.
  if (g_fps_b_tex) {
    const uint32_t save14 = GuestR32(kPlayfieldState + 0x14u);
    GuestW32(kPlayfieldState + 0x14u, g_fps_b_tex);
    msxx_fps_finish_playfield_render();
    GuestW32(kPlayfieldState + 0x14u, save14);
  }

  // Issue #5 sync: restore the real camera so the next tick's real render is unaffected.
  if (g_cam_applied) {
    GuestW16(kCameraX, static_cast<uint16_t>(g_cam_save_x));
    GuestW16(kCameraY, static_cast<uint16_t>(g_cam_save_y));
    g_cam_applied = false;
  }
  g_replaying = false;

  if (g_renderframe_drv_dbg > 0) {
    --g_renderframe_drv_dbg;
    REXLOG_INFO("fps-renderframe off-pass: redrew ready={} lerp={} phase={}/{} tid={:#x}",
                g_capture_ready.size(), FpsInterpEnabled(), g_pass, g_pass_count, ThisTid());
  }
}

// Guest-side redirect. [[midasm_hook]] at 0x823E6DA8 — the `bl ms_d3d_set_texture` inside
// ms_bind_playfield_upscale_draw_state (0x823E6D00). r5 = the scene source texture object
// (= [state+0x14], loaded at 0x823e6da0), about to be bound to sampler 0 before the 320->640 upscale.
//
// Off-pass discriminator: the bind runs immediately BEFORE the upscale (0x823E6DF0) in each present's
// re-run (present: bl 0x823E6D00 @0x823e7388, then bl 0x823E6DF0 @0x823e7390), and g_upscale_index is
// what the upscale entry increments. So at bind time g_upscale_index == 0 for present #0's re-run and
// == 1 for present #1.
//
// DIRECTION FIX (issue #2, 2026-06-26): B_tex holds mid(N-1,N) — a PAST half-step relative to the real
// frame N in +0x14. Redirecting present #1 (==1) gave display order [N, mid(N-1,N)] = step forward then
// back (judder). B_tex is filled in the render-frame driver BEFORE both presents, so it's ready for
// present #0 too. Redirect present #0 (==0) instead so the LERPED frame LEADS: [mid(N-1,N), N] =
// monotonic prev->mid->cur. (The upscale runs exactly 2x/tick — logs 187/194 — only the two present
// re-runs hit this bind, so there is no third standalone upscale to mis-trigger on.)
void msxx_fps_redirect_scene_source(PPCRegister& r5) {
  if (g_replaying) return;
  if (!FpsActive() || !g_fps_b_tex) return;
  if (g_upscale_index != 0) return;  // off-pass leads: redirect present #0 to the lerped buffer
  r5.u32 = g_fps_b_tex;
}

// [[midasm_hook]] at ms_render_list_insert_sprite (0x82200170) ENTRY — the single chokepoint where BOTH
// producers hand off a sprite with its FINAL baked screen position (camera + per-part offset already
// applied; config:268). Args (disasm): r4 = cel/frame-record ptr, r5 = screenX, r6 = screenY,
// r7 = xoff, r8 = yoff.
//
// #4 position PROBE (MSXX_FPS_ISPROBE=1, verify tool) — logs the final baked screen pos + pose curY/prevY
// at CAPTURE (engine walk) vs REPLAY (off-pass) so the player's per-part positions can be compared. This
// is what proved the torso/legs collapse and then confirmed the per-emit pose-capture fix. Read-only.
void msxx_fps_probe_insert_sprite(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6,
                                  PPCRegister& r7, PPCRegister& r8, PPCRegister& r9, PPCRegister& r10,
                                  PPCRegister& r1) {
  // Resolved-sprite capture: record the fully-resolved insert_sprite args from the REAL producer walk.
  // Gated on g_in_sprite so only producer-driven emits are captured (any unrelated caller of
  // insert_sprite is skipped, matching risk E); never during replay (that would double-capture). r1 is the
  // caller's stack ptr (this hook runs BEFORE insert_sprite's own `stwu`), so CaptureResolvedSprite reads
  // the four caller-frame slots insert_sprite will reload (alpha/scale/obj) straight from the producer frame.
  if (FpsActive() && g_in_sprite && !g_replaying)
    CaptureResolvedSprite(r1.u32, r3.u32, r4.u32, r5.u32, r6.u32, r7.u32, r8.u32, r9.u32, r10.u32);
  if (!FpsIsProbeEnabled()) return;
  const int16_t raw_sy = static_cast<int16_t>(r6.u32 & 0xFFFF);  // producer's baked screenY
  const bool rep = g_replaying;
  if (!rep && !g_in_sprite) return;  // only the producer-driven emits (skip any unrelated callers)
  if (g_isprobe_remaining <= 0) return;
  // Sample a short BURST (3 ticks) every ~120 begin_frames (~2s) so the window is spread across the
  // whole session — boot AND gameplay-while-moving — instead of a single front-loaded budget that
  // would drain before the player gets into a stage. The total backstop cap still bounds log size.
  if ((g_probe_begin_frame % 120u) >= 3u) return;
  --g_isprobe_remaining;
  const uint32_t pose = GuestR32(g_emit_obj + 0x88u);
  REXLOG_INFO(
      "fps-isprobe {} obj={:#010x} pose={:#010x} cel={:#010x} rawSY={} screenXY=({},{}) off=({},{}) "
      "curY={} prevY={}",
      rep ? "REP" : "CAP", g_emit_obj, pose, r4.u32, raw_sy, static_cast<int16_t>(r5.u32 & 0xFFFF),
      static_cast<int16_t>(r6.u32 & 0xFFFF), static_cast<int16_t>(r7.u32 & 0xFFFF),
      static_cast<int16_t>(r8.u32 & 0xFFFF), GuestR16(pose + 0x1Cu), GuestR16(pose + 0x24u));
}

// ===========================================================================
// 60fps #5 BACKGROUND probe — locate the scene_render pass that draws the
// "camera"/backdrop the user sees stepping on off-frames.
// ===========================================================================
//
// RE finding (2026-06-26): ms_scene_render reads the world-scroll globals
// (0x82D89AA0/A2/A4) ONLY for culling (compare vs -248 @0x8225078c), never for
// positioning, and the tile pass derives its vertex positions purely from the
// tilemap grid (fixed origin at column 0). So the background's per-tick position
// is BAKED IN, exactly like the sprite poses — there is no global scroll-translate
// to override (this is why the earlier renderer-camera override was a no-op). To
// smooth the backdrop we must shift the VERTICES of whichever pass draws it on
// the off-pass. First we must know WHICH pass that is.
//
// The off-pass driver already re-runs the WHOLE ms_scene_render (border + every
// queue flush + the tile pass) into the lerped frame, so the backdrop is redrawn
// at the CURRENT tick position while the sprites are lerped to the midpoint ->
// steppy backdrop, smooth sprites. This probe SKIPS a chosen pass on the off-pass
// ONLY (g_replaying): that layer then appears on the real frame but is MISSING on
// the interpolated off-pass, so if it draws the stepping backdrop it FLICKERS at
// 30Hz. Run the game scrolling with MSXX_FPS_BGPROBE_SKIP set to each value:
//   "tile"   -> ms_background_draw_tile_sprite_batches (0x822031D0)
//   "page"   -> page-entry queue flushes  (0x821F9038 / 0x821F9230 / 0x821F90F8)
//   "effect" -> effect-quad queue flushes (0x821FE328 / 0x821FF348 / 0x821FFD80)
// The value that makes the backdrop flicker identifies the pass. If NONE do, the
// backdrop is drawn through the captured object path (player/parallax objects,
// already lerped) — a different investigation. Each function below is a
// [[midasm_hook]] at the pass entry with return_on_true (metalslugxx_config.toml),
// so when armed it returns from that pass immediately (before its prologue). The
// gate is g_replaying, so the real frame and all non-off-pass callers are never
// affected. Pure diagnostic; default OFF (env unset).
namespace {
bool BgProbeSkipIs(const char* tag) {
  static const char* env = std::getenv("MSXX_FPS_BGPROBE_SKIP");
  return env && env[0] && std::strcmp(env, tag) == 0;
}
}  // namespace

bool msxx_bgprobe_skip_tile() { return g_replaying && BgProbeSkipIs("tile"); }
bool msxx_bgprobe_skip_page() { return g_replaying && BgProbeSkipIs("page"); }
bool msxx_bgprobe_skip_effect() { return g_replaying && BgProbeSkipIs("effect"); }

// ===========================================================================
// 60fps #5 BACKGROUND interpolation — per-slot snapshot-lerp of the PAGE pass.
// ===========================================================================
//
// Hooks ms_render_draw_sprite_page_entry (0x82206E68). The background is UV-scrolled fixed quads (diag
// logs 256/257: geometry static, UVs slide with the camera, record addresses stable per screen cell).
// So we snapshot each record's 8 position+UV fields per tick keyed by record address (= slot identity),
// and on the off-pass (g_replaying) draw each record at lerp(prev,cur,t) where t = g_pass/g_pass_count
// (=0.5). The ENTRY hook writes the lerped fields into the live record; the EXIT hook (0x8220714C, the
// converged epilogue) restores them so the engine's queue is pristine for the next real render.
namespace {
float BitsToFloat(uint32_t b) {
  float f;
  std::memcpy(&f, &b, sizeof(f));
  return f;
}
uint32_t FloatToBits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, sizeof(b));
  return b;
}
}  // namespace

// [[midasm_hook]] at ms_render_draw_sprite_page_entry (0x82206E68) ENTRY — r3 = the 0x60-byte page
// record. On the REAL pass we roll this slot's snapshot (prev <- last cur, cur <- live). On the off-pass
// we draw it at the inter-tick midpoint by writing lerp(prev,cur,t) into the record (restored at exit).
void msxx_fps_bg_shift_page_entry(PPCRegister& r3) {
  g_page_shift.active = false;
  const BgInterpCfg& cfg = BgCfg();
  if (!FpsInterpEnabled()) return;
  const uint32_t rec = r3.u32;
  if (!rec) return;

  if (!g_replaying) {
    // REAL pass (once per tick per record): advance this slot's snapshot. prev = previous tick's cur,
    // cur = this tick's live fields. First sighting seeds prev = cur (no lerp until two ticks seen).
    PageSlot& s = g_page_slots[rec];
    for (int i = 0; i < 8; ++i) {
      s.prev[i] = s.cur[i];
      s.cur[i] = GuestR32(rec + kPageFieldOff[i]);
    }
    if (!s.seeded) {
      for (int i = 0; i < 8; ++i) s.prev[i] = s.cur[i];
      s.seeded = true;
    }
    // DIAG: per-tick prev->cur delta (px) of this slot's fields — confirms the lerp input (UV scroll)
    // and slot identity (a stable slot shows a small smooth delta; a reassigned slot shows a huge jump).
    if (BgDiagEnabled()) {
      const int idx = g_bg_diag_rec_idx++;
      if (idx < 6 && (g_bg_tick % 20) == 0 && g_bg_diag_budget > 0) {
        --g_bg_diag_budget;
        auto dpx = [&](int i) { return (BitsToFloat(s.cur[i]) - BitsToFloat(s.prev[i])) / 4096.0f; };
        REXLOG_INFO(
            "fps-bgdiag tick={} rec#{} addr={:#010x} dUV=({:.2f},{:.2f},{:.2f},{:.2f}) "
            "dXY=({:.2f},{:.2f},{:.2f},{:.2f}) cam7450={:#x} cam7454={:#x}",
            g_bg_tick, idx, rec, dpx(0), dpx(1), dpx(2), dpx(3), dpx(4), dpx(5), dpx(6), dpx(7),
            GuestR32(0x82D74750u), GuestR32(0x82D74754u));
      }
    }
    return;
  }

  // OFF-PASS: draw this record at the midpoint. Need a seeded snapshot for this slot.
  auto it = g_page_slots.find(rec);
  if (it == g_page_slots.end() || !it->second.seeded) return;
  const PageSlot& s = it->second;

  if (g_pass_count == 0) return;
  const float t = static_cast<float>(g_pass) / static_cast<float>(g_pass_count);

  // Stash cur for the exit-hook restore (the live record currently holds cur == s.cur).
  g_page_shift.active = true;
  g_page_shift.rec = rec;
  for (int i = 0; i < 8; ++i) g_page_shift.cur[i] = s.cur[i];

  // DEBUG override: force a constant geometry-X shift (verify the field offsets reach the screen).
  if (cfg.debug_px != 0) {
    const float dx = static_cast<float>(cfg.debug_px) * 4096.0f;
    GuestW32(rec + 0x0Cu, FloatToBits(BitsToFloat(s.cur[4]) + dx));  // +0x0C
    GuestW32(rec + 0x3Cu, FloatToBits(BitsToFloat(s.cur[6]) + dx));  // +0x3C
    return;
  }

  // Snap guard: if any field jumps more than snap_px this tick (texture wrap / scene cut), draw at cur
  // (the live record already holds cur) — lerping across a wrap would streak. Keep the quad coherent by
  // snapping the WHOLE record, not individual corners.
  for (int i = 0; i < 8; ++i) {
    const float d = (BitsToFloat(s.cur[i]) - BitsToFloat(s.prev[i])) / 4096.0f;
    if (d > cfg.snap_px || d < -cfg.snap_px) return;  // restore at exit puts back cur (a no-op here)
  }

  // Lerp every field prev->cur to the off-pass phase and write into the live record.
  for (int i = 0; i < 8; ++i) {
    const float p = BitsToFloat(s.prev[i]);
    const float c = BitsToFloat(s.cur[i]);
    GuestW32(rec + kPageFieldOff[i], FloatToBits(p + t * (c - p)));
  }
}

// [[midasm_hook]] at the converged epilogue (0x8220714C) — restore the record to cur so the engine's
// queue memory is pristine for the next tick's real render. No-op when the entry hook applied no lerp.
void msxx_fps_bg_restore_page_entry() {
  if (!g_page_shift.active) return;
  const uint32_t rec = g_page_shift.rec;
  for (int i = 0; i < 8; ++i) GuestW32(rec + kPageFieldOff[i], g_page_shift.cur[i]);
  g_page_shift.active = false;
}
