#include "Granola.hpp"
#include "utils.hpp"

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
  // adding nor removing voices allocates or frees on the audio thread.
  resize(max_voices);
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

  // The voices past the count stop
  n = std::clamp(n, 0, max_voices);
  for(std::size_t i = n; i < grains.size(); i++)
    grains[i].reset();
}

void Granola::clear()
{
  for(auto& grain : grains)
    grain.reset();
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
  ph.scan = inputs.scan != 0.f ? std::lround(scan_phase * 65535.) : 0;
  for(long i = 0; i < inputs.num_voices && i < (long)grains.size(); i++)
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

void Granola::operator()(tick t)
{
  using namespace std;

  // A changed Sound file is freed once replaced: the grains still reading it
  // stop. The grains of the folder pin their file and play on.
  const std::string_view snd_path = inputs.sound.soundfile.filename;
  const std::size_t snd_path_hash = std::hash<std::string_view>{}(snd_path);
  if(snd_path_hash != last_sound_path)
  {
    last_sound_path = snd_path_hash;
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
                             : CLAMP(inputs.src_channels, 1, (int)cur.channels);
  const int n_out = inputs.out_channels <= 0
                        ? n_channels
                        : std::min((int)inputs.out_channels, max_outputs);
  const int ch_offset
      = CLAMP(inputs.channel_offset, 0, (int)cur.channels - n_channels);

  // request_channels() bumps outputs.audio.channels now but the buffers only
  // grow next tick, so only touch channels already backed this tick.
  const int live_out_channels = (int)outputs.audio.channels;
  // Spread needs two channels, even for a mono sound
  const int wanted_channels = inputs.spread > 0.f ? std::max(n_out, 2) : n_out;
  if(live_out_channels < wanted_channels && outputs.audio.request_channels)
    outputs.audio.request_channels(wanted_channels);
  // The outputs the grains play in: a single channel in all of them, as
  // before Outputs
  const int out_channels
      = n_channels == 1 ? live_out_channels : std::min(wanted_channels, live_out_channels);

  boost::container::static_vector<double, NCHAN> ampvec(n_channels, 1.);
  for(int i = 0; i < n_channels; i++)
  {
    // ampvec[i] = 1.0f; useDefaultAmp ? 1.0f : amps[i]; // add this when we want to use amp vectors
    // amps[i][j] if we use vectors from an audio port (as in granubuf_mc)
    // also move below in for (int j = 0; j < t.frames; j++)

    maxAmp = ampvec[i] > maxAmp ? ampvec[i] : maxAmp;
  }

  boost::container::static_vector<double, 2> windcoef(2, 1.);

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

      if(status == 0x90 && vel > 0) // note on
      {
        auto& v = midi_voices[note];
        v.active = true;
        v.trigger_counter = 0;
        v.velocity = vel;
        midi_active = true;
      }
      else if(status == 0x80 || (status == 0x90 && vel == 0)) // note off
      {
        midi_voices[note].active = false;
        midi_active = false;
        for(const auto& v : midi_voices)
          if(v.active) { midi_active = true; break; }
      }
    }
  }
  else if(midi_active)
  {
    // Gate closed — kill all active voices to avoid stuck notes
    for(auto& v : midi_voices) v.active = false;
    midi_active = false;
    midi_pending_voice = -1;
  }

  // A Jitter j spreads its control as N(0, j / 4): see the inputs
  // (a normal distribution needs a deviation above 0: scaled after)
  auto spread = [this](float jitter) {
    return jitter > 0.f ? std::normal_distribution<float>(0.f, 0.25f)(rd) * jitter : 0.f;
  };
  // Relative, and never below 0: a negative density would spawn a grain at
  // every sample
  const auto jittered_density = [&] {
    return inputs.density * std::max(1. + spread(inputs.dens_j), 0.);
  };
  double density = jittered_density();

  // Pitch Jitter, in semitones (6 a deviation at 1) or added to the pitch.
  // Reverse flips the whole rate, not only its jitter, and Reverse Chance
  // flips some grains.
  const auto jittered_rate = [&](float base) {
    // Whole semitones: notes of a scale rather than a detuned smear
    const float rate = inputs.rate_j_semitones
                           ? base * std::exp2(std::round(spread(inputs.rate_j) * 24.f) / 12.f)
                           : base + spread(inputs.rate_j);
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

  // The shortest grain, as a fraction of the sound at a given pitch: a time,
  // not a part of the file, so that long files still give short grains. Two
  // samples of the file at the least.
  const double min_grain_samples = min_grain_seconds * samplerate;
  const auto min_dur = [&](double rate) {
    return std::max(min_grain_samples * std::abs(rate), 2.) / cur.frames;
  };

  // Duration for the spawn timing, clamped up to the shortest grain: else a
  // Duration ~= 0 (stale or automated) drops the spawn interval to 1 sample and
  // packs grains back-to-back up to the voice cap, and Density stops meaning
  // anything.
  const double eff_dur
      = std::max((double)inputs.dur * region_len, min_dur(inputs.rate));

  if (inputs.trig ) {
    trigger = true;
    /*trigger_counter = inputs.sound.frames() * inputs.dur
               / (density * ((inputs.rate < 0) ?
                     -inputs.rate : inputs.rate));
    //qDebug() << " trigger ! " ;*/
  }

  for(int k = 0; k < t.frames; k++)
  {
    if(inputs.playing || midi_active)
    {
      scan_phase += scan_step;
      scan_phase -= std::floor(scan_phase);
    }

    alloccheck = false;
    maxAmp = 1;
    busyCount = 0;

    //qDebug() << " trigger counter " << trigger_counter;

    for(int i = 0; i < live_out_channels; i++)
    {
      outputs.audio.samples[i][k] = 0.;
    }

    for(long i = 0; i < inputs.num_voices; i++)
    {
      const bool should_spawn = midi_active ? (midi_pending_voice >= 0)
                                            : trigger;
      if(!alloccheck && should_spawn
         && busyCount < inputs.num_voices && maxAmp > 0.
         && inputs.dur != 0. && inputs.rate != 0.)
      {
        if(!grains[i].m_active)
        {
          windcoef[0] = 1.
                        + inputs.win_coefs.value.y * wc_radius
                              * std::cos((1-inputs.win_coefs.value.x) * PI / 2.);
          windcoef[1] = 1.
                        + inputs.win_coefs.value.y * wc_radius
                              * std::sin((1-inputs.win_coefs.value.x) * PI / 2.);

          // In the region: fractions of it
          float pos = inputs.pos + float(scan_phase) + spread(inputs.pos_j);
          float dur = (inputs.dur + spread(inputs.dur_j)) * region_len;
          float rate;
          boost::container::static_vector<double, NCHAN> spawn_ampvec = ampvec;
          // Gain jitter: some grains quieter, in decibels, down to -24 dB a
          // deviation at 1. Never louder, which would clip: and the level
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
            // Recompute pitch from current inputs.rate so glissandi take effect immediately
            float base = inputs.rate * std::pow(2.f, (midi_pending_voice - 60) / 12.f);
            // Reverse flips the whole rate, not only its jitter
            rate = jittered_rate(base);

            // Square-law velocity → perceptual gain, baked into the grain amplitude
            const float vel_gain = std::pow(midi_voices[midi_pending_voice].velocity / 127.f, 2.f);
            for(auto& a : spawn_ampvec)
              a *= vel_gain;

            // advance to the next active voice
            ++midi_pending_voice;
            while(midi_pending_voice < 128 && !midi_voices[midi_pending_voice].active)
              ++midi_pending_voice;
            if(midi_pending_voice >= 128)
              midi_pending_voice = -1;
          }
          else
          {
            rate = jittered_rate(inputs.rate);
            trigger = false;
          }

          // Not shorter than the shortest grain: this also keeps the jitter
          // from going under zero, which GranuGrain::set reads as the whole
          // file.
          dur = std::max(dur, float(min_dur(rate)));

          // Past the end with Scan: around again
          // Past its end with Scan: around again; else never outside of it
          if(inputs.scan != 0.f)
            pos -= std::floor(pos);
          else
            pos = std::clamp(pos, 0.f, 1.f);
          pos = float(region_lo + pos * region_len);
          grains[i].set(pos, dur, rate,
              windcoef, spawn_ampvec, spawn_src, std::move(spawn_pin),
              inputs.loopmode, inputs.window_mode, ch_offset, n_channels, region_lo,
              region_lo + region_len);

          // Spread: a place x among the output channels, between two of
          // them; Spread s mixes it with all the channels alike, as at 0
          const int places = out_channels;
          auto& pan = grains[i].m_pan;
          pan = {};
          if(inputs.spread > 0.f && places > 1)
          {
            const float x = std::uniform_real_distribution<float>(
                0.f, float(places - 1))(rd);
            const int a = std::min(int(x), places - 2);
            const float t = x - a;
            // Between a and a + 1: 1 at the centre, as the channels are now
            constexpr float sqrt2 = std::numbers::sqrt2_v<float>;
            constexpr float half_pi = std::numbers::pi_v<float> / 2.f;
            pan.all = 1.f - inputs.spread;
            pan.a = a;
            pan.at_a = inputs.spread * std::min(1.f, sqrt2 * std::cos(t * half_pi));
            pan.at_b = inputs.spread * std::min(1.f, sqrt2 * std::sin(t * half_pi));
          }
          alloccheck = true;
        }
      }

       if(grains[i].m_active)
      {

        std::span<double> outSamps{
            grains[i].incr((long)inputs.interp_type.value)};

        // The sound's channels in turn, in every output alike, then the
        // grain's place among them
        const int read = std::max((int)outSamps.size(), 1);
        const auto& pan = grains[i].m_pan;
        if(const double all = inputs.gain * pan.all; all != 0.)
          for(int j = 0; j < out_channels; j++)
            outputs.audio.samples[j][k] += outSamps[j % read] * all;
        if(pan.a >= 0 && pan.a + 1 < out_channels)
        {
          const int a = pan.a, b = pan.a + 1;
          outputs.audio.samples[a][k] += outSamps[a % read] * inputs.gain * pan.at_a;
          outputs.audio.samples[b][k] += outSamps[b % read] * inputs.gain * pan.at_b;
        }

        busyCount++;
      }

    }

    if(midi_active)
    {
      // One shared trigger fires all active notes simultaneously.
      // Grains are spawned one per sample over consecutive samples (via
      // midi_pending_voice), so a chord of N notes costs N samples to fully spawn.
      if(trigger_counter
         >= std::max(cur.frames * eff_dur / (density * inputs.rate), 1.0))
      {
        // start scanning from the first active voice
        midi_pending_voice = 0;
        while(midi_pending_voice < 128 && !midi_voices[midi_pending_voice].active)
          ++midi_pending_voice;
        if(midi_pending_voice >= 128)
          midi_pending_voice = -1;
        trigger_counter = 0;
        density = jittered_density();
      }
      ++trigger_counter;
    }
    else if(inputs.playing)
    {
      if(trigger_counter
         >= std::max(cur.frames * eff_dur / (density * inputs.rate), 1.0))
      {
        trigger = true;
        trigger_counter = 0;
        density = jittered_density();
      }
      ++trigger_counter;
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
