#pragma once

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
#include <halp/folder_combobox.hpp>
#include <halp/sample_accurate_controls.hpp>
#include <halp/sound_set.hpp>
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

  // What the Folder source lists
  static constexpr halp::static_string sound_extensions_lit{
      "wav w64 aif aiff aifc caf flac mp3 ogg opus m4a"};
  static constexpr std::string_view sound_extensions{sound_extensions_lit.value};

  // Shortest grain: below about this, the window cannot take its shape. Short
  // enough for pulsar-like timbres, whatever the length of the file.
  static constexpr double min_grain_seconds = 0.0005;

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
    // Each Deviation spreads its control per grain, as a normal distribution
    // of deviation d / 4: at 1, 95% of the grains within ±0.5 of it (of the
    // file for Position and Duration, relative for Density). Gain's lowers
    // some grains, in decibels.
    struct : halp::hslider_f32<"Position Deviation", halp::range{0., 1., 0.}>
    {
      static constexpr std::string_view former_name() noexcept { return "Position Jitter"; }
    } pos_j;

    // The five controls after each Deviation (then Jitter) were its Jitter
    // Range, which only scaled it. Their places are kept, so that documents
    // restore the ports after them; each is another kind of control than it
    // was, so that a document from then rebuilds it, and upgrade_value() gives
    // it the value that changes nothing. Their names are new: presets of then
    // skip them.

    // Position moves through the sound by itself, at this speed: 1 goes
    // through it in its own duration, -1 backwards, 0 stays at Position
    struct : halp::hslider_f32<"Scan", halp::range{-2., 2., 0.}>
    {
      static float upgrade_value(float) { return 0.f; }
    } scan;
    halp::hslider_f32<"Duration", halp::range{0.00000001, 1., 0.1}> dur;
    struct : halp::hslider_f32<"Duration Deviation", halp::range{0., 1., 0.}>
    {
      static constexpr std::string_view former_name() noexcept { return "Duration Jitter"; }
    } dur_j;
    // Each grain somewhere else between the output channels: 0 in all of
    // them alike, 1 anywhere
    struct : halp::hslider_f32<"Stereo Spread", halp::range{0., 1., 0.}>
    {
      static float upgrade_value(float) { return 0.f; }
    } spread;
    struct : halp::knob_f32<"Pitch", halp::range{0.000001, 10., 1.}>
    {
      using mapper = halp::inverse_mapper<halp::pow_mapper<4>>;
    } rate;
    struct : halp::vslider_f32<"Pitch Deviation", halp::range{0., 1., 0.}>
    {
      static constexpr std::string_view former_name() noexcept { return "Pitch Jitter"; }
    } rate_j;
    // Pitch Deviation in whole semitones, the same at every pitch: at 1, 95% of
    // the grains within an octave; else as a ratio added to Pitch
    struct : halp::toggle<"Pitch Deviation in Semitones">
    {
      static constexpr std::string_view former_name() noexcept
      {
        return "Pitch Jitter in Semitones";
      }
      static float upgrade_value(float) { return 0.f; }
    } rate_j_semitones;
    halp::toggle<"Reverse"> reverse;
    struct : halp::accurate<halp::knob_f32<"Density", halp::range{0., 256., 1.}>>
    {
      using mapper = halp::log_mapper<std::ratio<99, 100>>;
    } density;
    struct : halp::vslider_f32<"Density Deviation", halp::range{0., 1., 0.}>
    {
      static constexpr std::string_view former_name() noexcept { return "Density Jitter"; }
    } dens_j;
    // How likely a grain plays backwards; Reverse flips them all
    struct : halp::knob_f32<"Reverse Chance", halp::range{0., 1., 0.}>
    {
      static float upgrade_value(float) { return 0.f; }
    } reverse_chance;
    halp::knob_f32<"Gain", halp::range{.min = 0., .max = 4., .init = 0.5}> gain;
    struct : halp::vslider_f32<"Gain Deviation", halp::range{0., 1., 0.}>
    {
      static constexpr std::string_view former_name() noexcept { return "Gain Jitter"; }
    } gain_j;
    // Unused: free for a control to come, of another kind than a knob
    halp::knob_f32<"Gain Jitter Range", halp::range{0., 1., 1.}> gain_j_r;
    // A bell by default rather than a flat window: Beta(3.3, 3.3), close to
    // a Hann window (y is scaled by wc_radius)
    struct : halp::xy_pad_f32<"Window coefs", halp::range{0.f, 1.f, 0.f}>
    {
      static constexpr auto range()
      {
        struct
        {
          float min{0.f}, max{1.f};
          halp::xy_type<float> init{0.5f, 0.05f};
        } r;
        return r;
      }
    } win_coefs;
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

    // --- Where the sound comes from. Appended at the end so pre-existing
    // scenarios restore all earlier ports positionally.
    struct
    {
      halp__enum_combobox("Source", File, File, Folder)
    } source;
    // Folder source: the host lists, decodes and watches the folder
    struct : halp::sound_set<"Sounds">
    {
      static constexpr std::string_view extensions() { return sound_extensions; }
    } sounds;
    // The file of the folder that plays: by name, so that adding files keeps
    // it, or by its index in the folder when it gets a number (automation,
    // cables), clipped to the last file
    halp::folder_combobox<"Sound in folder", "Sounds", sound_extensions_lit> sound_file;
    // Every grain plays a random file of the folder instead
    halp::toggle<"Random"> random;
    // Scan goes on where it was when another file of the folder plays,
    // rather than from Position again
    halp::toggle<"Scan Across Sounds"> scan_across;

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
    uint16_t scan;           // where Scan has moved Position, 0-65535
    uint8_t amp[max_heads];  // window amplitude, 0-255
    uint16_t pos[max_heads]; // position in the current sound, 0-65535
  };
  // The sound that plays from the folder, and whether it could be decoded
  struct current_sound
  {
    int32_t index; // in the folder; -1 when the Sound port's file plays
    halp::sound_status status;
    char name[240]; // its path in the folder
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
  // What the UI was last told: the folder index and its state
  int ui_sent_index{-2};
  halp::sound_status ui_sent_status{};
  uint64_t ui_sent_generation{};
  int ui_frame_counter{0}; // throttles the playheads
  bool ui_heads_shown{false};

  // Where Scan has moved Position, as a part of the sound
  double scan_phase{};
  const void* scan_source{};
  bool scan_was_playing{};

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

  std::size_t last_sound_path{}; // hash: stops the grains of a replaced Sound file

  // The folder file the Sound in folder names or indexes, looked up again
  // only when the folder or the value changes
  int folder_index{-1};
  uint64_t folder_index_generation{};
  std::size_t folder_index_name{};
  int find_folder_index();
  int random_ready_index();

  void send_playheads(int frames, const GrainSource& cur);

  // t_critical  lock; // is there an equivalent?

  using setup = halp::setup;
  void prepare(setup info);

  void resize(int n);
  //! Stops the grains past \p n
  void set_voice_count(int n);
  void clear();

  using tick = halp::tick;
  void operator()(tick t);

private:
  rnd::pcg rd;
};

}
