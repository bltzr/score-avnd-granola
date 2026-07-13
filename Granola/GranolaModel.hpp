#pragma once

#include "SoundBank.hpp"
#include "grain.hpp"
#include "utils.hpp"

#include <QDebug>

#include <atomic>
#include <halp/audio.hpp>
#include <halp/controls.hpp>
#include <halp/mappers.hpp>
#include <halp/file_port.hpp>
#include <halp/meta.hpp>
#include <halp/midi.hpp>
#include <halp/sample_accurate_controls.hpp>
#include <libremidi/message.hpp>
#include <rnd/random.hpp>

#include <array>
#include <cmath>

#include <random>

namespace Granola
{

typedef std::vector<GranuGrain> GrainVec;

class Granola
{
public:
  halp_meta(name, "Granola")
  halp_meta(category, "Audio/Synth")
  halp_meta(c_name, "granola")
  halp_meta(uuid, "38F9684D-54A6-4F48-91E4-3B251F0956EA")

  //static const int NCHAN{8};

  struct ins
  {
    //halp::soundfile_port<"Window", double> win; // not supported yet
    //halp::range_slider_f32<"In", halp::range_slider_range{-10, 100, {5, 20}}> ta_range;
    // Jitter-range controls removed 2026-07-12: their spread is hardcoded to
    // the former default of 1.0 in the tick. (Reduces the inlet count below
    // score's 32-inlet nodal-fold threshold; breaks positional restore for
    // ports after this point on pre-existing scenarios.)
    halp::hslider_f32<"Position", halp::range{0., 1., 0.}> pos;
    halp::hslider_f32<"Position Jitter", halp::range{0., 1., 0.}> pos_j;
    halp::hslider_f32<"Duration", halp::range{0.01, 1., 1.}> dur;
    halp::hslider_f32<"Duration Jitter", halp::range{0., 1., 0.}> dur_j;
    struct : halp::knob_f32<"Pitch", halp::range{0.000001, 10., 1.}>
    {
      using mapper = halp::inverse_mapper<halp::pow_mapper<4>>;
    } rate;
    halp::vslider_f32<"Pitch Jitter", halp::range{0., 1., 0.}> rate_j;
    halp::toggle<"Reverse"> reverse;
    struct : halp::accurate<halp::knob_f32<"Density", halp::range{0., 256., 1.}>>
    {
      using mapper = halp::log_mapper<std::ratio<99, 100>>;
    } density;
    halp::vslider_f32<"Density Jitter", halp::range{0., 1., 0.}> dens_j;
    halp::knob_f32<"Gain", halp::range{.min = 0., .max = 4., .init = 0.5}> gain;
    halp::vslider_f32<"Gain Jitter", halp::range{0., 1., 0.}> gain_j;
    halp::xy_pad_f32<"Window coefs", halp::range{0.f, 1.f, 0.f}> win_coefs;
    struct
    {
      halp__enum_combobox("Interpolation mode", Cubic, None, Linear, Cubic)
    } interp_type;
    struct
    {
      halp__enum_combobox("Window mode", Kuma, Beta, Cos, Kuma)
    } window_mode;
    halp::toggle<"Warp", halp::toggle_setup{.init = true}> loopmode;
    struct : halp::spinbox_i32<"Source Channels", halp::range{1, NCHAN, 1}>
    {
    } src_channels;
    struct : halp::spinbox_i32<"Channel Offset", halp::range{0, NCHAN - 1, 0}>
    {
    } channel_offset;
    struct : halp::spinbox_i32<"Max Voices", halp::range{0, 256, 128}>
    {
      void update(Granola& self) { self.grains.resize(value); }
    } num_voices;
    struct : halp::toggle<"Continuous", halp::toggle_setup{.init = true}> {
      void update(Granola& self)
      {
        self.trigger = value;
      }
    }playing;
    struct : halp::impulse_button<"Trigger"> {
      /*void update(Granola& self)
      {
        self.trigger = true;
      }*/
    } trig;
    halp::midi_bus<"MIDI In", libremidi::message> midi;
    halp::toggle<"listening to MIDI", halp::toggle_setup{.init = true}> midi_gate;

    // --- Sound bank (multifile). Single "Sound" input: a folder loads all its
    // files (first selected), a file loads its folder's files (that one
    // selected). Held as a path string; the waveform widget and scan_folder
    // resolve file-vs-folder.
    halp::folder_port<"Sound"> sound_folder;
    // 0-based index into the bank files; driven by the custom picker widget
    // (which lists the folder UI-side). Random selection is the separate
    // Random toggle appended below.
    struct : halp::spinbox_i32<"Sound index", halp::range{0, 127, 0}>
    {
    } sound_index;
    // JSON key-zone mapping, resolved inside the folder (or absolute path):
    // { "zones": [ { "notes": [36,47], "sound": "kick.wav", "root": 40 } ] }
    halp::lineedit<"MIDI map", ""> midi_map;

    // Local-params mode: position, duration, their jitters and the window
    // coefs become per-file, persisted to granola-params.json in the folder
    // under the named set below.
    halp::toggle<"Local params"> local_params;
    halp::lineedit<"Params set", "default"> params_set;
    // When on, every grain picks a random bank sound (overrides Sound index).
    halp::toggle<"Random"> random;

  } inputs;

