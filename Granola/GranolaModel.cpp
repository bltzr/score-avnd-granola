#include "Granola.hpp"

#include <ossia/math/safe_math.hpp>

#include <avnd/introspection/input.hpp>
#include <avnd/introspection/range.hpp>

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <numeric>
#include <numbers>
#include <string_view>

namespace Granola
{

void Granola::prepare(setup info)
{
  samplerate = info.rate;

  // Every grain Max Voices allows is made here, with its buffers: neither
  // adding nor removing voices allocates or frees on the audio thread. The
  // host prepares again when the outputs change: the grains then play on.
  // The grains may be made already, by set_voice_count() which the host
  // calls first: what goes with them too
  if(grains.size() != std::size_t(max_voices) || free_from.size() != grains.size())
    resize(max_voices);
  if(window_tables.size() != std::size_t(window_tables_count))
    window_tables.resize(window_tables_count);
  // A block's samples, per channel: as long as the host's blocks, or the
  // block is done in parts
  max_block = std::clamp(info.frames, 64, 8192);
  scratch.assign(std::size_t(max_channels) * max_block, 0.);
  shared.assign(std::size_t(max_channels) * max_block, 0.);
  block_gain.assign(max_block, 0.);
  if(shuffle_order.size() != std::size_t(max_shuffle))
    shuffle_order.assign(max_shuffle, 0);
  if(file_scan.size() != std::size_t(max_scan_files))
  {
    file_scan.assign(max_scan_files, 0.);
    file_scan_next.assign(max_scan_files, 0.);
    file_scan_name.assign(max_scan_files, 0);
    file_scan_name_next.assign(max_scan_files, 0);
    file_scan_count = 0;
    file_scan_generation = ~uint64_t(0);
  }
  shuffle_n = 0;
  set_voice_count(inputs.num_voices);
}

int Granola::nearest_ready(int i) const noexcept
{
  const int n = (int)inputs.sounds.size();
  for(int d = 0; d < n; d++)
  {
    if(i + d < n && ready(i + d))
      return i + d;
    if(d > 0 && i - d >= 0 && ready(i - d))
      return i - d;
  }
  return -1;
}

void Granola::reshuffle(int n) noexcept
{
  shuffle_n = n;
  shuffle_at = 0;
  if(n <= int(shuffle_order.size()))
  {
    // Fisher-Yates
    for(int i = 0; i < n; i++)
      shuffle_order[i] = uint16_t(i);
    for(int i = n - 1; i > 0; i--)
      std::swap(shuffle_order[i], shuffle_order[unsigned(rd()) % unsigned(i + 1)]);
    if(n > 1 && shuffle_order[0] == shuffle_last)
      std::swap(shuffle_order[0], shuffle_order[1 + unsigned(rd()) % unsigned(n - 1)]);
    return;
  }
  // A multiplier prime with n, found in a few tries; 1 at worst
  shuffle_a = 1;
  for(int k = 0; k < 32; k++)
    if(const uint64_t a = 1 + uint64_t(rd()) % uint64_t(n - 1); std::gcd(a, uint64_t(n)) == 1)
    {
      shuffle_a = a;
      break;
    }
  shuffle_b = uint64_t(rd()) % uint64_t(n);
  if(int(shuffle_b) == shuffle_last)
    shuffle_b = (shuffle_b + 1) % uint64_t(n);
}

int Granola::file_of_note(int note) const noexcept
{
  const int n = (int)inputs.sounds.size();
  if(n == 0)
    return -1;
  const auto [lo, hi] = note_range();
  const int first
      = hi > lo ? int(std::lround(double(std::clamp(note, lo, hi) - lo) / (hi - lo) * (n - 1)))
                : 0;
  for(int k = 0; k < n; k++)
    if(const int i = (first + k) % n; ready(i))
      return i;
  return -1;
}

int Granola::latest_note() const noexcept
{
  int note = -1;
  uint64_t when = 0;
  for(int n = 0; n < 128; n++)
    if(const auto& v = midi_voices[n]; v.active && v.pressed >= when)
    {
      note = n;
      when = v.pressed;
    }
  return note;
}

void Granola::relist_file_scans() noexcept
{
  const auto& sounds = inputs.sounds;
  if(file_scan.empty() || sounds.generation() == file_scan_generation)
    return;
  file_scan_generation = sounds.generation();
  // Each file's place, from the file of its name nearest to its index
  const int n = std::min((int)sounds.size(), max_scan_files);
  constexpr int search_span = 16;
  for(int i = 0; i < n; i++)
  {
    const auto name = std::hash<std::string_view>{}(sounds[i].name);
    file_scan_name_next[i] = name;
    file_scan_next[i] = 0.;
    for(int d = 0; d <= search_span; d++)
    {
      if(i + d < file_scan_count && file_scan_name[i + d] == name)
      {
        file_scan_next[i] = file_scan[i + d];
        break;
      }
      if(d > 0 && i - d >= 0 && i - d < file_scan_count && file_scan_name[i - d] == name)
      {
        file_scan_next[i] = file_scan[i - d];
        break;
      }
    }
  }
  std::swap(file_scan, file_scan_next);
  std::swap(file_scan_name, file_scan_name_next);
  file_scan_count = n;
}

int Granola::next_in_order(int picked) noexcept
{
  using order = std::decay_t<decltype(inputs.order.value)>;
  const int n = (int)inputs.sounds.size();
  if(n == 0)
    return -1;
  order_at = std::min(order_at, n - 1);
  switch(inputs.order.value)
  {
    case order::Single:
    case order::ByNote: // with no note held
    default:
      return picked;
    case order::Random:
      return random_ready_index();
    case order::Alphabetical:
      for(int k = 1; k <= n; k++)
        if(const int i = (order_at + k) % n; ready(i))
          return order_at = i;
      return -1;
    case order::PingPong:
      // 0 1 2 1 0 1 ...: past an end, back the other way
      for(int k = 0; k < 2 * n; k++)
      {
        int i = order_at + order_dir;
        if(i < 0 || i >= n)
        {
          order_dir = -order_dir;
          i = std::clamp(order_at + order_dir, 0, n - 1);
        }
        order_at = i;
        if(ready(i))
          return i;
      }
      return -1;
    case order::Shuffle:
      for(int k = 0; k < n; k++)
      {
        if(shuffle_n != n || shuffle_at >= shuffle_n)
          reshuffle(n);
        const int at = shuffle_at++;
        const int i = n <= int(shuffle_order.size())
                          ? int(shuffle_order[at])
                          : int((shuffle_a * uint64_t(at) + shuffle_b) % uint64_t(n));
        if(ready(i))
          return shuffle_last = i;
      }
      return -1;
    case order::RandomWalk: {
      // From the picked file; one step mostly, two sometimes, either way,
      // back from the ends as a reflection
      if(order_at < 0)
        order_at = std::max(picked, 0);
      const int step
          = (std::uniform_real_distribution<float>(0.f, 1.f)(rd) < 0.8f ? 1 : 2)
            * (rd() & 1 ? 1 : -1);
      int i = order_at + step;
      const int last = n - 1;
      for(int k = 0; k < 4 && (i < 0 || i > last); k++)
        i = i < 0 ? -i : 2 * last - i;
      // Back onto the same file, as two steps from the second one: the
      // other way instead
      if(i == order_at && n > 1)
        i = order_at - step;
      i = std::clamp(i, 0, last);
      const int r = nearest_ready(i);
      if(r >= 0)
        order_at = r;
      return r;
    }
  }
}

int Granola::find_folder_index()
{
  const auto& sounds = inputs.sounds;
  const auto name = std::hash<std::string_view>{}(inputs.sound_file.value);
  if(sounds.generation() != folder_index_generation || name != folder_index_name)
  {
    folder_index_generation = sounds.generation();
    folder_index_name = name;
    if(sounds.empty())
      folder_index = -1;
    else if(auto index = inputs.sound_file.index())
      folder_index = std::min(*index, int(sounds.size()) - 1);
    else
    {
      folder_index = sounds.find(inputs.sound_file.value);
      // No such file, e.g. none picked yet: the first one
      if(folder_index < 0)
        folder_index = 0;
    }
  }
  return folder_index;
}

int Granola::random_ready_index()
{
  const auto& sounds = inputs.sounds;
  const int n = (int)sounds.size();
  if(n == 0)
    return -1;
  // A random file, or the next one that could be decoded
  const int start = int(unsigned(rd()) % unsigned(n));
  for(int k = 0; k < n; k++)
  {
    const int i = (start + k) % n;
    if(sounds[i].status == halp::sound_status::ready)
      return i;
  }
  return -1;
}

void Granola::resize(int n)
{
  grains.resize(n);
  free_from.resize(n);
  clear();
}

void Granola::set_voice_count(int n)
{
  // Before prepare(), e.g. in the copy of the object the UI keeps
  if(grains.size() < std::size_t(max_voices))
    resize(max_voices);

  // The grains of the voices past the count fade out in 2 ms: no click
  n = std::clamp(n, 0, max_voices);
  const long fade = std::max(long(0.002 * samplerate), 1L);
  for(std::size_t i = n; i < grains.size(); i++)
    grains[i].release(fade);
}

void Granola::clear()
{
  for(auto& grain : grains)
    grain.reset();
}

float Granola::scanned(double rel) const noexcept
{
  using mode = std::decay_t<decltype(inputs.scan_mode.value)>;
  const double last = scan_last();
  switch(inputs.scan_mode.value)
  {
    case mode::Straight:
      return float(std::clamp(rel, 0., last));
    case mode::PingPong: {
      const double m = rel - 2. * last * std::floor(rel / (2. * last));
      return float(m <= last ? m : 2. * last - m);
    }
    case mode::Loop:
    default:
      return float(rel - last * std::floor(rel / last));
  }
}

int Granola::output_channels(int sound_channels) const noexcept
{
  const int read = inputs.src_channels <= 0
                       ? std::min(sound_channels, max_channels)
                       : std::clamp(
                           (int)inputs.src_channels, 1,
                           std::max(std::min(sound_channels, max_channels), 1));
  const int n = inputs.out_channels <= 0
                    ? read
                    : std::min((int)inputs.out_channels, max_outputs);
  // Spread needs two outputs: as many as channels read gives two for a mono
  // read. Output Channels says how many there are; over one, Spread places
  // every grain in it.
  return inputs.spread > 0.f && inputs.out_channels <= 0 ? std::max(n, 2) : n;
}

void Granola::send_playheads(int frames, const GrainSource& cur)
{
  if(!send_message)
    return;

  // ~30 messages per second is plenty for display
  ui_frame_counter += frames;
  if(ui_frame_counter < samplerate / 30.f)
    return;
  ui_frame_counter = 0;

  processor_to_ui msg{playheads{}};
  auto& ph = ossia::get<playheads>(msg.message);
  ph.count = 0;
  // Where the grains of Continuous and Trigger start: from a note's place
  // with Set position
  const bool placed = note_place(-1) >= 0.;
  ph.scanning = inputs.scan != 0.f || placed;
  // The place of the file shown, Per file
  double at = scan_phase;
  if(ui_shown_index >= 0 && file_scan_count > 0
     && inputs.scan_folder.value == decltype(inputs.scan_folder.value)::PerFile)
    at = file_scan[ui_shown_index % file_scan_count];
  ph.scan = ph.scanning
                ? uint16_t(std::lround(grain_place(-1, at, 0.) * playheads::pos_scale))
                : 0;
  for(long i = 0; i < std::min<long>(inputs.num_voices, (long)grains.size()); i++)
  {
    const auto& g = grains[i];
    // Only the grains of the sound that is shown: Sound Order plays others
    if(!g.m_active || g.m_source.data != cur.data)
      continue;
    ph.pos[ph.count] = uint16_t(std::lround(g.position() * playheads::pos_scale));
    ph.amp[ph.count]
        = uint8_t(std::lround(std::clamp(g.m_last_win, 0., 1.) * playheads::amp_scale));
    if(++ph.count == playheads::max_heads)
      break;
  }

  // Whenever Scan moves Position, even with no grain of the shown sound, as
  // when the grains play the folder's other files: else the play zone
  // stands still, then jumps. Else while grains of it play, and once more
  // to clear them.
  const bool moving = ph.scanning && (inputs.playing || midi_active);
  if(ph.count == 0 && !moving && !ui_heads_shown)
    return;
  ui_heads_shown = ph.count > 0 || moving;
  send_message(std::move(msg));
}

const window_table* Granola::table_for(const window_shape& w) noexcept
{
  for(const auto& t : window_tables)
    if(t.built && t.shape == w)
      return t.accurate ? &t : nullptr;
  // Building one costs a few hundred microseconds: only for a new window
  for(auto& t : window_tables)
  {
    const bool read = std::ranges::any_of(
        grains, [&t](const Grain& g) { return g.m_active && g.m_table == &t; });
    if(!read)
    {
      t.build(w);
      return t.accurate ? &t : nullptr;
    }
  }
  return nullptr;
}

void Granola::spread_grain(Grain& g, int places) noexcept
{
  auto& pan = g.m_pan;
  pan = {};
  // 0: in all of them alike; 1: between two neighbouring ones (1-D VBAP);
  // 2: in one of them
  const float sp = std::clamp(inputs.spread.value, 0.f, 2.f);
  if(sp <= 0.f || places <= 1)
    return;
  // A loudspeaker, each as likely, and a place around it
  const float u = std::uniform_real_distribution<float>(0.f, float(places))(rd);
  const int speaker = std::min(int(u), places - 1);
  const float x = std::clamp(u - 0.5f, 0.f, float(places - 1));
  const int a = std::min(int(x), places - 2);
  const float t = x - a;
  // Between a and a + 1 at the same power wherever it is
  constexpr float half_pi = std::numbers::pi_v<float> / 2.f;
  float wa = std::cos(t * half_pi);
  float wb = std::sin(t * half_pi);
  // Past 1, towards the loudspeaker alone, which is a or a + 1
  if(const float one = sp - 1.f; one > 0.f)
  {
    wa = (1.f - one) * wa + one * float(speaker == a);
    wb = (1.f - one) * wb + one * float(speaker == a + 1);
  }
  const float pair = std::min(sp, 1.f);
  pan.all = 1.f - pair;
  pan.a = a;
  pan.at_a = pair * wa;
  pan.at_b = pair * wb;
}

void Granola::sanitize_inputs() noexcept
{
  // In range, and the default for NaN or infinity
  const auto fix = []<typename V>(V& v, auto lo, auto hi, auto init) {
    if constexpr(std::is_floating_point_v<V>)
      v = ossia::safe_isfinite(v) ? std::clamp(v, V(lo), V(hi)) : V(init);
    else
      v = std::clamp(v, V(lo), V(hi));
  };
  avnd::input_introspection<Granola>::for_all(inputs, [&]<typename F>(F& field) {
    if constexpr(avnd::has_range<F> && requires { field.value; })
    {
      using V = std::decay_t<decltype(field.value)>;
      static constexpr auto r = avnd::get_range<F>();
      if constexpr(
          (std::is_floating_point_v<V> || std::is_integral_v<V>) && !std::is_same_v<V, bool>
          && requires { V(r.min); V(r.max); V(r.init); })
      {
        fix(field.value, r.min, r.max, r.init);
      }
      else if constexpr(requires { field.value.x; field.value.y; })
      {
        // An xy pad: one range for both axes, a point to start from
        if constexpr(requires { r.init.x; })
        {
          fix(field.value.x, r.min, r.max, r.init.x);
          fix(field.value.y, r.min, r.max, r.init.y);
        }
        else
        {
          fix(field.value.x, r.min, r.max, r.init);
          fix(field.value.y, r.min, r.max, r.init);
        }
      }
    }
  });
}

void Granola::stop_replaced_sound() noexcept
{
  // A replaced Sound file is freed: the grains still reading it stop. By its
  // data rather than its name: the same file loaded again is another one.
  // The grains of the folder pin their file and play on.
  const GrainSource sound{
      inputs.sound.soundfile.data, (long)inputs.sound.soundfile.channels,
      (double)inputs.sound.soundfile.frames};
  if(sound.data == last_sound.data && sound.frames == last_sound.frames
     && sound.channels == last_sound.channels)
    return;
  last_sound = sound;
  for(auto& grain : grains)
    if(!grain.m_pin)
      grain.reset();
}

GrainSource Granola::current_source(bool from_folder, int& index) noexcept
{
  // The Sound file, or the file of the folder picked by name or index (in
  // with a Sound Order, each grain picks its own and this one is shown)
  index = -1;
  if(from_folder)
  {
    index = find_folder_index();
    if(index >= 0)
      if(const auto& snd = inputs.sounds[index]; snd.status == halp::sound_status::ready)
        return {snd.data, (long)snd.channels, (double)snd.frames};
    return {};
  }
  if(inputs.sound && inputs.sound.channels() > 0)
    return {
        inputs.sound.soundfile.data, (long)inputs.sound.channels(),
        (double)inputs.sound.frames()};
  return {};
}

void Granola::notify_current_sound(int index, const GrainSource& cur) noexcept
{
  // Whether the sound shown could be decoded, when that changes or a
  // (re)created panel asks
  if(!send_message)
    return;
  const auto status = index >= 0 ? inputs.sounds[index].status
                      : cur      ? halp::sound_status::ready
                                 : halp::sound_status::unloaded;
  if(!ui_refresh && index == ui_sent_index && status == ui_sent_status
     && (index < 0 || inputs.sounds.generation() == ui_sent_generation))
    return;
  processor_to_ui msg{current_sound{index, status, {}}};
  if(index >= 0)
  {
    auto& cs = ossia::get<current_sound>(msg.message);
    const std::string_view name = inputs.sounds[index].name;
    const auto n = std::min(name.size(), sizeof(cs.name) - 1);
    std::memcpy(cs.name, name.data(), n);
    cs.name[n] = 0;
  }
  send_message(std::move(msg));
  ui_refresh = false;
  ui_sent_index = index;
  ui_sent_status = status;
  ui_sent_generation = inputs.sounds.generation();
}

void Granola::read_midi() noexcept
{
  // The note Continuous and Trigger go on with once released: until a file
  // is picked by hand, or Sound Order changes
  const auto picked = std::hash<std::string_view>{}(inputs.sound_file.value);
  if(picked != latch_picked || int(inputs.order.value) != latch_order)
  {
    latched_note = -1;
    latch_picked = picked;
    latch_order = int(inputs.order.value);
  }

  // Notes play grains, or pick files
  if(inputs.on_midi.value == decltype(inputs.on_midi.value)::Nothing
     && inputs.order.value != decltype(inputs.order.value)::ByNote)
  {
    // Not listening: every note off, none stuck
    if(midi_held)
    {
      for(auto& v : midi_voices)
        v.active = false;
      midi_held = midi_active = false;
      midi_pending.clear();
    }
    latched_note = -1;
    return;
  }
  using libremidi::message_type;
  for(const auto& msg : inputs.midi)
  {
    if(msg.bytes.size() < 3)
      continue;
    const auto type = message_type(msg.bytes[0] & 0xF0);
    const int note = msg.bytes[1] & 0x7F;
    const int vel = msg.bytes[2] & 0x7F;
    if(type == message_type::NOTE_ON && vel > 0) // its first grain at once
    {
      auto& v = midi_voices[note];
      v.active = true;
      v.counter = 0;
      v.velocity = uint8_t(vel);
      v.pressed = ++notes_pressed;
      midi_held = true;
      midi_pending.set(note);
      latched_note = note;
      latched_velocity = vel;
    }
    else if(type == message_type::NOTE_OFF || type == message_type::NOTE_ON)
    {
      // The last one released: what played then goes on
      const int in_effect = latest_note();
      midi_voices[note].active = false;
      midi_pending.reset(note);
      midi_held = std::ranges::any_of(midi_voices, &MidiVoice::active);
      if(!midi_held && in_effect >= 0)
      {
        latched_note = in_effect;
        latched_velocity = midi_voices[in_effect].velocity;
      }
    }
  }
  // The notes' own grains only with Note pitch; else they may pick files
  midi_active = notes_trigger() && midi_held;
  if(!midi_active)
    midi_pending.clear();
}

float Granola::deviate(float deviation) noexcept
{
  // N(0, d / 4): a normal distribution needs a deviation above 0, so it is
  // scaled after
  return deviation > 0.f ? n01(rd) * 0.25f * deviation : 0.f;
}

double Granola::grain_duration(double frames, double region_len) noexcept
{
  if(!timed())
    return (inputs.dur + deviate(inputs.dur_j)) * region_len;
  // Seconds of the sound: the same time whatever the length of the file. A
  // Deviation relative to the grain, as Density's: never 0 or negative.
  const double part = (double)inputs.dur_time.value * samplerate / frames;
  return inputs.dur_j > 0.f ? part * density_factor_of(n01(rd), inputs.dur_j) : part;
}

double Granola::density_factor_of(float z, float d) noexcept
{
  // In octaves, so that as many intervals are shorter as longer, and none is
  // ever 0 or infinite: the next grain always comes
  const double octaves = double(z) * double(d) * 0.5;
  if(ossia::safe_isnan(octaves))
    return 1.;
  return std::exp2(std::clamp(octaves, -max_density_octaves, max_density_octaves));
}

double Granola::density_at(int s) noexcept
{
  // The values that came by this sample, as the control's own are: in range
  const auto& values = inputs.density.values;
  for(; density_next < values.size(); density_next++)
  {
    const auto& [at, v] = *values.nth(density_next);
    if(at > s)
      break;
    density_now = ossia::safe_isfinite(v) ? std::clamp(double(v), 0., 256.) : 1.;
  }
  return density_now;
}

bool Granola::reads_folder() const noexcept
{
  return inputs.source.value == decltype(inputs.source.value)::Folder
         && !inputs.sounds.value.empty();
}

float Granola::grain_rate(float base) noexcept
{
  // Pitch Deviation in whole semitones (6 a deviation at 1: notes of a scale
  // rather than a detuned smear), or added to the pitch and then four
  // octaves under it at the lowest: else a grain could read at about no
  // speed, last for minutes and keep a voice.
  const float rate = inputs.rate_j_semitones
                         ? base * std::exp2(std::round(deviate(inputs.rate_j) * 24.f) / 12.f)
                         : std::max(base + deviate(inputs.rate_j), base / 16.f);
  // Backwards as often as Reverse Chance says: never at 0, always at 1
  const float chance = inputs.reverse_chance;
  const bool backwards
      = chance >= 1.f
        || (chance > 0.f && std::uniform_real_distribution<float>(0.f, 1.f)(rd) < chance);
  return backwards ? -rate : rate;
}

double Granola::velocity_gain(int velocity) const noexcept
{
  const double v = std::clamp(velocity, 0, 127) / 127.;
  const double amount = ossia::safe_isfinite(inputs.velocity.value)
                            ? std::clamp((double)inputs.velocity.value, 0., 1.)
                            : 1.;
  return 1. - amount * (1. - v * v);
}

bool Granola::notes_trigger() const noexcept
{
  using on = std::decay_t<decltype(inputs.on_midi.value)>;
  const auto m = inputs.on_midi.value;
  return m == on::Trigger || m == on::TriggerPitch || m == on::TriggerPosition;
}

double Granola::note_place(int note) const noexcept
{
  using on = std::decay_t<decltype(inputs.on_midi.value)>;
  switch(inputs.on_midi.value)
  {
    case on::SetPosition:
      if(const int held = clock_note(); held >= 0)
        return note_position(held);
      return -1.;
    case on::TriggerPosition:
      return note >= 0 ? note_position(note) : -1.;
    default:
      return -1.;
  }
}

double Granola::grain_place(int note, double scan_at, double deviation) const noexcept
{
  const double rel = inputs.pos + scan_at + deviation;
  const double place = note_place(note);
  if(place < 0.)
    return inputs.scan != 0.f ? scanned(rel) : std::clamp(rel, 0., 1.);
  // From the note's place, folded at the region's ends as Scan Mode says
  const double x = place + rel;
  using mode = std::decay_t<decltype(inputs.scan_mode.value)>;
  switch(inputs.scan_mode.value)
  {
    case mode::Straight:
      return std::clamp(x, 0., 1.);
    case mode::PingPong: {
      const double m = x - 2. * std::floor(x / 2.);
      return m <= 1. ? m : 2. - m;
    }
    case mode::Loop:
    default:
      return x - std::floor(x);
  }
}

float Granola::pitch_for(int note) const noexcept
{
  using on = std::decay_t<decltype(inputs.on_midi.value)>;
  switch(inputs.on_midi.value)
  {
    case on::TriggerPitch:
      return note >= 0 ? inputs.rate * note_ratio(note) : inputs.rate;
    case on::SetPitch:
      if(const int held = clock_note(); held >= 0)
        return inputs.rate * note_ratio(held);
      return inputs.rate;
    default:
      return inputs.rate;
  }
}

double Granola::min_duration(double rate, double frames) const noexcept
{
  // A time, not a part of the file, so that long files still give short
  // grains; two samples of the file at the least
  return std::max(min_grain_seconds * samplerate * std::abs(rate), 2.) / frames;
}

Granola::tick_plan Granola::make_plan(const GrainSource& cur, int cur_index, bool from_folder)
    noexcept
{
  tick_plan p;
  p.cur = cur;
  p.cur_index = cur_index;
  p.from_folder = from_folder;
  p.voices = std::clamp<long>(inputs.num_voices, 0, (long)grains.size());

  // The sound's channels the grains read, 0 all of them; the outputs, 0 as
  // many: outputs past the channels read take them again in turn, e.g. a
  // mono sound in all of them
  p.n_channels = inputs.src_channels <= 0
                     ? std::min((int)cur.channels, max_channels)
                     : std::clamp(
                         (int)inputs.src_channels, 1, std::min((int)cur.channels, max_channels));
  p.ch_offset = std::clamp((int)inputs.channel_offset, 0, (int)cur.channels - p.n_channels);

  // request_channels() changes outputs.audio.channels now but the buffers
  // follow next tick, so only touch channels backed this tick: the fewer of
  // the two. As many as asked for, more or fewer.
  p.live_out_channels = (int)outputs.audio.channels;
  const int wanted = output_channels((int)cur.channels);
  if(p.live_out_channels != wanted && outputs.audio.request_channels)
    outputs.audio.request_channels(wanted);
  p.out_channels = std::min(wanted, p.live_out_channels);

  // The window of the grains made in this tick; its table is found when a
  // grain is made
  p.window = window_shape::from_pad(
      int(inputs.window_mode.value), inputs.win_coefs.value.x, inputs.win_coefs.value.y);
  p.interp = interpolation(std::clamp(int(inputs.interp_type.value), 0, 2));

  // The region, as parts of the sound, in either order, two samples at least
  p.region_lo = std::clamp(std::min(inputs.sample_start.value, inputs.sample_end.value), 0.f, 1.f);
  const double hi
      = std::clamp(std::max(inputs.sample_start.value, inputs.sample_end.value), 0.f, 1.f);
  p.region_len = std::max(hi - p.region_lo, 2. / cur.frames);
  // Duration Time in the region of the shown sound: where Scan turns
  time_in_region = timed() ? std::min(
                                 (double)inputs.dur_time.value * samplerate
                                     / (cur.frames * p.region_len),
                                 1.)
                           : 0.;

  // Scan: Position moves by itself, through the region at Scan times the
  // sound's speed. From Position again when it stops, when it starts
  // playing, and when the sound changes, unless it goes on across the
  // folder's sounds.
  using scan_folder = std::decay_t<decltype(inputs.scan_folder.value)>;
  const bool new_sound = cur.data != scan_source
                         && !(from_folder && inputs.scan_folder.value != scan_folder::Restart);
  const bool restart = (inputs.playing && !scan_was_playing) || inputs.scan == 0.f;
  if(new_sound || restart)
    scan_phase = 0.;
  if(from_folder)
    relist_file_scans();
  if(restart && file_scan_count > 0)
    std::fill_n(file_scan.begin(), file_scan_count, 0.);
  scan_source = cur.data;
  scan_was_playing = inputs.playing;
  p.scan_step = inputs.scan / (cur.frames * p.region_len);
  p.scan_end = scan_last();

  // When Sound Order plays other files than the one shown, the length of
  // the file the last grain read: the next grain comes as that one's length
  // and Density say, not as the length of the file shown
  const bool other_files = from_folder && inputs.order.value != decltype(inputs.order.value)::Single;
  const double frames
      = other_files && last_spawn_frames > 0. ? last_spawn_frames : cur.frames;
  // Samples between grains at a pitch, but for Density: the length of a
  // grain, at least the shortest one, else a Duration ~= 0 (stale or
  // automated) would pack grains back-to-back up to the voice cap
  // Keep time: the length of a grain at speed 1 whatever its pitch, so that
  // grains of any pitch keep the same pace
  // Time mode: Duration Time, the same in any file
  const double part = timed() ? (double)inputs.dur_time.value * samplerate / frames
                              : (double)inputs.dur * p.region_len;
  const auto interval_at = [&](double rate) {
    if(inputs.keep_time)
      rate = 1.;
    const double dur = std::max(part, min_duration(rate, frames));
    return frames * dur / std::abs(rate);
  };
  p.interval = interval_at(pitch_for(-1));
  // MIDI: each held note at its own pace, as its pitch sets the length of
  // its grains
  if(midi_active)
    for(int n = 0; n < 128; n++)
      if(midi_voices[n].active)
        p.note_interval[n] = interval_at(pitch_for(n));

  p.gain_step = 1. - std::exp(-1. / (0.005 * samplerate));
  return p;
}

int Granola::play(Grain& g, int s0, int n, int b0, const tick_plan& p) noexcept
{
  const int C = g.channels();
  std::array<double*, max_channels> samples;
  for(int c = 0; c < C; c++)
    samples[c] = scratch.data() + c * max_block;
  const int count = g.render(n - s0, samples.data(), p.interp);

  // The sound's channels in turn, in every output alike: summed per channel,
  // given to the outputs once for every grain
  const auto& pan = g.m_pan;
  if(pan.all != 0.f)
  {
    if(C == p.n_channels)
      for(int c = 0; c < C; c++)
      {
        double* sh = shared.data() + c * max_block + s0;
        for(int s = 0; s < count; s++)
          sh[s] += samples[c][s] * pan.all;
      }
    else // a grain made when Channels was another count
      for(int j = 0; j < p.out_channels; j++)
      {
        double* out = outputs.audio.samples[j] + b0 + s0;
        const double* in = samples[j % C];
        for(int s = 0; s < count; s++)
          out[s] += in[s] * pan.all;
      }
  }
  // Then the grain at its place among them, all of its channels: none is
  // lost when a grain is at one side
  if(pan.a >= 0 && pan.a + 1 < p.out_channels)
  {
    double* out_a = outputs.audio.samples[pan.a] + b0 + s0;
    double* out_b = outputs.audio.samples[pan.a + 1] + b0 + s0;
    for(int s = 0; s < count; s++)
    {
      double mono = 0.;
      for(int c = 0; c < C; c++)
        mono += samples[c][s];
      mono /= C;
      out_a[s] += mono * pan.at_a;
      out_b[s] += mono * pan.at_b;
    }
  }
  return s0 + count;
}

bool Granola::spawn(Grain& g, tick_plan& p) noexcept
{
  Grain::Params gp;
  // Gain Deviation: some grains quieter, in decibels, down to -24 dB a
  // deviation at 1. Never louder, which would clip: and the level of many
  // grains around Gain would average out.
  if(inputs.gain_j > 0.f)
    gp.amp = std::pow(10., -std::abs(n01(rd)) * inputs.gain_j * 24. / 20.);
  // The note of a MIDI grain
  const int note = midi_active ? midi_pending.first() : -1;
  // A grain of the folder pins its file, which then outlives changes of the
  // folder; Sound Order picks the file of each grain, By MIDI note its note,
  // or the note pressed last of the held ones
  gp.source = p.cur;
  int idx = -1;
  if(p.from_folder)
  {
    const bool by_note = inputs.order.value == decltype(inputs.order.value)::ByNote;
    const int file_note = by_note ? (note >= 0 ? note : clock_note()) : -1;
    idx = file_note >= 0 ? file_of_note(file_note) : next_in_order(p.cur_index);
    if(idx >= 0)
    {
      const auto& snd = inputs.sounds[idx];
      gp.source = GrainSource{snd.data, (long)snd.channels, (double)snd.frames};
      gp.pin = inputs.sounds.pin(idx);
    }
  }
  if(midi_active)
  {
    // The pitch of the note, from the current Pitch, so that glissandi take
    // effect at once
    midi_pending.reset(note);
    gp.rate = grain_rate(pitch_for(note));
    gp.amp *= velocity_gain(midi_voices[note].velocity);
  }
  else
  {
    // Set pitch and Set position: at the velocity of the note they go on with
    using on = std::decay_t<decltype(inputs.on_midi.value)>;
    if(inputs.on_midi.value == on::SetPitch || inputs.on_midi.value == on::SetPosition)
      if(const int n = clock_note(); n >= 0)
        gp.amp *= velocity_gain(midi_held ? midi_voices[n].velocity : latched_velocity);
    gp.rate = grain_rate(pitch_for(-1));
    trigger = false;
  }

  // In the region, in fractions of it; not shorter than the shortest grain
  // of the file it reads, which also keeps the deviation from going under
  // zero, which Grain::set reads as the whole region
  // Scan in folder, Per file: the file's own place, which goes on by as far
  // as Scan went since the grain before
  double scan_at = scan_phase;
  if(p.from_folder && idx >= 0 && file_scan_count > 0
     && inputs.scan_folder.value == decltype(inputs.scan_folder.value)::PerFile)
  {
    double& place = file_scan[idx % file_scan_count];
    place = wrap_scan(place + (scan_travel - scan_travel_at_grain), p.scan_end);
    scan_at = place;
  }
  scan_travel_at_grain = scan_travel;
  const double pos = grain_place(note, scan_at, deviate(inputs.pos_j));
  // Time mode: Duration Time in the file this grain reads, which in a
  // folder may be another length than the one shown
  double dur = grain_duration(gp.source.frames, p.region_len);
  // Keep time: as much more of the sound as the grain reads faster, for it
  // to last as long
  if(inputs.keep_time && ossia::safe_isfinite(gp.rate))
    dur *= std::abs(gp.rate);
  dur = std::max(dur, min_duration(gp.rate, gp.source.frames));
  gp.start = p.region_lo + pos * p.region_len;
  gp.duration = dur;
  if(!p.table_found)
  {
    p.table = table_for(p.window);
    p.table_found = true;
  }
  gp.window = p.window;
  gp.table = p.table;
  gp.wrap = inputs.loopmode;
  gp.channel_offset = p.ch_offset;
  gp.channels = p.n_channels;
  gp.region_start = p.region_lo;
  gp.region_end = p.region_lo + p.region_len;
  last_spawn_frames = gp.source.frames;

  g.set(std::move(gp));
  if(!g.m_active)
    return false;
  grains_made++;
  // Density Deviation: drawn for each grain, applied to the current Density
  density_factor = inputs.dens_j > 0.f ? density_factor_of(n01(rd), inputs.dens_j) : 1.;
  spread_grain(g, p.out_channels);
  return true;
}

double Granola::wrap_scan(double phase, double scan_end) const noexcept
{
  using mode = std::decay_t<decltype(inputs.scan_mode.value)>;
  switch(inputs.scan_mode.value)
  {
    case mode::Loop:
      return phase - scan_end * std::floor(phase / scan_end);
    case mode::PingPong: // there and back
      return phase - 2. * scan_end * std::floor(phase / (2. * scan_end));
    case mode::Straight: // stays at the end it reached
    default:
      return std::clamp(phase, -(double)inputs.pos, std::max(scan_end - inputs.pos, 0.));
  }
}

void Granola::advance_scan(const tick_plan& p) noexcept
{
  scan_phase = wrap_scan(scan_phase + p.scan_step, p.scan_end);
  scan_travel += p.scan_step;
}

void Granola::advance_timing(const tick_plan& p, int s) noexcept
{
  // When an interval has passed, keeping the fraction of a sample past it
  // Density 0: none, the counters going on, so that grains come at once
  // when it is above 0 again
  const auto passed = [](double& counter, double every) {
    const bool done = every > 0. && counter >= every;
    if(done)
      counter = counter - every < every ? counter - every : 0.;
    counter += 1.;
    return done;
  };
  // Within the bounds density_factor_of() gives it whatever set it: never
  // 0, so that any Density above 0 makes grains
  constexpr double most = max_density_factor;
  const double factor = ossia::safe_isnan(density_factor)
                            ? 1.
                            : std::clamp(density_factor, 1. / most, most);
  const double density = density_at(s) * factor;
  // 0 for Density 0: no interval passes
  const auto every = [density](double interval) { return density > 0. ? interval / density : 0.; };
  if(midi_active)
  {
    for(int note = 0; note < 128; note++)
      if(auto& v = midi_voices[note]; v.active)
        if(passed(v.counter, every(p.note_interval[note])))
          midi_pending.set(note);
  }
  else if(inputs.playing)
  {
    if(passed(trigger_counter, every(p.interval)))
      trigger = true;
  }
}

void Granola::operator()(tick t)
{
  // score keeps the values of a cable, automation or OSC as they come: past
  // the controls' ranges, NaN or infinite. They reach indices and the
  // window's math.
  sanitize_inputs();
  stop_replaced_sound();

  // Density from where it was, then as its values in this tick say; the
  // control's own when none came
  density_next = 0;
  if(density_now < 0. || inputs.density.values.empty())
    density_now = inputs.density;

  const bool from_folder = reads_folder();
  int cur_index = -1;
  const GrainSource cur = current_source(from_folder, cur_index);
  notify_current_sound(cur_index, cur);
  ui_shown_index = cur_index;
  // Nothing to play, e.g. a file of the folder being decoded; or not
  // prepared, its buffers not made
  if(!cur || scratch.empty() || free_from.size() != grains.size())
  {
    for(int j = 0; j < (int)outputs.audio.channels; j++)
      std::fill_n(outputs.audio.samples[j], t.frames, 0.);
    // Density as its values of the tick leave it, for the next tick
    density_at(std::numeric_limits<int>::max());
    return;
  }

  read_midi();
  if(inputs.trig)
    trigger = true;
  auto plan = make_plan(cur, cur_index, from_folder);

  // In blocks the buffers made in prepare() hold
  for(int b0 = 0; b0 < t.frames; b0 += max_block)
  {
    const int n = std::min(max_block, t.frames - b0);

    // Gain as heard follows the control in about 5 ms: no zipper noise
    if(gain_now < 0.)
      gain_now = inputs.gain;
    for(int s = 0; s < n; s++)
    {
      gain_now += (inputs.gain - gain_now) * plan.gain_step;
      block_gain[s] = gain_now;
    }
    for(int j = 0; j < plan.live_out_channels; j++)
      std::fill_n(outputs.audio.samples[j] + b0, n, 0.);
    for(int c = 0; c < plan.n_channels; c++)
      std::fill_n(shared.data() + c * max_block, n, 0.);

    // The grains playing, over the block; a voice is free from the sample
    // after its grain's last one
    for(std::size_t i = 0; i < grains.size(); i++)
      free_from[i] = grains[i].m_active ? play(grains[i], 0, n, b0, plan) : 0;

    // Then sample by sample: Scan, the clock, and the grains that start, one
    // a sample at most, in a voice free at that sample. The clock first: a
    // grain is due at the sample its interval ends, or Density's value comes
    for(int s = 0; s < n; s++)
    {
      if(inputs.playing || midi_active)
        advance_scan(plan);
      advance_timing(plan, b0 + s);

      const bool wanted = midi_active ? midi_pending.any() : trigger;
      if(wanted && has_duration() && inputs.rate != 0.)
        for(long k = 0; k < plan.voices; k++)
        {
          const long i = (next_voice + k) % plan.voices;
          if(grains[i].m_active || free_from[i] > s)
            continue;
          next_voice = i + 1;
          if(spawn(grains[i], plan))
            free_from[i] = play(grains[i], s, n, b0, plan);
          break;
        }
    }

    // The part every output shares, and the gain, once for every sample
    for(int j = 0; j < plan.out_channels; j++)
    {
      double* out = outputs.audio.samples[j] + b0;
      const double* sh = shared.data() + (j % plan.n_channels) * max_block;
      for(int s = 0; s < n; s++)
        out[s] = (out[s] + sh[s]) * block_gain[s];
    }
  }

  // Values past the tick's last sample, which a host does not send: kept
  density_at(std::numeric_limits<int>::max());
  outputs.active_grains.value
      = (int)std::ranges::count_if(grains, [](const Grain& g) { return g.m_active; });
  send_playheads(t.frames, cur);
}
}
