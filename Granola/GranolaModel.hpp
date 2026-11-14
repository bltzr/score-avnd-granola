#pragma once
#include "grain.hpp"

#include <ossia/detail/variant.hpp>

#include <halp/audio.hpp>
#include <halp/controls.hpp>
#include <halp/folder_combobox.hpp>
#include <halp/grid.hpp>
#include <halp/mappers.hpp>
#include <halp/meta.hpp>
#include <halp/midi.hpp>
#include <halp/sample_accurate_controls.hpp>
#include <halp/sound_set.hpp>
#include <halp/soundfile_port.hpp>
#include <libremidi/message.hpp>
#include <rnd/random.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <random>
#include <vector>

namespace Granola
{
class Granola
{
public:
  halp_meta(name, "Granola")
  halp_meta(category, "Synths")
  halp_meta(
      description,
      "Generate granular audio from a sound file with controllable position, duration, "
      "pitch, density, their deviations, and where they play.")
  halp_meta(c_name, "granola")
  // Another object than Granola (v1), whose uuid documents made with it
  // name: its ports and its grains are not those
  halp_meta(uuid, "4F43C108-4972-44CA-8A75-5804CA072FE2")

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
  // max_channels is how many of a sound's channels a grain reads.
  static constexpr int max_outputs = 1024;

  // What Density Deviation multiplies Density by: within this many octaves
  static constexpr double max_density_octaves = 4.;
  static constexpr double max_density_factor = 16.; // 2^max_density_octaves

  // Quantize: no grid at first, else the grids of score's grid chooser
  static constexpr halp::grid_setup quantize_setup{
      .units = halp::grid_units::musical, .init = {}, .max_bars = 16, .max_seconds = 8.};

