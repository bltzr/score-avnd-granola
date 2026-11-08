#include "Granola.hpp"
#include "utils.hpp"

#include <ossia/math/safe_math.hpp>

#include <avnd/introspection/input.hpp>
#include <avnd/introspection/range.hpp>
#include <boost/container/static_vector.hpp>

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
  sampleinterval = 1.0 / samplerate;
  ms2samps = samplerate * 0.001;

  // Every grain Max Voices allows is made here, with its buffers: neither
  // adding nor removing voices allocates or frees on the audio thread. The
  // host prepares again when the outputs change: the grains then play on.
  if(grains.size() != std::size_t(max_voices))
    resize(max_voices);
  if(window_tables.size() != std::size_t(window_tables_count))
    window_tables.resize(window_tables_count);
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
  buf_soft_lock = true; /// TODO: use a mutex or something?
  grains.resize(n);
  clear();
  buf_soft_lock = false;
}

void Granola::set_voice_count(int n)
{
  // Before prepare(), e.g. in the copy of the object the UI keeps
  if(grains.size() < std::size_t(max_voices))
    grains.resize(max_voices);

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
                       ? std::min(sound_channels, NCHAN)
                       : std::clamp(
                           (int)inputs.src_channels, 1,
                           std::max(std::min(sound_channels, NCHAN), 1));
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
  ph.scan = ph.scanning ? std::lround(scanned(inputs.pos + scan_phase) * 65535.) : 0;
  for(long i = 0; i < std::min<long>(inputs.num_voices, (long)grains.size()); i++)
  {
    const auto& g = grains[i];
    // Only the grains of the sound that is shown: random mode plays others
    if(!g.m_active || g.m_source.data != cur.data)
      continue;
    ph.pos[ph.count] = std::lround(g.position() * 65535.);
    ph.amp[ph.count] = std::lround(CLAMP(g.m_last_win, 0., 1.) * 255.);
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
        grains, [&t](const GranuGrain& g) { return g.m_active && g.m_table == &t; });
    if(!read)
    {
      t.build(w);
      return t.accurate ? &t : nullptr;
    }
  }
  return nullptr;
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

