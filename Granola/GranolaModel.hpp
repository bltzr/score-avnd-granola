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

  // Grains made in prepare(): the most Max Voices can be
  static constexpr int max_voices = 256;

  // What the Folder source lists
  static constexpr halp::static_string sound_extensions_lit{
      "wav w64 aif aiff aifc caf flac mp3 ogg opus m4a"};
  static constexpr std::string_view sound_extensions{sound_extensions_lit.value};

  // Shortest grain: below about this, the window cannot take its shape. Short
  // enough for pulsar-like timbres, whatever the length of the file.
  static constexpr double min_grain_seconds = 0.0005;

  // Output Channels: as many as a spatial audio system has loudspeakers.
  // NCHAN is how many of a sound's channels a grain reads.
  static constexpr int max_outputs = 1024;

  struct ins
  {
    struct : halp::soundfile_port<"Sound">
    {
      halp_flag(waveform);
      void update(Granola& self)
      {
        if(self.outputs.audio.request_channels)
          self.outputs.audio.request_channels(self.output_channels(this->channels()));
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
    // Each grain somewhere among the output channels (Output Channels): 0 in
    // all of them alike, 1 between two neighbouring ones (1-D VBAP), 2 in a
    // single one
    struct : halp::hslider_f32<"Spread", halp::range{0., 2., 0.}>
    {
      static constexpr std::string_view former_name() noexcept { return "Stereo Spread"; }
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
    // a Hann window (y is scaled by window_shape::radius)
    halp::xy_pad_f32<"Window coefs", halp::xy_range{.min = 0., .max = 1., .init = {0.5, 0.05}}>
        win_coefs;
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
    // The part of the sound that plays: Position, Duration and their
    // Deviations are fractions of it, Scan goes through it, Loop wraps in
    // it, and no grain reads outside of it
    halp::hslider_f32<"Sample Start", halp::range{0., 1., 0.}> sample_start;
    halp::hslider_f32<"Sample End", halp::range{0., 1., 1.}> sample_end;
    // How many outputs, 0 as many as the channels read: Spread places the
    // grains among them
    halp::spinbox_i32<"Output Channels", halp::range{0, max_outputs, 0}> out_channels;
    // How Scan goes on at the region's ends: around again, not past them, or
    // back. At the end itself with Wrap at end, else where a whole grain
    // still fits
    struct
    {
      halp__enum_combobox("Scan Mode", Loop, Straight, Loop, PingPong)
    } scan_mode;

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
    bool scanning;           // whether Scan moves Position, to:
    uint16_t scan;           // where it has moved it in the region, 0-65535
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
  //! Where Scan moves a position of the region, \p rel, to: in it
  float scanned(double rel) const noexcept;
  //! Where Scan wraps or turns: the region's end when grains wrap there
  //! (Wrap at end), else the last place where a whole grain fits
  double scan_last() const noexcept
  {
    return inputs.loopmode ? 1. : std::clamp(1. - (double)inputs.dur, 1e-3, 1.);
  }
  const void* scan_source{};
  bool scan_was_playing{};

  // A held note: its grains come at its own pace, as its pitch sets the
  // length of its grains
  struct MidiVoice
  {
    bool active{false};
    uint8_t velocity{0};
    double counter{0}; // samples since its last grain
  };
  // The notes with a grain to make, one bit each
  struct note_set
  {
    uint64_t bits[2]{};
    void set(int n) noexcept { bits[n >> 6] |= uint64_t(1) << (n & 63); }
    void reset(int n) noexcept { bits[n >> 6] &= ~(uint64_t(1) << (n & 63)); }
    void clear() noexcept { bits[0] = bits[1] = 0; }
    bool any() const noexcept { return bits[0] | bits[1]; }
    int first() const noexcept
    {
      return bits[0] ? std::countr_zero(bits[0]) : 64 + std::countr_zero(bits[1]);
    }
  };

  GrainVec grains;
  // The windows grains read, as tables: a few, as the window may change
  // while grains of the previous ones play (made in prepare())
  static constexpr int window_tables_count = 8;
  std::vector<window_table> window_tables;
  //! A table of \p w: one built already, else one no grain reads, built;
  //! none when every table is read by grains of other windows
  const window_table* table_for(const window_shape& w) noexcept;

  std::atomic<bool> buf_soft_lock{false};
  bool alloccheck{false};
  bool trigger{false};
  // Samples since the last grain, with the fraction of a sample the
  // interval had: grains at the rate Density sets, not one rounded to
  // whole samples
  double trigger_counter{0};
  // The Density Deviation of the next grain, drawn when a grain is made
  double density_factor{1.};
  // Gain as heard: it follows the control in a few milliseconds
  double gain_now{-1.};
  // In random folder mode, the length of the file the last grain read: the
  // next grain comes as that one's length and Density say
  double last_spawn_frames{};
  // Grains made since prepare(): how fast they come, for tests
  uint64_t grains_made{};
  int busyCount{0};
  bool useDefaultAmp{true};
  double maxAmp{0};
  float samplerate{48000.f}; // until prepare()
  float sampleinterval;
  double ms2samps;
  long numoutputs;

  std::array<MidiVoice, 128> midi_voices{};
  bool midi_active{false};   // true when at least one MIDI note is held
  note_set midi_pending;

  // The Sound file's data, to stop the grains of a replaced one: it is freed
  GrainSource last_sound{};

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
  //! The controls in their ranges, finite
  void sanitize_inputs() noexcept;
  //! The outputs for a sound of \p sound_channels: Output Channels, else as
  //! many as Channels reads of it; two at least with Spread
  int output_channels(int sound_channels) const noexcept;
  //! Stops the grains past \p n
  void set_voice_count(int n);
  void clear();

  using tick = halp::tick;
  void operator()(tick t);

private:
  // Another sequence for every instance
  static std::random_device& random_source()
  {
    static thread_local std::random_device d;
    return d;
  }
  rnd::pcg rd{random_source()};
};

}