  struct ins
  {
    struct : halp::soundfile_port<"Sound">
    {
      halp_meta(description, "The sound file the grains play when Source is File.")
      halp_flag(waveform);
      void update(Granola& self)
      {
        if(self.outputs.audio.request_channels)
          self.outputs.audio.request_channels(self.output_channels(this->channels()));
      }
    } sound;
    struct : halp::hslider_f32<"Position", halp::range{0.00000001, 1., 0.00000001}>
    {
      halp_meta(
          description,
          "Where in the sound grains start, from the region's start (0) to its end (1).")
    } pos;
    // Each Deviation spreads its control per grain, as a normal distribution
    // of deviation d / 4: at 1, 95% of the grains within ±0.5 of it, of the
    // region for Position and Duration. Density's and Gain's are their own,
    // below.
    struct : halp::hslider_f32<"Position Deviation", halp::range{0., 1., 0.}>
    {
      halp_meta(
          description,
          "How far each grain's start strays at random around Position, 0 for none.")
    } pos_j;
    // Position moves through the sound by itself, at this speed: 1 goes
    // through it in its own duration, -1 backwards, 0 stays at Position
    struct : halp::hslider_f32<"Scan", halp::range{-2., 2., 0.}>
    {
      halp_meta(
          description,
          "Moves Position through the sound by itself: 1 at the sound's own speed, "
          "negative backwards, 0 to stay.")
    } scan;
    // The length of the grains in Region mode (Duration Mode)
    struct : halp::hslider_f32<"Duration", halp::range{0.00000001, 1., 0.1}>
    {
      halp_meta(
          description,
          "How long each grain is in Region mode, as a part of the region, from almost "
          "nothing to all of it (1).")
    } dur;
    // Region mode: N(0, d / 4) of the region, as the other Deviations. Time
    // mode: times 2^N(0, d / 2), as Density Deviation, so that a grain of
    // 1/16 on a long sound strays by notes rather than by minutes: at 1, 95%
    // of the grains between half and twice Duration Time, as many shorter as
    // longer.
    struct : halp::hslider_f32<"Duration Deviation", halp::range{0., 1., 0.}>
    {
      halp_meta(
          description,
          "How much the length of each grain varies at random around its Duration, 0 "
          "for none.")
    } dur_j;
    // Each grain somewhere among the output channels (Output Channels): 0 in
    // all of them alike, 1 between two neighbouring ones (1-D VBAP), 2 in a
    // single one
    struct : halp::hslider_f32<"Spread", halp::range{0., 2., 1.}>
    {
      halp_meta(
          description,
          "Where grains sit among the outputs: 0 in all alike, 1 panned between two "
          "neighbours, 2 in a single one.")
    } spread;
    struct : halp::knob_f32<"Pitch", halp::range{0.000001, 10., 1.}>
    {
      halp_meta(
          description,
          "How fast grains read the sound: 1 as recorded, 2 an octave up, 0.5 an octave "
          "down.")
      using mapper = halp::inverse_mapper<halp::pow_mapper<4>>;
    } rate;
    struct : halp::vslider_f32<"Pitch Deviation", halp::range{0., 1., 0.}>
    {
      halp_meta(
          description,
          "How much the pitch of each grain varies at random around Pitch, 0 for none.")
    } rate_j;
    // Pitch Deviation in whole semitones, the same at every pitch: at 1, 95% of
    // the grains within an octave; else as a ratio added to Pitch
    struct : halp::toggle<"Pitch Deviation in Semitones">
    {
      halp_meta(
          description,
          "Makes Pitch Deviation pick whole semitones instead of any pitch in between.")
    } rate_j_semitones;
    // Grains per grain length. Sample-accurate: automation and cables change
    // the pace at the sample their values come at, not a block later. 0
    // makes no grains, which a Trigger or a note still makes; any Density
    // above 0 does, from the sample it comes at.
    struct : halp::accurate<halp::knob_f32<"Density", halp::range{0., 256., 1.}>>
    {
      halp_meta(
          description,
          "How often grains start: 1 starts one as the last one ends, 2 twice as often, "
          "0 none.")
      using mapper = halp::log_mapper<std::ratio<99, 100>>;
    } density;
    // Multiplies Density per grain by 2^N(0, d / 2): at 1, 95% of the
    // intervals between half and twice as long, as many shorter as longer
    struct : halp::vslider_f32<"Density Deviation", halp::range{0., 1., 0.}>
    {
      halp_meta(
          description,
          "How irregular the time between grains is, 0 for a steady pulse.")
    } dens_j;
    // How likely a grain plays backwards: 1 all of them
    struct : halp::knob_f32<"Reverse Chance", halp::range{0., 1., 0.}>
    {
      halp_meta(
          description,
          "How likely each grain is to play backwards, from never (0) to always (1).")
    } reverse_chance;
    struct : halp::knob_f32<"Gain", halp::range{.min = 0., .max = 4., .init = 0.5}>
    {
      halp_meta(description, "The output level, 1 as loud as the sound itself.")
    } gain;
    // Lowers some grains, in decibels: down to -24 dB at 1, never louder
    struct : halp::vslider_f32<"Gain Deviation", halp::range{0., 1., 0.}>
    {
      halp_meta(
          description,
          "How much quieter some grains are at random, down to -24 dB at 1, never "
          "louder.")
    } gain_j;
    // A bell by default rather than a flat window: Beta(3.3, 3.3), close to
    // a Hann window (y is scaled by window_shape::radius)
    struct : halp::xy_pad_f32<"Window coefs", halp::xy_range{.min = 0., .max = 1., .init = {0.5, 0.05}}>
    {
      halp_meta(
          description,
          "The fade in and out of each grain: up for a sharper peak, left or right to "
          "move it earlier or later.")
    } win_coefs;
    struct
    {
      halp_meta(
          description,
          "How the sound is read between its samples: None is grittier, Linear "
          "smoother, Cubic smoothest.")
      halp__enum_combobox("Interpolation mode", Cubic, None, Linear, Cubic)
    } interp_type;
    struct
    {
      halp_meta(
          description,
          "The family of fade shapes of each grain: Beta, Cos or Kuma.")
      halp__enum_combobox("Window mode", Beta, Beta, Cos, Kuma)
    } window_mode;
    struct : halp::toggle<"Loop">
    {
      halp_meta(
          description,
          "Lets a grain that reaches the end of the region go on from its start.")
    } loopmode;
    // 0 = all the channels of the sound file
    struct : halp::spinbox_i32<"Source Channels", halp::range{0, max_channels, 0}>
    {
      halp_meta(
          description,
          "How many channels of the sound the grains read, 0 for all of them.")
    } src_channels;
    struct : halp::spinbox_i32<"Channel Offset", halp::range{0, max_channels - 1, 0}>
    {
      halp_meta(
          description,
          "The first channel of the sound the grains read, 0 for the first one.")
    } channel_offset;
    struct : halp::spinbox_i32<"Max Voices", halp::range{0, max_voices, 128}>
    {
      halp_meta(description, "The most grains that may play at once, from 0 to 256.")
      void update(Granola& self) { self.set_voice_count(value); }
    } num_voices;
    struct : halp::toggle<"Continuous", halp::toggle_setup{.init = true}>
    {
      halp_meta(description, "Plays grains by itself, as often as Density says.")
      // Started: a grain at once, the next one an interval later
      void update(Granola& self)
      {
        if(value && !self.clock_started)
        {
          self.trigger = true;
          self.trigger_counter = 0.;
        }
        else if(!value)
          self.trigger = false;
        self.clock_started = value;
      }
    } playing;
    struct : halp::impulse_button<"Trigger">
    {
      halp_meta(description, "Plays one grain each time it is pressed.")
    } trig;
    struct : halp::midi_bus<"MIDI In", libremidi::message>
    {
      halp_meta(
          description,
          "MIDI notes that play grains, set their pitch or place, or pick the file, as "
          "On MIDI and Sound Order say.")
    } midi;
    // What the notes of MIDI In do. Nothing: no grain, no pitch. Trigger: a
    // held note plays grains at Pitch, at its own pace. Trigger and set
    // pitch: at its pitch, at the pace its pitch makes. Set pitch: no grain
    // of its own; the grains of Continuous and Trigger at the pitch of the
    // note pressed last of those held, and once all are released at the
    // pitch of the last one, until a file is picked by hand or Sound Order
    // changes; at Pitch before any note. A note's pitch is relative to note
    // 60. Set position: no grain of their own either; Position of the grains
    // of Continuous and Trigger is the note's place, as Set pitch's pitch
    // (latched alike). Trigger and set position: a held note plays grains at
    // Pitch from its own place: slices. A note's place: the region cut in a
    // slice a key of Low note to High note, Low note's at the region's start,
    // High note's the last one, the grain starting at the start of its slice;
    // a note below or above the range plays the slice of its end. The range
    // sets places and files, never the pitch, which stays relative to 60.
    // Position and Scan's travel are offsets from it, which a note does not
    // reset, and Position Deviation spreads around that. Past the region's
    // end, or before its start, the place goes on as Scan Mode says, at the
    // region's ends themselves since the slices span all of it: around
    // (Loop), back (Ping-pong) or held there (Straight). Sound Order's By MIDI note
    // picks files from the notes of the same range in every one: with the
    // slices, the same key picks both the file and the slice.
    struct
    {
      halp_meta(
          description,
          "What MIDI notes do: nothing; play grains while held, at Pitch or at their "
          "own pitch; set the pitch or the place of the other grains; or play grains "
          "from their own place.")
      static clang_buggy_consteval auto name() { return "On MIDI"; }
      enum enum_type
      {
        Nothing,
        Trigger,
        TriggerPitch,
        SetPitch,
        SetPosition,
        TriggerPosition
      } value = TriggerPitch;
      enum widget
      {
        combobox
      };
      struct range
      {
        std::pair<std::string_view, enum_type> values[6]{
            {"Nothing", Nothing},
            {"Trigger", Trigger},
            {"Trigger and set pitch", TriggerPitch},
            {"Set pitch", SetPitch},
            {"Set position", SetPosition},
            {"Trigger and set position", TriggerPosition}};
        enum_type init = TriggerPitch;
      };
      operator enum_type&() noexcept { return value; }
      operator const enum_type&() const noexcept { return value; }
      auto& operator=(enum_type t) noexcept
      {
        value = t;
        return *this;
      }
    } on_midi;
    // How much a note's velocity sets the level of its grains: the grains a
    // note plays, and with Set pitch and Set position the grains of
    // Continuous and Trigger, at the velocity of the note they go on with.
    // Level 1 - amount (1 - (velocity / 127)^2), squared as loudness goes:
    // velocity 127 at full level whatever the amount; at 0 velocity is
    // ignored; at 1, the default, as notes always set it before. Each grain
    // keeps the level it starts with: no click.
    struct : halp::knob_f32<"Velocity", halp::range{0., 1., 1.}>
    {
      halp_meta(
          description,
          "How much MIDI velocity sets the level of the grains: 0 ignores it, 1 follows "
          "it fully.")
    } velocity;
    // Every grain lasts as long whatever its pitch (Pitch, its Deviation, a
    // note, Backwards), in either Duration Mode, reading as much more of the
    // sound as it goes faster, and the grains come at a pace their pitch does
    // not change: notes of any pitch play theirs at the same instants. A
    // grain still reads no further than the region: past it, it ends sooner,
    // or with Wrap at end goes around it once at most.
    struct : halp::toggle<"Keep time">
    {
      halp_meta(
          description,
          "Keeps the length of the grains and their pace the same at any pitch.")
    } keep_time;