void Granola::operator()(tick t)
{
  using namespace std;

  // score keeps the values of a cable, automation or OSC as they come: past
  // the controls' ranges, NaN or infinite. They reach indices and the
  // window's math.
  sanitize_inputs();
  const long voices = std::clamp<long>(inputs.num_voices, 0, (long)grains.size());

  // A changed Sound file is freed once replaced: the grains still reading it
  // stop. The grains of the folder pin their file and play on.
  // By its data rather than its name: the same file loaded again is
  // another one.
  const GrainSource sound{
      inputs.sound.soundfile.data, (long)inputs.sound.soundfile.channels,
      (double)inputs.sound.soundfile.frames};
  if(sound.data != last_sound.data || sound.frames != last_sound.frames
     || sound.channels != last_sound.channels)
  {
    last_sound = sound;
    for(auto& grain : grains)
      if(!grain.m_pin)
        grain.reset();
  }

  // Current sound: the Sound file, or the file of the folder picked by name
  // (in random mode, each grain picks its own and this one is shown).
  const bool from_folder = inputs.source.value == decltype(inputs.source.value)::Folder;
  GrainSource cur{};
  int cur_index = -1;
  if(from_folder)
  {
    cur_index = find_folder_index();
    if(cur_index >= 0)
    {
      const auto& snd = inputs.sounds[cur_index];
      if(snd.status == halp::sound_status::ready)
        cur = GrainSource{snd.data, (long)snd.channels, (double)snd.frames};
    }
  }
  else if(inputs.sound && inputs.sound.channels() > 0)
  {
    cur = GrainSource{
        inputs.sound.soundfile.data, (long)inputs.sound.channels(),
        (double)inputs.sound.frames()};
  }

  // UI: whether the sound shown could be decoded, when that changes or a
  // (re)created panel asks
  if(send_message)
  {
    const auto status = cur_index >= 0 ? inputs.sounds[cur_index].status
                        : cur          ? halp::sound_status::ready
                                       : halp::sound_status::unloaded;
    if(ui_refresh || cur_index != ui_sent_index || status != ui_sent_status
       || (cur_index >= 0 && inputs.sounds.generation() != ui_sent_generation))
    {
      processor_to_ui msg{current_sound{cur_index, status, {}}};
      if(cur_index >= 0)
      {
        auto& cs = ossia::get<current_sound>(msg.message);
        const std::string_view name = inputs.sounds[cur_index].name;
        const auto n = std::min(name.size(), sizeof(cs.name) - 1);
        std::memcpy(cs.name, name.data(), n);
        cs.name[n] = 0;
      }
      send_message(std::move(msg));
      ui_refresh = false;
      ui_sent_index = cur_index;
      ui_sent_status = status;
      ui_sent_generation = inputs.sounds.generation();
    }
  }

  if(!cur)
    return;

  // The sound's channels the grains read, 0 all of them; the outputs, 0 as
  // many: outputs past the channels read take them again in turn, e.g. a
  // mono sound in all of them
  const int n_channels = inputs.src_channels <= 0
                             ? std::min((int)cur.channels, NCHAN)
                             : std::clamp((int)inputs.src_channels, 1, std::min((int)cur.channels, NCHAN));
  const int ch_offset
      = CLAMP(inputs.channel_offset, 0, (int)cur.channels - n_channels);

  // request_channels() changes outputs.audio.channels now but the buffers
  // follow next tick, so only touch channels backed this tick: the fewer of
  // the two. As many as asked for, more or fewer.
  const int live_out_channels = (int)outputs.audio.channels;
  const int wanted_channels = output_channels((int)cur.channels);
  if(live_out_channels != wanted_channels && outputs.audio.request_channels)
    outputs.audio.request_channels(wanted_channels);
  const int out_channels = std::min(wanted_channels, live_out_channels);

  // The window of the grains made in this tick, and its table, found when a
  // grain is made
  const auto window = window_shape::from_pad(
      int(inputs.window_mode.value), inputs.win_coefs.value.x, inputs.win_coefs.value.y);
  const window_table* table = nullptr;
  bool table_found = false;

  if(buf_soft_lock)
  {
    for(int k = 0; k < t.frames; k++)
    {
      for(int i = 0; i < live_out_channels; i++)
      {
        auto out = outputs.audio.channel(i, t.frames);
        out[k] = 0.0;
      }
    }
    return;
  }

  // Process incoming MIDI: update voice state for note on/off
  if(inputs.midi_gate)
  {
    for(const auto& msg : inputs.midi)
    {
      if(msg.bytes.size() < 3)
        continue;
      const uint8_t status = msg.bytes[0] & 0xF0;
      if(status != 0x90 && status != 0x80)
        continue;
      const uint8_t note   = msg.bytes[1] & 0x7F;
      const uint8_t vel    = msg.bytes[2] & 0x7F;

      if(status == 0x90 && vel > 0) // note on: its first grain at once
      {
        auto& v = midi_voices[note];
        v.active = true;
        v.counter = 0;
        v.velocity = vel;
        midi_active = true;
        midi_pending.set(note);
      }
      else if(status == 0x80 || (status == 0x90 && vel == 0)) // note off
      {
        midi_voices[note].active = false;
        midi_pending.reset(note);
        midi_active = std::ranges::any_of(midi_voices, &MidiVoice::active);
      }
    }
  }
  else if(midi_active)
  {
    // Gate closed — kill all active voices to avoid stuck notes
    for(auto& v : midi_voices) v.active = false;
    midi_active = false;
    midi_pending.clear();
  }

  // A Jitter j spreads its control as N(0, j / 4): see the inputs
  // (a normal distribution needs a deviation above 0: scaled after)
  auto spread = [this](float jitter) {
    return jitter > 0.f ? std::normal_distribution<float>(0.f, 0.25f)(rd) * jitter : 0.f;
  };
  // Relative, and never below 0: a negative density would spawn a grain at
  // every sample. Drawn for each grain, applied to the current Density.
  const auto draw_density_factor = [&] {
    density_factor = std::max(1. + spread(inputs.dens_j), 0.);
  };

  // Pitch Deviation, in semitones (6 a deviation at 1) or added to the
  // pitch, then four octaves under it at the lowest: else a grain could read
  // at about no speed, last for minutes and keep a voice; backwards, only by
  // Reverse.
  // Reverse flips the whole rate, not only its jitter, and Reverse Chance
  // flips some grains.
  const auto jittered_rate = [&](float base) {
    // Whole semitones: notes of a scale rather than a detuned smear
    const float rate = inputs.rate_j_semitones
                           ? base * std::exp2(std::round(spread(inputs.rate_j) * 24.f) / 12.f)
                           : std::max(base + spread(inputs.rate_j), base / 16.f);
    bool backwards = inputs.reverse;
    if(inputs.reverse_chance > 0.f
       && std::uniform_real_distribution<float>(0.f, 1.f)(rd) < inputs.reverse_chance)
      backwards = !backwards;
    return backwards ? -rate : rate;
  };

  // Scan: Position moves by itself, through the sound at Scan times its
  // speed. From Position again when it stops, when it starts playing, and
  // when the sound changes, unless it goes on across the folder's sounds.
  const bool new_sound
      = cur.data != scan_source && !(from_folder && inputs.scan_across);
  if(new_sound || (inputs.playing && !scan_was_playing) || inputs.scan == 0.f)
    scan_phase = 0.;
  scan_source = cur.data;
  scan_was_playing = inputs.playing;
  // The region, as parts of the sound, in either order, two samples at least
  const double region_lo
      = std::clamp(std::min(inputs.sample_start.value, inputs.sample_end.value), 0.f, 1.f);
  const double region_hi
      = std::clamp(std::max(inputs.sample_start.value, inputs.sample_end.value), 0.f, 1.f);
  const double region_len
      = std::max(region_hi - region_lo, cur.frames > 0 ? 2. / cur.frames : 1.);

  const double scan_step
      = cur.frames > 0 ? inputs.scan / (cur.frames * region_len) : 0.;

  // In random folder mode, the length of the file the last grain read: the
  // next grain comes as that one's length and Density say, not as the
  // length of the file shown
  const double interval_frames = from_folder && inputs.random && last_spawn_frames > 0.
                                     ? last_spawn_frames
                                     : cur.frames;

  // The shortest grain, as a fraction of a sound of \p frames at a given
  // pitch: a time, not a part of the file, so that long files still give
  // short grains. Two samples of the file at the least.
  const double min_grain_samples = min_grain_seconds * samplerate;
  const auto min_dur = [&](double rate, double frames) {
    return std::max(min_grain_samples * std::abs(rate), 2.) / frames;
  };

  // Samples between grains at a pitch, but for Density: the length of a
  // grain, at least the shortest one, else a Duration ~= 0 (stale or
  // automated) would pack grains back-to-back up to the voice cap
  const auto interval_at = [&](double rate) {
    const double dur = std::max(
        (double)inputs.dur * region_len, min_dur(rate, interval_frames));
    return interval_frames * dur / std::abs(rate);
  };
  const double interval = interval_at(inputs.rate);

  // MIDI: each held note at its own pace, as its pitch sets the length of
  // its grains
  std::array<double, 128> note_interval;
  const auto note_ratio = [](int note) { return std::exp2((note - 60) / 12.f); };
  if(midi_active)
    for(int n = 0; n < 128; n++)
      if(midi_voices[n].active)
        note_interval[n] = interval_at(inputs.rate * note_ratio(n));

  if(inputs.trig)
    trigger = true;

  // Gain as heard follows the control in about 5 ms: no zipper noise
  if(gain_now < 0.)
    gain_now = inputs.gain;
  const double gain_step = 1. - std::exp(-1. / (0.005 * samplerate));

  const double scan_end = scan_last();
  for(int k = 0; k < t.frames; k++)
  {
    if(inputs.playing || midi_active)
    {
      scan_phase += scan_step;
      using mode = std::decay_t<decltype(inputs.scan_mode.value)>;
      switch(inputs.scan_mode.value)
      {
        case mode::Loop:
          scan_phase -= scan_end * std::floor(scan_phase / scan_end);
          break;
        case mode::PingPong: // there and back
          scan_phase -= 2. * scan_end * std::floor(scan_phase / (2. * scan_end));
          break;
        case mode::Straight: // stays at the end it reached
          scan_phase = std::clamp(
              scan_phase, -(double)inputs.pos, std::max(scan_end - inputs.pos, 0.));
          break;
      }
    }
    gain_now += (inputs.gain - gain_now) * gain_step;

    alloccheck = false;
    busyCount = 0;

    for(int i = 0; i < live_out_channels; i++)
      outputs.audio.samples[i][k] = 0.;

    // The voices past Max Voices only fade out: no grain is made in them
    for(long i = 0; i < (long)grains.size(); i++)
    {
      const bool should_spawn = midi_active ? midi_pending.any() : trigger;
      if(i < voices && !alloccheck && should_spawn && busyCount < voices
         && inputs.dur != 0. && inputs.rate != 0.)
      {
        if(!grains[i].m_active)
        {
          float rate;
          boost::container::static_vector<double, NCHAN> spawn_ampvec(n_channels, 1.);
          // Gain Deviation: some grains quieter, in decibels, down to -24 dB
          // a deviation at 1. Never louder, which would clip: and the level
          // of many grains around Gain would average out.
          if(inputs.gain_j > 0.f)
          {
            const double g = std::pow(
                10., -std::abs(std::normal_distribution<double>(0., 1.)(rd))
                         * inputs.gain_j * 24. / 20.);
            for(auto& a : spawn_ampvec)
              a *= g;
          }
          // A grain of the folder pins its file, which then outlives changes
          // of the folder; random mode picks a file per grain.
          GrainSource spawn_src = cur;
          halp::sound_pin spawn_pin;
          if(from_folder)
          {
            const int idx = inputs.random ? random_ready_index() : cur_index;
            if(idx >= 0)
            {
              const auto& snd = inputs.sounds[idx];
              spawn_src = GrainSource{snd.data, (long)snd.channels, (double)snd.frames};
              spawn_pin = inputs.sounds.pin(idx);
            }
          }
          if(midi_active)
          {
            // The pitch of the note, from the current Pitch, so that
            // glissandi take effect at once
            const int note = midi_pending.first();
            midi_pending.reset(note);
            rate = jittered_rate(inputs.rate * note_ratio(note));

            // Square-law velocity: perceptual gain, in the grain's level
            const float vel = midi_voices[note].velocity / 127.f;
            for(auto& a : spawn_ampvec)
              a *= vel * vel;
          }
          else
          {
            rate = jittered_rate(inputs.rate);
            trigger = false;
          }

          // In the region, in fractions of it; not shorter than the shortest
          // grain of the file it reads, which also keeps the deviation from
          // going under zero, which GranuGrain::set reads as the whole file
          float pos = inputs.pos + float(scan_phase) + spread(inputs.pos_j);
          float dur = (inputs.dur + spread(inputs.dur_j)) * region_len;
          dur = std::max(dur, float(min_dur(rate, spawn_src.frames)));

          // With Scan, as its mode goes on past the region's ends; else
          // never outside of it
          pos = inputs.scan != 0.f ? scanned(pos) : std::clamp(pos, 0.f, 1.f);
          pos = float(region_lo + pos * region_len);
          if(!table_found)
          {
            table = table_for(window);
            table_found = true;
          }
          grains[i].set(pos, dur, rate,
              window, table, spawn_ampvec, spawn_src, std::move(spawn_pin),
              inputs.loopmode, ch_offset, n_channels, region_lo,
              region_lo + region_len);
          last_spawn_frames = spawn_src.frames;
          grains_made++;
          draw_density_factor();

          // Spread: a place among the output channels
          const int places = out_channels;
          auto& pan = grains[i].m_pan;
          pan = {};
          // 0: in all of them alike; 1: between two neighbouring ones (1-D
          // VBAP); 2: in one of them
          if(const float sp = std::clamp(inputs.spread.value, 0.f, 2.f); sp > 0.f && places > 1)
          {
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
          alloccheck = true;
        }
      }

      if(grains[i].m_active)
      {
        std::span<double> outSamps{
            grains[i].incr((long)inputs.interp_type.value)};

        // The sound's channels in turn, in every output alike; then the
        // grain at its place among them, all of its channels: none is lost
        // when a grain is at one side
        const int read = std::max((int)outSamps.size(), 1);
        const auto& pan = grains[i].m_pan;
        if(const double all = gain_now * pan.all; all != 0.)
          for(int j = 0; j < out_channels; j++)
            outputs.audio.samples[j][k] += outSamps[j % read] * all;
        if(pan.a >= 0 && pan.a + 1 < out_channels)
        {
          double mono = 0.;
          for(int c = 0; c < read; c++)
            mono += outSamps[c];
          mono *= gain_now / read;
          outputs.audio.samples[pan.a][k] += mono * pan.at_a;
          outputs.audio.samples[pan.a + 1][k] += mono * pan.at_b;
        }

        busyCount++;
      }
    }

    const double density = inputs.density * density_factor;
    if(midi_active)
    {
      // Each held note when its interval has passed, keeping the fraction of
      // a sample past it
      for(int n = 0; n < 128; n++)
      {
        auto& v = midi_voices[n];
        if(!v.active)
          continue;
        const double every = note_interval[n] / density;
        if(v.counter >= every)
        {
          midi_pending.set(n);
          v.counter = v.counter - every < every ? v.counter - every : 0.;
        }
        v.counter += 1.;
      }
    }
    else if(inputs.playing)
    {
      const double every = interval / density;
      if(trigger_counter >= every)
      {
        trigger = true;
        trigger_counter = trigger_counter - every < every ? trigger_counter - every : 0.;
      }
      trigger_counter += 1.;
    }
  }

  outputs.active_grains = busyCount;

  send_playheads(t.frames, cur);

  //qDebug() << "Busy count:" << busyCount << " / density: " << density << " / " << inputs.density;

  //}

  /*
  // Process the input buffer
  for (int i = 0; i < n_channels; i++)
  {
      // Take the ith channel of the soundfile.
      // in is a std::span
      const auto in = inputs.sound.channel(i+ch_offset);

      // We'll read at this position
      const int64_t start = std::floor(inputs.pos * inputs.sound.frames());
      int64_t end_raw = std::ceil((inputs.pos + inputs.dur) * inputs.sound.frames());
      const int64_t end = (end_raw < inputs.sound.frames()) ? end_raw : inputs.sound.frames();

      // Output buffer for channel i, also a std::span.
      auto out = outputs.audio.channel(i, t.frames);

      for (int j = 0; j < t.frames; j++)
      {

        // If we're before the end of the file copy the sample
        if(start + j < end)
          out[j] = inputs.gain * in[start + j];
        else
          out[j] = 0;
      }

  }
  */
}
}
