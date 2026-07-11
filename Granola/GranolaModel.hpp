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
    struct : halp::soundfile_port<"Sound">
    {
      halp_flag(waveform);
      // Reload automatically when the file changes on disk: lets Granola act
      // as a live buffer player for the FluCoMa working folder (e.g. point it
      // at nmf_resynth.wav and re-run BufNMF while playing).
      halp_flag(file_watch);
      void update(Granola& self)
      {
        self.outputs.audio.request_channels(this->channels());
        //if (self.inputs.playing) self.trigger = true;
        //qDebug() << "sound" << self.trigger;
      }
    } sound;
    //halp::soundfile_port<"Window", double> win; // not supported yet
    //halp::range_slider_f32<"In", halp::range_slider_range{-10, 100, {5, 20}}> ta_range;
    halp::hslider_f32<"Position", halp::range{0.00000001, 1., 0.00000001}> pos;
    halp::hslider_f32<"Position Jitter", halp::range{0., 1., 0.}> pos_j;
    halp::knob_f32<"Position Jitter Range", halp::range{0., 1., 1.}> pos_j_r;
    halp::hslider_f32<"Duration", halp::range{0.00000001, 1., 0.1}> dur;
    halp::hslider_f32<"Duration Jitter", halp::range{0., 1., 0.}> dur_j;
    halp::knob_f32<"Duration Jitter Range", halp::range{0., 1., 1.}> dur_j_r;
    struct : halp::knob_f32<"Pitch", halp::range{0.000001, 10., 1.}>
    {
      using mapper = halp::inverse_mapper<halp::pow_mapper<4>>;
    } rate;
    halp::vslider_f32<"Pitch Jitter", halp::range{0., 1., 0.}> rate_j;
    halp::knob_f32<"Pitch Jitter Range", halp::range{0., 1., 1.}> rate_j_r;
    halp::toggle<"Reverse"> reverse;
    struct : halp::accurate<halp::knob_f32<"Density", halp::range{0., 256., 1.}>>
    {
      using mapper = halp::log_mapper<std::ratio<99, 100>>;
    } density;
    halp::vslider_f32<"Density Jitter", halp::range{0., 1., 0.}> dens_j;
    halp::hslider_f32<"Density Jitter Range", halp::range{0., 50., 1.}> dens_j_r;
    halp::knob_f32<"Gain", halp::range{.min = 0., .max = 4., .init = 0.5}> gain;
    halp::vslider_f32<"Gain Jitter", halp::range{0., 1., 0.}> gain_j;
    halp::knob_f32<"Gain Jitter Range", halp::range{0., 1., 1.}> gain_j_r;
    halp::xy_pad_f32<"Window coefs", halp::range{0.f, 1.f, 0.f}> win_coefs;
    struct
    {
      halp__enum_combobox("Interpolation mode", Cubic, None, Linear, Cubic)
    } interp_type;
    struct
    {
      halp__enum_combobox("Window mode", Beta, Beta, Cos, Kuma)
    } window_mode;
    halp::toggle<"Loop"> loopmode;
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

    // --- Sound bank (multifile). New ports are appended at the end so saved
    // scenarios keep their existing port ids.
    halp::folder_port<"Sound folder"> sound_folder;
    struct : halp::spinbox_i32<"Sound index", halp::range{0, 127, 0}>
    {
    } sound_index;
    // JSON key-zone mapping, resolved inside the folder (or absolute path):
    // { "zones": [ { "notes": [36,47], "sound": "kick.wav", "root": 40 } ] }
    halp::lineedit<"MIDI map", ""> midi_map;

  } inputs;

  struct
  {
    halp::variable_audio_bus<"Output", double> audio;
    halp::val_port<"Active Grains", int> active_grains;
    halp::val_port<"Current sound", std::string> current_sound;
  } outputs;

  struct ui;

  // --- Waveform UI messaging (score message bus, both directions).
  // The displayed sound is the one picked by the Sound index port ONLY;
  // MIDI-zone voices must not drive the display.
  struct processor_to_ui
  {
    std::vector<float> min_peaks, max_peaks;
    std::string name;
    float duration_s{};
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
    double rate;
    SoundBank previous;
  };

  struct worker_t
  {
    std::function<void(std::shared_ptr<scan_request>)> request;

    static std::function<void(Granola&)> work(std::shared_ptr<scan_request> rq)
    {
      auto bank = scan_folder(rq->folder, rq->map_file, rq->rate, rq->previous);
      return [bank = std::move(bank)](Granola& self) mutable {
        self.bank = std::move(bank);
        self.bank_scan_inflight = false;
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