    // Where the sound comes from. Folder with no folder set reads the Sound
    // file, as the file picked in its own folder: switching to Folder plays on
    // what played
    struct
    {
      halp_meta(
          description,
          "Where the sound comes from: one sound file, or a folder of them.")
      halp__enum_combobox("Source", File, File, Folder)
    } source;
    // Folder source: the host lists, decodes and watches the folder
    struct : halp::sound_set<"Sounds">
    {
      halp_meta(
          description,
          "The folder of sound files the grains play when Source is Folder.")
      static constexpr std::string_view extensions() { return sound_extensions; }
    } sounds;
    // The file of the folder that plays: by name, so that adding files keeps
    // it, or by its index in the folder when it gets a number (automation,
    // cables), clipped to the last file
    struct : halp::folder_combobox<"Sound in folder", "Sounds", sound_extensions_lit>
    {
      halp_meta(
          description,
          "The file of the folder that plays, by its name or by its number in the "
          "folder.")
    } sound_file;
    // Which file of the folder each grain plays. Single file: the one Sound
    // in folder picks. Random: any, each grain. Shuffle: every file once, in
    // a random order, then again in another, never the same file twice in a
    // row. Alphabetical: the next one in the folder's order (as the host
    // lists it, in natural order), around again after the last. Ping-pong:
    // to the last and back, the ends once. Random walk: a neighbour, mostly
    // the next or previous one, sometimes the one after, back from the ends.
    // By MIDI note: a note of MIDI In picks it, whatever On MIDI says: the
    // keys of Low note to High note spread over the files, Low note the
    // first, High note the last, the others the nearest file to their place
    // between (several keys a file when there are fewer files than keys,
    // files between keys when there are more); a note out of the range
    // picks the file of its end: the grains a note plays read
    // its file; the grains of Continuous and Trigger read the file of the
    // note pressed last of the held ones, and once all are released the file
    // of the last one, until a file is picked by hand in Sound in folder or
    // Sound Order changes (starting and stopping keep it). Before any note,
    // the picked file. The other sequences are the folder's, one for every
    // grain, MIDI notes too: a chord goes through the files rather than
    // every note of it playing the same one. Files not decoded yet, or which
    // cannot be, are skipped.
    struct
    {
      halp_meta(
          description,
          "Which file of the folder each grain plays: the picked one, any at random, "
          "each once shuffled, in order, back and forth, a neighbour, or the one of the "
          "MIDI note.")
      static clang_buggy_consteval auto name() { return "Sound Order"; }
      enum enum_type
      {
        Single,
        Random,
        Shuffle,
        Alphabetical,
        PingPong,
        RandomWalk,
        ByNote
      } value = Single;
      enum widget
      {
        combobox
      };
      struct range
      {
        std::pair<std::string_view, enum_type> values[7]{
            {"Single file", Single},     {"Random", Random},
            {"Shuffle", Shuffle},        {"Alphabetical", Alphabetical},
            {"Ping-pong", PingPong},     {"Random walk", RandomWalk},
            {"By MIDI note", ByNote}};
        enum_type init = Single;
      };
      operator enum_type&() noexcept { return value; }
      operator const enum_type&() const noexcept { return value; }
      auto& operator=(enum_type t) noexcept
      {
        value = t;
        return *this;
      }
    } order;
    // Scan with the folder's files. Restart: from Position again when another
    // file is picked. Continue: on where it was. Per file: each file has its
    // place, which moves only when a grain reads that file, by as much as
    // Scan went since the grain before: a file a grain in ten reads goes ten
    // times slower, and its grains go on where its last ones were. With
    // Single file, every grain reads the picked file: it goes on as with
    // Continue, and a file picked again goes on where it was.
    struct
    {
      halp_meta(
          description,
          "With a folder, where Scan goes on when another file plays: from Position "
          "again, where it was, or where that file was left.")
      static clang_buggy_consteval auto name() { return "Scan in folder"; }
      enum enum_type
      {
        Restart,
        Continue,
        PerFile
      } value = Restart;
      enum widget
      {
        combobox
      };
      struct range
      {
        std::pair<std::string_view, enum_type> values[3]{
            {"Restart", Restart}, {"Continue", Continue}, {"Per file", PerFile}};
        enum_type init = Restart;
      };
      operator enum_type&() noexcept { return value; }
      operator const enum_type&() const noexcept { return value; }
      auto& operator=(enum_type t) noexcept
      {
        value = t;
        return *this;
      }
    } scan_folder;
    // The part of the sound that plays: Position and its Deviation, and in
    // Region mode Duration and its Deviation, are fractions of it, Scan goes
    // through it, Loop wraps in it, and no grain reads outside of it
    struct : halp::hslider_f32<"Sample Start", halp::range{0., 1., 0.}>
    {
      halp_meta(
          description,
          "Where the part of the sound that plays begins, 0 for the start of the sound.")
    } sample_start;
    struct : halp::hslider_f32<"Sample End", halp::range{0., 1., 1.}>
    {
      halp_meta(
          description,
          "Where the part of the sound that plays ends, 1 for the end of the sound.")
    } sample_end;
    // How many outputs, 0 as many as the channels read: Spread places the
    // grains among them
    struct : halp::spinbox_i32<"Output Channels", halp::range{0, max_outputs, 2}>
    {
      halp_meta(
          description,
          "How many outputs the grains are spread over, 0 for as many as the sound "
          "channels read.")
    } out_channels;
    // How Scan goes on at the region's ends: around again, not past them, or
    // back. At the end itself with Wrap at end, else where a whole grain
    // still fits
    struct
    {
      halp_meta(
          description,
          "What Scan does at the end of the region: Loop starts over, Straight stops "
          "there, PingPong turns back.")
      halp__enum_combobox("Scan Mode", Loop, Straight, Loop, PingPong)
    } scan_mode;
    // The keys whose notes span the region (On MIDI's slices) and the
    // folder's files (Sound Order's By MIDI note), whichever is higher
    struct : halp::spinbox_i32<"Low note", halp::range{0, 127, 48}>
    {
      halp_meta(
          description,
          "The MIDI note that plays from the start of the region and picks the first "
          "file of the folder.")
    } low_note;
    struct : halp::spinbox_i32<"High note", halp::range{0, 127, 72}>
    {
      halp_meta(
          description,
          "The MIDI note that plays the last part of the region and picks the last file "
          "of the folder.")
    } high_note;
    // What Duration is: Region, a part of the region (Duration); Time, a
    // time (Duration Time). Each mode plays its own control: switching
    // writes neither.
    struct
    {
      halp_meta(
          description,
          "Whether grains last a part of the region (Duration) or a time, in seconds or "
          "in notes at the tempo (Duration Time).")
      halp__enum_combobox("Duration Mode", Region, Region, Time)
    } dur_mode;
    // Time mode: the length of sound a grain reads at speed 1, which is how
    // long it lasts then, and with Keep time at any speed. Seconds, or a
    // note value, which the host gives in seconds at the tempo of each tick:
    // the grains made after a change of tempo follow it, the one playing
    // keeps its length. 30 s holds four whole notes down to 32 BPM.
    struct : halp::time_chooser<"Duration Time", halp::range{min_grain_seconds, 30., 0.1}>
    {
      halp_meta(
          description,
          "How long each grain is in Time mode: seconds, or a note value that follows "
          "the tempo.")
    } dur_time;
    // Grains start on this grid only: the grains Continuous, Trigger and the
    // notes of MIDI In make wait for its next point, one at a point, and a
    // chord's from the same point on. Continuous then counts its pace in
    // points of the grid: Density sets how many there are between two
    // grains, the nearest whole number, one at least, so that a grain every
    // eighth note is one every eighth even as the tempo moves. None: grains
    // start when they are due, at any sample.
    struct : halp::grid_chooser<"Quantize", quantize_setup>
    {
      halp_meta(
          description,
          "Starts grains only on this grid: a time, a note value at the tempo or bars; "
          "None starts them whenever they are due.")
    } quantize;

  } inputs;

