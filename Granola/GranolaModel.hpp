#pragma once

#include "SoundBank.hpp"
#include "grain.hpp"
#include "utils.hpp"

#include <QDebug>

#include <atomic>
#include <functional>
#include <halp/audio.hpp>
#include <halp/controls.hpp>
#include <halp/mappers.hpp>
#include <halp/meta.hpp>
#include <halp/midi.hpp>
#include <halp/sample_accurate_controls.hpp>
#include <libremidi/message.hpp>
#include <rnd/random.hpp>

#include <ossia/detail/variant.hpp>

#include <boost/container/static_vector.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

#include <random>

namespace Granola
{

typedef std::vector<GranuGrain> GrainVec;

class Granola
{
public:
  halp_meta(name, "Granola")
  halp_meta(category, "Synths")
  halp_meta(
      description,
      "Generate granular audio from a sound file with controllable position, duration, "
      "pitch, density and jitter.")
  halp_meta(c_name, "granola")
  halp_meta(uuid, "38F9684D-54A6-4F48-91E4-3B251F0956EA")

  // Radius of the (angle, amount) -> (window a, b) mapping for the Window-coefs
  // XY pad; shared with the window-shape widget in GranolaUi.hpp.
  static constexpr float wc_radius{64.};

  // Grains made in prepare(): the most Max Voices can be
  static constexpr int max_voices = 256;

  //static const int NCHAN{8};

  struct ins
  {
    struct : halp::soundfile_port<"Sound">
    {
      halp_flag(waveform);
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
    halp::hslider_f32<"Duration", halp::range{0.01, 1., 0.1}> dur;
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
    // 0 = all the channels of the sound file
    struct : halp::spinbox_i32<"Source Channels", halp::range{0, NCHAN, 0}>
    {
    } src_channels;
    struct : halp::spinbox_i32<"Channel Offset", halp::range{0, NCHAN - 1, 0}>
    {
    } channel_offset;
    struct : halp::spinbox_i32<"Max Voices", halp::range{0, max_voices, 128}>
    {
      void update(Granola& self) { self.set_voice_count(value); }
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

    // --- Multifile bank. Appended at the end so pre-existing scenarios restore
    // all earlier ports positionally. The Sound port's folder is scanned; this
    // 0-based index (clipped) selects which file plays.
    struct : halp::spinbox_i32<"Sound index", halp::range{0, 4096, 0}>
    {
    } sound_index;
    // When on, every grain picks a random bank sound (overrides Sound index).
    halp::toggle<"Random"> random;
    // Master gate for the bank. Off (default) = the single Sound-port file
    // only (folder is not scanned); on = the multifile bank with index/random.
    // Also drives the UI: off shows an "enable" button, on shows the picker.
    halp::toggle<"Multiple sound support"> multi;

  } inputs;

  struct
  {
    halp::variable_audio_bus<"Output", double> audio;
    halp::val_port<"Active Grains", int> active_grains;
  } outputs;

  struct ui;

  // Advertise the panel height so score opens the temporal slot expanded to
  // fit the waveform + controls, rather than at its short default.
  struct layout
  {
    static constexpr double height() { return 420.; }
  };

  // --- UI messaging (score message bus). The host draws the waveform from the
  // sound file itself (halp::waveform_view), so what the processor sends is
  // small and of fixed size: sending it never allocates on the audio thread.

  // The read positions of the grains playing the current sound.
  struct playheads
  {
    static constexpr int max_heads = 32;
    uint8_t count;
    uint8_t amp[max_heads];  // window amplitude, 0-255
    uint16_t pos[max_heads]; // position in the current sound, 0-65535
  };
  // The sound that plays, for the picker.
  struct current_sound
  {
    int32_t index;  // in the bank; -1 when the Sound port's file plays
    char name[256]; // its file name
  };
  struct processor_to_ui
  {
    halp_flag(relocatable);
    ossia::variant<playheads, current_sound> message;
  };
  struct ui_to_processor
  {
    // Refresh request, sent when the UI panel is (re)created so a late panel
    // still gets the current sound.
  };
  std::function<void(processor_to_ui)> send_message;
  void process_message(const ui_to_processor&) { ui_refresh = true; }

  bool ui_refresh{true};
  // What the UI was last told: the bank sound, its index, the Sound port file
  int ui_sent_index{-2};
  const void* ui_sent_sound{};
  std::size_t ui_sent_path{};
  int ui_frame_counter{0}; // throttles the playheads
  bool ui_heads_shown{false};

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
  // Nothing of it is allocated nor freed on the processing thread: the worker
  // frees the banks that are replaced, and the sounds whose last hold a grain
  // let go of.
  SoundBank bank;
  long bank_scan_phase{0};
  bool bank_scan_inflight{false};
  std::size_t last_sound_path{}; // hash: resets captured grains when the file changes

  //! Shared by the processing thread and the worker; made in prepare(). The
  //! processing thread writes it only while no scan is in flight, the worker
  //! only during one, and the task queue orders the two.
  struct scan_state
  {
    std::string folder; // to scan; its capacity is reserved in prepare()
    double rate{};
    SoundBank last;     // the worker's last result: unchanged files are reused
    SoundBank retired;  // the bank the last result replaced
    std::vector<std::shared_ptr<const void>> released; // sounds grains let go of
  };
  std::shared_ptr<scan_state> scan;

  // Sounds a grain held last, kept until the next scan frees them
  boost::container::static_vector<std::shared_ptr<const void>, 256> released;
  void release(std::shared_ptr<const void>& hold) noexcept;

  // A scan's result, applied on the processing thread. Not a std::function:
  // it travels inline in the execution command, and the bank it replaces goes
  // back to the worker to be freed.
  struct apply_scan
  {
    SoundBank bank;
    explicit operator bool() const noexcept { return true; }
    void operator()(Granola& self) noexcept;
  };

  struct worker_t
  {
    std::function<void(std::shared_ptr<scan_state>)> request;

    static apply_scan work(std::shared_ptr<scan_state> st)
    {
      // What the processing thread is done with is freed here
      st->retired = {};
      st->released.clear();
      st->last = scan_folder(st->folder, st->rate, st->last);
      return apply_scan{st->last};
    }
  } worker;

  void request_scan();
  void send_playheads(int frames, const GrainSource& cur);

  // t_critical  lock; // is there an equivalent?

  using setup = halp::setup;
  void prepare(setup info);

  void resize(int n);
  //! Stops the grains past \p n, letting go of their sounds
  void set_voice_count(int n);
  void clear();

  using tick = halp::tick;
  void operator()(tick t);

private:
  rnd::pcg rd;
};

}
