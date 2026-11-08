#pragma once
#include "grain.hpp"

#include <ossia/detail/variant.hpp>

#include <halp/audio.hpp>
#include <halp/controls.hpp>
#include <halp/folder_combobox.hpp>
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
    halp::hslider_f32<"Position", halp::range{0.00000001, 1., 0.00000001}> pos;
    // Each Deviation spreads its control per grain, as a normal distribution
    // of deviation d / 4: at 1, 95% of the grains within ±0.5 of it, of the
    // region for Position and Duration. Density's and Gain's are their own,
    // below.
    halp::hslider_f32<"Position Deviation", halp::range{0., 1., 0.}> pos_j;
    // Position moves through the sound by itself, at this speed: 1 goes
    // through it in its own duration, -1 backwards, 0 stays at Position
    halp::hslider_f32<"Scan", halp::range{-2., 2., 0.}> scan;
    halp::hslider_f32<"Duration", halp::range{0.00000001, 1., 0.1}> dur;
    halp::hslider_f32<"Duration Deviation", halp::range{0., 1., 0.}> dur_j;
    // Each grain somewhere among the output channels (Output Channels): 0 in
    // all of them alike, 1 between two neighbouring ones (1-D VBAP), 2 in a
    // single one
    halp::hslider_f32<"Spread", halp::range{0., 2., 0.}> spread;
    struct : halp::knob_f32<"Pitch", halp::range{0.000001, 10., 1.}>
    {
      using mapper = halp::inverse_mapper<halp::pow_mapper<4>>;
    } rate;
    halp::vslider_f32<"Pitch Deviation", halp::range{0., 1., 0.}> rate_j;
    // Pitch Deviation in whole semitones, the same at every pitch: at 1, 95% of
    // the grains within an octave; else as a ratio added to Pitch
    halp::toggle<"Pitch Deviation in Semitones"> rate_j_semitones;
    // Grains per grain length. Sample-accurate: automation and cables change
    // the pace at the sample their values come at, not a block later. 0
    // makes no grains, which a Trigger or a note still makes; any Density
    // above 0 does, from the sample it comes at.
    struct : halp::accurate<halp::knob_f32<"Density", halp::range{0., 256., 1.}>>
    {
      using mapper = halp::log_mapper<std::ratio<99, 100>>;
    } density;
    // Multiplies Density per grain by 2^N(0, d / 2): at 1, 95% of the
    // intervals between half and twice as long, as many shorter as longer
    halp::vslider_f32<"Density Deviation", halp::range{0., 1., 0.}> dens_j;
    // How likely a grain plays backwards: 1 all of them
    halp::knob_f32<"Reverse Chance", halp::range{0., 1., 0.}> reverse_chance;
    halp::knob_f32<"Gain", halp::range{.min = 0., .max = 4., .init = 0.5}> gain;
    // Lowers some grains, in decibels: down to -24 dB at 1, never louder
    halp::vslider_f32<"Gain Deviation", halp::range{0., 1., 0.}> gain_j;
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
    struct : halp::spinbox_i32<"Source Channels", halp::range{0, max_channels, 0}>
    {
    } src_channels;
    struct : halp::spinbox_i32<"Channel Offset", halp::range{0, max_channels - 1, 0}>
    {
    } channel_offset;
    struct : halp::spinbox_i32<"Max Voices", halp::range{0, max_voices, 128}>
    {
      void update(Granola& self) { self.set_voice_count(value); }
    } num_voices;
    struct : halp::toggle<"Continuous", halp::toggle_setup{.init = true}>
    {
      void update(Granola& self) { self.trigger = value; }
    } playing;
    halp::impulse_button<"Trigger"> trig;
    halp::midi_bus<"MIDI In", libremidi::message> midi;
    // What the notes of MIDI In do. Nothing: no grain, no pitch. Trigger: a
    // held note plays grains at Pitch, at its own pace. Trigger and set
    // pitch: at its pitch, at the pace its pitch makes. Set pitch: no grain
    // of its own; the grains of Continuous and Trigger at the pitch of the
    // note pressed last of those held, and once all are released at the
    // pitch of the last one, until a file is picked by hand or Sound Order
    // changes; at Pitch before any note. A note's pitch is relative to note
    // 60. Sound Order's By MIDI note picks files from the notes in every one.
    struct
    {
      halp_meta(
          description,
          "What MIDI notes do: play grains while held, at Pitch or at their pitch, or "
          "transpose the grains of Continuous and Trigger")
      static clang_buggy_consteval auto name() { return "On MIDI"; }
      enum enum_type
      {
        Nothing,
        Trigger,
        TriggerPitch,
        SetPitch
      } value = TriggerPitch;
      enum widget
      {
        combobox
      };
      struct range
      {
        std::pair<std::string_view, enum_type> values[4]{
            {"Nothing", Nothing},
            {"Trigger", Trigger},
            {"Trigger and set pitch", TriggerPitch},
            {"Set pitch", SetPitch}};
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

    // Where the sound comes from. Folder with no folder set reads the Sound
    // file, as the file picked in its own folder: switching to Folder plays on
    // what played
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
    // Which file of the folder each grain plays. Single file: the one Sound
    // in folder picks. Random: any, each grain. Shuffle: every file once, in
    // a random order, then again in another, never the same file twice in a
    // row. Alphabetical: the next one in the folder's order (as the host
    // lists it, in natural order), around again after the last. Ping-pong:
    // to the last and back, the ends once. Random walk: a neighbour, mostly
    // the next or previous one, sometimes the one after, back from the ends.
    // By MIDI note: a note of MIDI In picks it, (note - 60) modulo the
    // number of files, whatever On MIDI says: the grains a note plays read
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
          "Folder only: when another file plays, Scan restarts from Position, continues, "
          "or goes on where that file's grains were")
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
    // Positions and levels in [0, 1] as integers of this many steps
    static constexpr double pos_scale = 65535.;
    static constexpr double amp_scale = 255.;
    uint8_t count;
    bool scanning;           // whether Scan moves Position, to:
    uint16_t scan;           // where it has moved it in the region
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
  // Samples since the last grain, with the fraction of a sample the
  // interval had: grains at the rate Density sets, not one rounded to
  // whole samples
  double trigger_counter{0};
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
  //! The file a MIDI note picks, (note - 60) modulo the number of files, or
  //! the next ready one; -1 for none
  int file_of_note(int note) const noexcept;
  //! Of the held notes, the one pressed last; -1 for none
  int latest_note() const noexcept;
  //! The note of the grains of Continuous and Trigger: the held one pressed
  //! last, else the one in effect when the last one was released, until a
  //! file is picked by hand or Sound Order changes; -1 for none
  int clock_note() const noexcept { return midi_held ? latest_note() : latched_note; }
  int latched_note{-1};
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

  using tick = halp::tick;
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
  };
  tick_plan make_plan(const GrainSource& cur, int cur_index, bool from_folder) noexcept;

  void stop_replaced_sound() noexcept;
  GrainSource current_source(bool from_folder, int& index) noexcept;
  void notify_current_sound(int index, const GrainSource& cur) noexcept;
  void read_midi() noexcept;

  //! A Deviation's value for a grain: N(0, d / 4)
  float deviate(float deviation) noexcept;
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
  void advance_timing(const tick_plan& p, int s) noexcept;

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