  struct
  {
    struct : halp::variable_audio_bus<"Output", double>
    {
      halp_meta(description, "The grains, on as many channels as Output Channels says.")
    } audio;
    struct : halp::val_port<"Active Grains", int>
    {
      halp_meta(description, "How many grains are playing.")
    } active_grains;
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
    // Positions and levels in [0, 1] as integers of this many steps
    static constexpr double pos_scale = 65535.;
    static constexpr double amp_scale = 255.;
    uint8_t count;
    bool scanning;           // whether Scan moves Position, to:
    uint16_t scan;           // where it has moved it in the region
    float tempo;             // of the tick, for the note values the panel shows
    uint8_t amp[max_heads];  // window amplitude
    uint16_t pos[max_heads]; // position in the current sound
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
  float ui_sent_tempo{};

  // Where Scan has moved Position, as a part of the sound
  double scan_phase{};
  //! Where Scan moves a position of the region, \p rel, to: in it
  float scanned(double rel) const noexcept;
  //! Where Scan wraps or turns: the region's end when grains wrap there
  //! (Wrap at end), else the last place where a whole grain fits
  double scan_last() const noexcept
  {
    return inputs.loopmode ? 1. : std::clamp(1. - grain_in_region(), 1e-3, 1.);
  }
  //! Duration Mode is Time
  bool timed() const noexcept
  {
    return inputs.dur_mode.value == decltype(inputs.dur_mode.value)::Time;
  }
  //! A grain at speed 1, as a part of the region: Duration, or Duration Time
  //! in the shown sound, at most all of it
  double grain_in_region() const noexcept
  {
    return timed() ? time_in_region : (double)inputs.dur;
  }
  // Duration Time as a part of the region of the shown sound, at most 1
  double time_in_region{};
  const void* scan_source{};
  bool scan_was_playing{};

