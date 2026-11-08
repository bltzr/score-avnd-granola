#include "Granola.hpp"

#include <ossia/math/safe_math.hpp>

#include <avnd/introspection/input.hpp>
#include <avnd/introspection/range.hpp>

#include <algorithm>
#include <cstring>
#include <functional>
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
  set_voice_count(inputs.num_voices);
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
  // Spread needs two channels, even for a mono sound
  return inputs.spread > 0.f ? std::max(n, 2) : n;
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
  ph.scanning = inputs.scan != 0.f;
  ph.scan = ph.scanning
                ? uint16_t(std::lround(scanned(inputs.pos + scan_phase) * playheads::pos_scale))
                : 0;
  for(long i = 0; i < std::min<long>(inputs.num_voices, (long)grains.size()); i++)
  {
    const auto& g = grains[i];
    // Only the grains of the sound that is shown: random mode plays others
    if(!g.m_active || g.m_source.data != cur.data)
      continue;
    ph.pos[ph.count] = uint16_t(std::lround(g.position() * playheads::pos_scale));
    ph.amp[ph.count]
        = uint8_t(std::lround(std::clamp(g.m_last_win, 0., 1.) * playheads::amp_scale));
    if(++ph.count == playheads::max_heads)
      break;
  }

  // Nothing plays: one empty message clears the playheads, then nothing
  if(ph.count == 0 && !ui_heads_shown)
    return;
  ui_heads_shown = ph.count > 0;
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
  // random mode, each grain picks its own and this one is shown)
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
  if(!inputs.midi_gate)
  {
    // Gate closed: every note off, none stuck
    if(midi_active)
    {
      for(auto& v : midi_voices)
        v.active = false;
      midi_active = false;
      midi_pending.clear();
    }
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
      midi_active = true;
      midi_pending.set(note);
    }
    else if(type == message_type::NOTE_OFF || type == message_type::NOTE_ON)
    {
      midi_voices[note].active = false;
      midi_pending.reset(note);
      midi_active = std::ranges::any_of(midi_voices, &MidiVoice::active);
    }
  }
}

