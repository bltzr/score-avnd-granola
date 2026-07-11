# Brief: Granola waveform UI (wfui) session

You are working on **Granola**, a granular synthesis process for ossia score
(avendish-based addon), in this repo (`score-avnd-granola`, branch `flucoma`).
This brief transfers everything you need from the previous sessions. Read it
fully before coding. The user is Pia (expert, prefers concise technical
communication). **Credits rule: never draw overage — if you approach the
session limit, commit what builds and stop cleanly with a pickup note.**

## Context

- Granola lives at `/Users/bltzr/dev/score/src/addons/score-avnd-granola`,
  built as part of the score tree: `ninja -C /Users/bltzr/dev/score/build score`
  (addons auto-included; the binary is `/Users/bltzr/dev/score/build/score`).
- Current state (all pushed on `flucoma`): multifile **SoundBank**
  (`Granola/SoundBank.hpp`) — a folder of audio files scanned in a worker
  thread, sorted by name = stable indices, mtime-cached, rescanned ~2x/s;
  **Sound index** spinbox selects the current sound; **Current sound** string
  output; **MIDI key zones** from a JSON in the folder (note ranges → sound +
  root, per-voice capture at spawn); grains hold their source via
  `GrainSource` view + `shared_ptr` hold (`Granola/grain.hpp/cpp`).
- The model is `Granola/GranolaModel.hpp/cpp`; the declarative custom UI is
  `Granola/GranolaUi.hpp` (`halp_meta(layout, ...)`, `halp::item<&ins::...>`).
- **Port rule**: new ports are ALWAYS appended at the end of ins/outs — score
  restores old scenarios by positional prefix matching on (type, name);
  inserting or renaming breaks saved values.
- avnd worker rule: worker request payloads must be tiny (oscr wraps requests
  in a 128-byte SmallFun) — pass `std::shared_ptr<snapshot>`.
- Test assets: `~/Documents/granola-bank/` (kick.wav root 48 / tone.wav root
  69 / noise.wav root 96 + mapping.json splitting the keyboard in three).

## Task 1 (warm-up): window-shape widget

Replace the standard XY pad for **Window coefs** with a custom-painted widget
showing the actual window shape for the current values. The curve math exists:
`GranuGrain::window()` in `grain.cpp` (beta / cos / kumaraswamy, selected by
`window_mode`), and the mapping from the XY pad to shape coefficients is in
`GranolaModel.cpp` (`windcoef[0/1] = 1 + win_coefs.y * wc_radius * cos/sin(...)`).
Keep the port semantics identical (still an xy value) — only the widget
changes. This is the smallest custom-widget exercise; do it first to learn the
machinery.

## Task 2 (main): waveform widget

**Phase 1 — display.**
- Compute a downsampled min/max peaks envelope (~1024 pairs) per `BankSound`
  at scan time in `scan_folder` (worker thread; cache on the struct).
- Processor → UI via the avnd **message bus** (`avnd/concepts/message_bus.hpp`;
  score side compiles `MessageBusWrapperToUi` for objects with the `ui::bus`
  shape — find working usage in the avendish examples/score tree). Send
  {peaks, name, duration_s} when the displayed sound changes.
- **Which sound is displayed (user decision, firm):** the sound picked by the
  *Sound index* port (and the future dynamic combobox) ONLY. MIDI-zone
  selections must NOT drive the display — notes flying by would thrash it.
- Paint: envelope; overlay the playback window = position + duration as a
  highlighted region; position-jitter and duration-jitter as translucent bands
  at the window's start/end edges.

**Phase 2 — interaction (user-specified gesture map, firm):**
- Drag left–right anywhere: move **position**.
- Vertical drag in the **central part of the window**: change **duration**.
- Vertical drag near the window's **start edge**: **position jitter**.
- Vertical drag near the window's **end edge**: **duration jitter**.
- UI → processor via the bus; processor applies to the controls.
- For mouse handling, the `halp_flag(fully_custom_item)` Qt-layer route may be
  more practical than declarative painter items — see
  `/Users/bltzr/dev/score/3rdparty/avendish/examples/Advanced/UI/2DView.hpp`
  and `PulseView.hpp` (full QPainter + inlet access + mouse). Choose after
  reading both mechanisms; phase 1 plumbing (peaks + bus) is identical either
  way.

## Task 3: global/local parameter mode

A toggle deciding whether **position, duration, their jitters, and window
coefs** are global to the folder or local to the currently picked sound.

- Local mode: per-file values stored in RAM and persisted to disk.
- **Storage (assessed, recommended)**: ONE folder-level JSON with named sets:
  `granola-params.json` = `{ "<set>": { "<file.wav>": { pos, dur, pos_j,
  pos_j_r, dur_j, dur_j_r, win_x, win_y } } }`, plus a new lineedit port
  `Params set` (default "default"). Rejected: per-file sidecars (folder
  clutter, presets = many files); score native presets (per-instance snapshot,
  no per-file dimension, doesn't travel with the folder). The folder-level
  file is copyable as a preset across sessions/projects and live-reloadable
  with the existing watch pattern. Debounced write from a worker on change.
- **RESOLVED by the user**: control write-back is required. On file switch in
  local mode, stored values are written back to the pos/dur/jitter ports so
  the inspector AND the wfui widget reflect them (investigate score's
  `Crousti/ExecutorUpdateControlValueInUi.hpp` — never used by us yet). The
  ports REMAIN (inspector visibility, automation, cables), but once the
  waveform widget is operational, REMOVE the pos/dur+jitters sliders from the
  process panel layout in `GranolaUi.hpp` (declarative: just stop listing
  them) — the widget becomes the only in-scenario surface for these params.

## Background (not tasks)

- A **dynamic combobox** avnd extension is proposed separately (see
  `~/Dropbox/Projets/Raha/Ossia_PdS_Montreal/dynamic-combobox-proposal.md`);
  the sound picker stays index+string until that lands.
- **MIDI zones ↔ FluCoMa reflection** (user musing, not urgent): a zone map is
  a degenerate classifier (note → sound label). A flucoma-dialect version
  would store zones as a dataset/LabelSet pair (note-as-feature →
  sound-as-label), consumable by the sibling score-avnd-flucoma addon's
  KDTreeQuery/MLPClassifier — zones could then be *learned* from examples.
  Keep in mind if it makes a design cleaner; don't build it.

## Practical notes

- Commit style: imperative subject + body explaining why; end with
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`. Commit on the
  `flucoma` branch; push when green. The user tests in the studio in batches.
- Samples are FLOAT throughout (ossia::audio_array, soundfile ports).
- If you touch the model's tick path, remember `buf_soft_lock`, and that
  grains cache per-spawn state — reread `GranolaModel.cpp` before editing.
- The sibling flucoma addon (`score-avnd-flucoma`) holds reference patterns
  for workers/watching if needed; do not modify it from this session.