  // A held note: its grains come at its own pace, as its pitch sets the
  // length of its grains
  struct MidiVoice
  {
    bool active{false};
    uint8_t velocity{0};
    double counter{0}; // samples since its last grain
    double grid_points{0}; // with Quantize, points of the grid since then
    uint64_t pressed{}; // when, as a count of the notes pressed
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

  std::vector<Grain> grains;
  // In a block: the sample from which each voice is free, the samples of a
  // grain per channel, the part of the outputs every one shares per channel,
  // and the gain (made in prepare())
  int max_block{512};
  std::vector<int> free_from;
  std::vector<double> scratch;
  std::vector<double> shared;
  std::vector<double> block_gain;
  //! Where \p g is among \p places outputs, as Spread says
  void spread_grain(Grain& g, int places) noexcept;
  // The windows grains read, as tables: a few, as the window may change
  // while grains of the previous ones play (made in prepare())
  static constexpr int window_tables_count = 8;
  std::vector<window_table> window_tables;
  //! A table of \p w: one built already, else one no grain reads, built;
  //! none when every table is read by grains of other windows
  const window_table* table_for(const window_shape& w) noexcept;


  bool trigger{false};
  // Whether Continuous was turned on last, as its update() saw it
  bool clock_started{false};
  // Samples since the last grain, with the fraction of a sample the
  // interval had: grains at the rate Density sets, not one rounded to
  // whole samples
  double trigger_counter{0};
  // Quantize: the frames run before this tick, the frame of the last grid
  // point; the grains due may start since that point, until they have;
  // the grid points since the last grain of Continuous and Trigger
  int64_t frames_run{};
  // The tempo of the last tick
  double tempo_now{120.};
  int64_t grid_point_at{std::numeric_limits<int64_t>::min()};
  bool grid_open{};
  double grid_points{};
  //! Quantize in its units' ranges; none for any other value
  halp::grid_value quantize_grid() const noexcept;
  // The Density Deviation of the next grain, drawn when a grain is made
  double density_factor{1.};
  //! What Density Deviation multiplies Density by, for a normal draw \p z of
  //! N(0, 1) at deviation \p d: 2^(z d / 2), within max_density_octaves of
  //! 1 whatever z is; 1 for NaN
  static double density_factor_of(float z, float d) noexcept;
  // Density as heard at the sample being made: its sample-accurate values
  // as they come in the tick
  double density_now{-1.};
  std::size_t density_next{};
  // Gain as heard: it follows the control in a few milliseconds
  double gain_now{-1.};
  // When grains play other files of the folder than the one shown, the
  // length of the file the last grain read: the next grain comes as that
  // one's length and Density say
  double last_spawn_frames{};