float Granola::deviate(float deviation) noexcept
{
  // N(0, d / 4): a normal distribution needs a deviation above 0, so it is
  // scaled after
  return deviation > 0.f ? n01(rd) * 0.25f * deviation : 0.f;
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
  return std::max(density_now, min_density);
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
  // Backwards only by Reverse, which flips every grain, and Reverse Chance,
  // which flips some
  bool backwards = inputs.reverse;
  if(inputs.reverse_chance > 0.f
     && std::uniform_real_distribution<float>(0.f, 1.f)(rd) < inputs.reverse_chance)
    backwards = !backwards;
  return backwards ? -rate : rate;
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

  // Scan: Position moves by itself, through the region at Scan times the
  // sound's speed. From Position again when it stops, when it starts
  // playing, and when the sound changes, unless it goes on across the
  // folder's sounds.
  const bool new_sound = cur.data != scan_source && !(from_folder && inputs.scan_across);
  if(new_sound || (inputs.playing && !scan_was_playing) || inputs.scan == 0.f)
    scan_phase = 0.;
  scan_source = cur.data;
  scan_was_playing = inputs.playing;
  p.scan_step = inputs.scan / (cur.frames * p.region_len);
  p.scan_end = scan_last();

  // In random folder mode, the length of the file the last grain read: the
  // next grain comes as that one's length and Density say, not as the
  // length of the file shown
  const double frames
      = from_folder && inputs.random && last_spawn_frames > 0. ? last_spawn_frames : cur.frames;
  // Samples between grains at a pitch, but for Density: the length of a
  // grain, at least the shortest one, else a Duration ~= 0 (stale or
  // automated) would pack grains back-to-back up to the voice cap
  const auto interval_at = [&](double rate) {
    const double dur
        = std::max((double)inputs.dur * p.region_len, min_duration(rate, frames));
    return frames * dur / std::abs(rate);
  };
  p.interval = interval_at(inputs.rate);
  // MIDI: each held note at its own pace, as its pitch sets the length of
  // its grains
  if(midi_active)
    for(int n = 0; n < 128; n++)
      if(midi_voices[n].active)
        p.note_interval[n] = interval_at(inputs.rate * note_ratio(n));

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
  // A grain of the folder pins its file, which then outlives changes of the
  // folder; random mode picks a file per grain
  gp.source = p.cur;
  if(p.from_folder)
  {
    const int idx = inputs.random ? random_ready_index() : p.cur_index;
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
    const int note = midi_pending.first();
    midi_pending.reset(note);
    gp.rate = grain_rate(inputs.rate * note_ratio(note));
    // Square-law velocity: perceptual gain, in the grain's level
    const double vel = midi_voices[note].velocity / 127.;
    gp.amp *= vel * vel;
  }
  else
  {
    gp.rate = grain_rate(inputs.rate);
    trigger = false;
  }

  // In the region, in fractions of it; not shorter than the shortest grain
  // of the file it reads, which also keeps the deviation from going under
  // zero, which Grain::set reads as the whole region
  double pos = inputs.pos + scan_phase + deviate(inputs.pos_j);
  double dur = (inputs.dur + deviate(inputs.dur_j)) * p.region_len;
  dur = std::max(dur, min_duration(gp.rate, gp.source.frames));
  // With Scan, as its mode goes on past the region's ends; else never
  // outside of it
  pos = inputs.scan != 0.f ? scanned(pos) : std::clamp(pos, 0., 1.);
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

void Granola::advance_scan(const tick_plan& p) noexcept
{
  scan_phase += p.scan_step;
  using mode = std::decay_t<decltype(inputs.scan_mode.value)>;
  switch(inputs.scan_mode.value)
  {
    case mode::Loop:
      scan_phase -= p.scan_end * std::floor(scan_phase / p.scan_end);
      break;
    case mode::PingPong: // there and back
      scan_phase -= 2. * p.scan_end * std::floor(scan_phase / (2. * p.scan_end));
      break;
    case mode::Straight: // stays at the end it reached
      scan_phase
          = std::clamp(scan_phase, -(double)inputs.pos, std::max(p.scan_end - inputs.pos, 0.));
      break;
  }
}

void Granola::advance_timing(const tick_plan& p, int s) noexcept
{
  // When an interval has passed, keeping the fraction of a sample past it
  const auto passed = [](double& counter, double every) {
    const bool done = counter >= every;
    if(done)
      counter = counter - every < every ? counter - every : 0.;
    counter += 1.;
    return done;
  };
  // Within the bounds density_factor_of() gives it whatever set it: never 0
  constexpr double most = max_density_factor;
  const double factor = ossia::safe_isnan(density_factor)
                            ? 1.
                            : std::clamp(density_factor, 1. / most, most);
  const double density = std::max(density_at(s) * factor, min_density);
  if(midi_active)
  {
    for(int note = 0; note < 128; note++)
      if(auto& v = midi_voices[note]; v.active)
        if(passed(v.counter, p.note_interval[note] / density))
          midi_pending.set(note);
  }
  else if(inputs.playing)
  {
    if(passed(trigger_counter, p.interval / density))
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

  const bool from_folder = reads_folder();
  int cur_index = -1;
  const GrainSource cur = current_source(from_folder, cur_index);
  notify_current_sound(cur_index, cur);
  // Nothing to play, e.g. a file of the folder being decoded; or not
  // prepared, its buffers not made
  if(!cur || scratch.empty() || free_from.size() != grains.size())
  {
    // Nothing to play, e.g. a file of the folder being decoded
    for(int j = 0; j < (int)outputs.audio.channels; j++)
      std::fill_n(outputs.audio.samples[j], t.frames, 0.);
    return;
  }

  read_midi();
  if(inputs.trig)
    trigger = true;
  auto plan = make_plan(cur, cur_index, from_folder);
  // Density from where it was, then as its values in this tick say; the
  // control's own when none came
  density_next = 0;
  if(density_now < 0. || inputs.density.values.empty())
    density_now = inputs.density;

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

    // Then sample by sample: Scan, and the grains that start, one a sample
    // at most, in a voice free at that sample
    for(int s = 0; s < n; s++)
    {
      if(inputs.playing || midi_active)
        advance_scan(plan);

      const bool wanted = midi_active ? midi_pending.any() : trigger;
      if(wanted && inputs.dur != 0. && inputs.rate != 0.)
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

      advance_timing(plan, b0 + s);
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

  outputs.active_grains
      = (int)std::ranges::count_if(grains, [](const Grain& g) { return g.m_active; });
  send_playheads(t.frames, cur);
}
}