  struct
  {
    halp::variable_audio_bus<"Output", double> audio;
    halp::val_port<"Active Grains", int> active_grains;
    halp::val_port<"Current sound", std::string> current_sound;
  } outputs;

  struct ui;

  // Advertise the panel's full height so score opens the slot expanded rather
  // than at its short default (~300px), which clipped the taller panel and
  // made it look collapsed. Read by the avnd LayerFactory::recommendedHeight;
  // deliberately not `layout::graphics`/`compute`, so it is not a GPU node.
  struct layout
  {
    static constexpr double height() { return 700.; }
  };

  // --- Waveform UI messaging (score message bus, both directions).
  // The displayed sound is the one picked by the Sound index port ONLY;
  // MIDI-zone voices must not drive the display.
  struct processor_to_ui
  {
    std::vector<float> min_peaks, max_peaks;
    std::string name;
    float duration_s{};

    // Local-params write-back: when set, the UI writes these values to the
    // ports' document values through the same `set` hooks the gestures use,
    // so the inspector and the widgets reflect the restored file params.
    bool has_params{false};
    ParamSnapshot params{};
  };
  struct ui_to_processor
  {
    // For now just a refresh request, sent when the UI panel is (re)created;
    // the phase-2 gesture map will extend this.
  };

  std::function<void(processor_to_ui)> send_message;
  void process_message(const ui_to_processor&) { ui_refresh = true; }

  bool ui_refresh{true};
  std::string ui_sound_name;
  int64_t ui_sound_mtime{-1};

  // --- Local-params state (see handle_local_params in GranolaModel.cpp)
  std::map<std::string, ParamSnapshot> local_store; // RAM copy of current set
  std::string local_file;        // file the current port values belong to
  ParamSnapshot local_last{};    // last applied/captured snapshot
  bool local_active{false};      // toggle state seen last tick
  bool local_dirty{false};       // RAM store has edits not yet written
  bool local_save_inflight{false};
  long local_dirty_age{0};       // frames since last edit, for debounce
  int64_t params_adopted_mtime{-1};
  bool ui_send_params{false};    // attach params to the next UI message

  ParamSnapshot param_snapshot() const noexcept
  {
    return ParamSnapshot{
        inputs.pos,   inputs.dur, inputs.pos_j, inputs.dur_j,
        inputs.win_coefs.value.x, inputs.win_coefs.value.y};
  }

  void apply_params(const ParamSnapshot& p) noexcept
  {
    inputs.pos.value = p.pos;
    inputs.dur.value = p.dur;
    inputs.pos_j.value = p.pos_j;
    inputs.dur_j.value = p.dur_j;
    inputs.win_coefs.value = {p.win_x, p.win_y};
  }

  void handle_local_params(std::string_view cur_name, bool have_bank_sound);

  struct MidiVoice
  {
    bool active{false};
    double trigger_counter{0};
    uint8_t velocity{0};
  };

  GrainVec grains;

  std::atomic<bool> buf_soft_lock{false};
  bool alloccheck{false};
  bool trigger{false};
  long trigger_counter{0};
  int busyCount{0};
  bool useDefaultAmp{true};
  double maxAmp{0};
  float samplerate;
  float sampleinterval;
  double ms2samps;
  long numoutputs;

  std::array<MidiVoice, 128> midi_voices{};
  bool midi_active{false};   // true when at least one MIDI note is held
  int midi_pending_voice{-1}; // index of next voice to spawn (-1 = none pending)

  // --- Sound bank: scanned/decoded in a worker thread, swapped on the
  // processing thread. Active grains keep their sound via shared_ptr holds.
  SoundBank bank;
  long bank_scan_phase{0};
  bool bank_scan_inflight{false};

  struct scan_request
  {
    std::string folder;
    std::string map_file;
    std::string params_set;
    double rate;
    SoundBank previous;
    // Pending local-params save (whole current set), written to
    // granola-params.json before scanning so the fresh bank loads it back.
    std::shared_ptr<const std::map<std::string, ParamSnapshot>> save;
  };

  struct worker_t
  {
    std::function<void(std::shared_ptr<scan_request>)> request;

    static std::function<void(Granola&)> work(std::shared_ptr<scan_request> rq)
    {
      const bool had_save = bool(rq->save);
      const bool saved = had_save && write_params(rq->folder, rq->params_set, *rq->save);
      auto bank = scan_folder(
          rq->folder, rq->map_file, rq->params_set, rq->rate, rq->previous);
      return [bank = std::move(bank), had_save, saved](Granola& self) mutable {
        self.bank = std::move(bank);
        self.bank_scan_inflight = false;
        if(had_save)
        {
          self.local_save_inflight = false;
          if(!saved) // e.g. read-only folder: retry on a later rescan
            self.local_dirty = true;
        }
      };
    }
  } worker;

  void request_scan();

  // t_critical  lock; // is there an equivalent?

  using setup = halp::setup;
  void prepare(setup info);

  void resize(int n);
  void clear();

  using tick = halp::tick;
  void operator()(tick t);

  // Radius of the (angle, amount) -> (beta a, beta b) mapping for the window
  // coefs XY pad; shared with the window-shape widget in GranolaUi.hpp.
  static constexpr float wc_radius{64.};

private:
  rnd::pcg rd;
};

}