  // Sound Order: where the sequence is, for Alphabetical, Ping-pong and
  // Random walk; which way Ping-pong goes
  int order_at{-1};
  int order_dir{1};
  // Shuffle: the order of a cycle, made in prepare() for folders of up to
  // max_shuffle files; past that a permutation i -> (a i + b) mod n, which
  // takes no memory
  static constexpr int max_shuffle = 4096;
  std::vector<uint16_t> shuffle_order;
  int shuffle_n{}, shuffle_at{}, shuffle_last{-1};
  uint64_t shuffle_a{1}, shuffle_b{0};
  //! The file of the folder the next grain plays, as Sound Order says, from
  //! \p note when a MIDI note makes it (else -1); -1 for none ready
  int next_in_order(int picked) noexcept;
  //! Low note and High note, the lower first
  std::pair<int, int> note_range() const noexcept
  {
    const int a = std::clamp((int)inputs.low_note.value, 0, 127);
    const int b = std::clamp((int)inputs.high_note.value, 0, 127);
    return {std::min(a, b), std::max(a, b)};
  }
  //! A note's place in the region: the start of its slice, of a slice a key
  //! of the range
  double note_position(int note) const noexcept
  {
    const auto [lo, hi] = note_range();
    return double(std::clamp(note, lo, hi) - lo) / double(hi - lo + 1);
  }
  //! The note's place of a grain of \p note (-1 for the grains of Continuous
  //! and Trigger), as On MIDI says; -1 when no note places it
  double note_place(int note) const noexcept;
  //! Where a grain of \p note starts, a part of the region: Position, Scan's
  //! place \p scan_at and \p deviation, from the note's place if any
  double grain_place(int note, double scan_at, double deviation) const noexcept;
  //! The file a MIDI note picks, its key's place in the range among the
  //! files, or the next ready one; -1 for none
  int file_of_note(int note) const noexcept;
  //! Of the held notes, the one pressed last; -1 for none
  int latest_note() const noexcept;
  //! The note of the grains of Continuous and Trigger: the held one pressed
  //! last, else the one in effect when the last one was released, until a
  //! file is picked by hand or Sound Order changes; -1 for none
  int clock_note() const noexcept { return midi_held ? latest_note() : latched_note; }
  int latched_note{-1};
  int latched_velocity{127};
  //! The level of a grain of a note of \p velocity, as Velocity says
  double velocity_gain(int velocity) const noexcept;
  std::size_t latch_picked{};
  int latch_order{-1};

  // Scan in folder, Per file: the place of each file, by its name, kept as
  // the folder is listed again (files added or removed move the others a
  // little: looked for near their index). For up to max_scan_files files;
  // past that, files share places.
  static constexpr int max_scan_files = 4096;
  std::vector<double> file_scan, file_scan_next;
  std::vector<std::size_t> file_scan_name, file_scan_name_next;
  int file_scan_count{};
  uint64_t file_scan_generation{~uint64_t(0)};
  // How far Scan went, not wrapped; as far as when the last grain was made
  double scan_travel{}, scan_travel_at_grain{};
  int ui_shown_index{-1};
  //! The places of the files as the folder is listed now
  void relist_file_scans() noexcept;
  //! Scan's place \p phase as its mode keeps it
  double wrap_scan(double phase, double scan_end) const noexcept;
  //! Shuffle: a new order of \p n files, not starting with the last one
  void reshuffle(int n) noexcept;
  //! The ready file nearest to \p i, first \p i itself; -1 for none
  int nearest_ready(int i) const noexcept;
  bool ready(int i) const noexcept
  {
    return inputs.sounds[i].status == halp::sound_status::ready;
  }
  // Grains made since prepare(): how fast they come, for tests
  uint64_t grains_made{};

  float samplerate{48000.f}; // until prepare()

  std::array<MidiVoice, 128> midi_voices{};
  bool midi_active{false};   // Note pitch, and at least one MIDI note held
  bool midi_held{false};     // at least one MIDI note held
  uint64_t notes_pressed{};
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

  using setup = halp::setup;
  void prepare(setup info);

  void resize(int n);
  //! The controls in their ranges, finite
  void sanitize_inputs() noexcept;
  //! The outputs for a sound of \p sound_channels: Output Channels, else as
  //! many as Channels reads of it; two at least with Spread
  int output_channels(int sound_channels) const noexcept;
  //! Fades out the grains past \p n
  void set_voice_count(int n);
  void clear();

  // Musical: Quantize's note values and bars, the tempo the panel shows
  using tick = halp::tick_musical;
  void operator()(tick t);

private:
  // What a tick's grains share, found once for it
  struct tick_plan
  {
    GrainSource cur;
    int cur_index{-1};
    bool from_folder{};
    long voices{};
    int n_channels{1}, ch_offset{}, live_out_channels{}, out_channels{};
    window_shape window;
    const window_table* table{};
    bool table_found{};
    interpolation interp{};
    double region_lo{}, region_len{1.};
    double scan_step{}, scan_end{1.};
    double interval{};
    std::array<double, 128> note_interval{};
    double gain_step{};
    // Quantize: its grid, and its step in samples at this tick's tempo
    halp::grid_value grid{};
    double grid_step{};
  };
  tick_plan make_plan(
      const tick& t, const GrainSource& cur, int cur_index, bool from_folder) noexcept;
  //! The first point of the grid \p g in \p t from frame \p from on, after
  //! the last one; none in this tick
  std::optional<int>
  grid_point(const tick& t, const halp::grid_value& g, int from) const noexcept;

  void stop_replaced_sound() noexcept;
  GrainSource current_source(bool from_folder, int& index) noexcept;
  void notify_current_sound(int index, const GrainSource& cur) noexcept;
  void read_midi() noexcept;

  //! A Deviation's value for a grain: N(0, d / 4)
  float deviate(float deviation) noexcept;
  //! The length of a grain at speed 1, with Duration Deviation, as a part of
  //! a sound of \p frames: of the region in Region mode, Duration Time in
  //! Time mode
  double grain_duration(double frames, double region_len) noexcept;
  //! Whether the grains have a length: Duration above 0, or Time mode
  bool has_duration() const noexcept { return timed() || inputs.dur != 0.; }
  //! Whether the grains read the folder's files: Folder, with a folder set
  bool reads_folder() const noexcept;
  //! Density at sample \p s of the tick, in range
  double density_at(int s) noexcept;
  //! A grain's speed at \p base, with Pitch Deviation and Reverse Chance
  float grain_rate(float base) noexcept;
  //! The shortest grain, as a part of a sound of \p frames, at \p rate
  double min_duration(double rate, double frames) const noexcept;
  //! The pitch of a MIDI note, middle C at 1
  static float note_ratio(int note) noexcept { return std::exp2((note - 60) / 12.f); }
  //! Whether notes play grains of their own, as On MIDI says
  bool notes_trigger() const noexcept;
  //! The speed of a grain at Pitch, for a grain of \p note (-1 for the
  //! grains of Continuous and Trigger), as On MIDI says
  float pitch_for(int note) const noexcept;

  //! Starts a grain: false if there is nothing to read
  bool spawn(Grain& g, tick_plan& p) noexcept;
  //! Renders and mixes \p g from sample \p s0 of the block of \p n samples
  //! at \p b0: the sample after its last one in the block
  int play(Grain& g, int s0, int n, int b0, const tick_plan& p) noexcept;
  void advance_scan(const tick_plan& p) noexcept;
  void advance_timing(const tick_plan& p, int s, bool at_grid) noexcept;

  // Another sequence for every instance
  static std::random_device& random_source()
  {
    static thread_local std::random_device d;
    return d;
  }
  rnd::pcg rd{random_source()};
  // Kept: it makes its values two at a time
  std::normal_distribution<float> n01{0.f, 1.f};
  // Where the search for a free voice starts: after the last one taken
  long next_voice{0};
};

}
