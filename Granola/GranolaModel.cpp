#include "Granola.hpp"
#include "utils.hpp"

#include <boost/container/static_vector.hpp>

namespace Granola
{

void Granola::prepare(setup info)
{
  samplerate = info.rate;
  sampleinterval = 1.0 / samplerate;
  ms2samps = samplerate * 0.001;

  // create the appropriate number of grains:
  grains.reserve(256);
  resize(inputs.num_voices);

  if(inputs.multi)
    request_scan();
}

void Granola::request_scan()
{
  if(bank_scan_inflight)
    return;
  bank_scan_inflight = true;
  auto rq = std::make_shared<scan_request>();
  rq->folder = std::string(inputs.sound.soundfile.filename);
  rq->rate = samplerate;
  rq->previous = bank;
  worker.request(std::move(rq));
}

void Granola::resize(int n)
{
  buf_soft_lock = true; /// TODO: use a mutex or something?
  grains.resize(n);
  clear();
  buf_soft_lock = false;
}

void Granola::clear()
{
  for(auto& grain : grains)
    grain.reset();
}

void Granola::operator()(tick t)
{
  using namespace std;

  // A changed Sound-port file resets active grains: in mono mode a grain
  // captured the previous (now-freed) buffer; the clear makes new grains
  // capture the current one. (Bank grains hold their sound, but a file change
  // also means a folder change, so the reset is harmless there too.)
  const std::string_view snd_path = inputs.sound.soundfile.filename;
  if(snd_path != last_sound_path)
  {
    last_sound_path = snd_path;
    clear();
  }

  // Folder rescan only in multifile mode (worker thread; ~2x/second), immediate
  // when the folder changes. Mono mode never decodes the folder.
  bank_scan_phase += t.frames;
  if(inputs.multi
     && (bank.folder != snd_path || bank_scan_phase >= (long)(samplerate / 2)))
  {
    bank_scan_phase = 0;
    request_scan();
  }

  // Current sound: in multifile mode, the bank entry chosen by Sound index (or
  // per-grain random); otherwise the single Sound-port file.
  GrainSource cur{};
  std::shared_ptr<const void> cur_hold{};
  const BankSound* cur_bank_snd = nullptr;
  const bool random_pick = inputs.multi && !bank.sounds.empty() && inputs.random;
  if(inputs.multi && !bank.sounds.empty())
  {
    const int N = (int)bank.sounds.size();
    const int idx = random_pick ? 0 : CLAMP(inputs.sound_index.value, 0, N - 1);
    cur = bank.sounds[idx]->view();
    cur_hold = bank.sounds[idx];
    cur_bank_snd = bank.sounds[idx].get();
  }
  else if(inputs.sound && inputs.sound.channels() > 0)
  {
    cur = GrainSource{
        inputs.sound.soundfile.data, (long)inputs.sound.channels(),
        (double)inputs.sound.frames()};
  }
  if(!cur)
    return;

  // Waveform UI: ship the current sound's envelope when it changes (bank entry
  // or mono file) or a freshly (re)created panel asks for a refresh.
  if(send_message)
  {
    const std::string_view cur_name
        = cur_bank_snd ? std::string_view(cur_bank_snd->name) : snd_path;
    const int64_t cur_mtime = cur_bank_snd ? cur_bank_snd->mtime : 0;
    if(ui_refresh || ui_sound_name != cur_name || ui_sound_mtime != cur_mtime)
    {
      processor_to_ui msg;
      msg.name = std::string(cur_name);
      if(cur_bank_snd)
      {
        msg.min_peaks = cur_bank_snd->min_peaks;
        msg.max_peaks = cur_bank_snd->max_peaks;
        msg.duration_s = cur_bank_snd->duration_s;
      }
      else
      {
        // mono: recompute the envelope from the Sound port when the file changes
        if(mono_peaks_path != snd_path)
        {
          compute_peaks_raw(
              inputs.sound.soundfile.data, (int)inputs.sound.channels(),
              (int64_t)inputs.sound.frames(), samplerate, mono_min_peaks,
              mono_max_peaks, mono_duration_s);
          mono_peaks_path = std::string(snd_path);
        }
        msg.min_peaks = mono_min_peaks;
        msg.max_peaks = mono_max_peaks;
        msg.duration_s = mono_duration_s;
      }
      send_message(std::move(msg));
      ui_refresh = false;
      ui_sound_name = cur_name;
      ui_sound_mtime = cur_mtime;
    }
  }

  const int n_channels = CLAMP(inputs.src_channels, 1, (int)cur.channels);
  const int ch_offset
      = CLAMP(inputs.channel_offset, 0, (int)cur.channels - n_channels);

  // request_channels() bumps outputs.audio.channels now but the buffers only
  // grow next tick, so only touch channels already backed this tick.
  const int live_out_channels = (int)outputs.audio.channels;
  if(live_out_channels < n_channels && outputs.audio.request_channels)
    outputs.audio.request_channels(n_channels);
  const int out_channels = std::min(n_channels, live_out_channels);

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

  auto dist = std::normal_distribution<float>(0., inputs.dens_j_r / 4);

  double density = inputs.density * (1 + dist(rd) * inputs.dens_j);

  // Effective base duration for spawn timing. Two corrections that keep the
  // active-grain count ~= Density at short durations and under jitter:
  //  - clamp sub-minimum (stale / automated) values up to the slider minimum,
  //    so Duration ~= 0 can't drop the spawn interval to 1 sample (which packs
  //    ~64-sample grains back-to-back into a degenerate grain pile);
  //  - add the mean of the add-only duration jitter. The jitter is
  //    |N(0, DurJitterRange/4)| * DurJitter, whose mean is
  //    (DurJitterRange/4)*sqrt(2/pi)*DurJitter ~= 0.2*DurJitterRange*DurJitter,
  //    so the spawn rate tracks the AVERAGE jittered grain length.
  const double dur_base = std::max((double)inputs.dur, 0.01);
  const double eff_dur
      = dur_base + 0.2 * (double)inputs.dur_j_r * (double)inputs.dur_j;

  if (inputs.trig ) {
    trigger = true;
    /*trigger_counter = inputs.sound.frames() * inputs.dur
               / (density * ((inputs.rate < 0) ?
                     -inputs.rate : inputs.rate));
    //qDebug() << " trigger ! " ;*/
  }

  for(int k = 0; k < t.frames; k++)
  {

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

          float pos = inputs.pos + std::normal_distribution<float>
                                   (0., inputs.pos_j_r / 4)(rd) * inputs.pos_j;
          // Duration jitter is add-only (grains only lengthen, never trip the
          // "whole file" path GranuGrain::set reads for a <= 0 duration).
          // dur_base is already clamped to the minimum.
          float dur = dur_base
                      + std::abs(std::normal_distribution<float>
                                 (0., inputs.dur_j_r / 4)(rd) * inputs.dur_j);
          dur = std::max(dur, 64.f / (float)cur.frames);
          float rate;
          boost::container::static_vector<double, NCHAN> spawn_ampvec = ampvec;
          // Per-grain source: random mode picks a fresh bank sound per grain.
          GrainSource spawn_src = cur;
          std::shared_ptr<const void> spawn_hold = cur_hold;
          if(random_pick)
          {
            const auto& rsnd = bank.sounds[rd() % bank.sounds.size()];
            spawn_src = rsnd->view();
            spawn_hold = rsnd;
          }
          if(midi_active)
          {
            // Recompute pitch from current inputs.rate so glissandi take effect immediately
            float base = inputs.rate * std::pow(2.f, (midi_pending_voice - 60) / 12.f);
            rate = base + std::normal_distribution<float>
                          (0., inputs.rate_j_r / 4)(rd) * inputs.rate_j
                                          * ((inputs.reverse) ? -1 : 1);

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
            rate = inputs.rate + std::normal_distribution<float>
                                 (0., inputs.rate_j_r / 4)(rd) * inputs.rate_j
                                                 * ((inputs.reverse) ? -1 : 1);
            trigger = false;
          }

          grains[i].set(pos, dur, rate,
              windcoef, spawn_ampvec, spawn_src, spawn_hold,
              inputs.loopmode, inputs.window_mode, ch_offset, n_channels);
          alloccheck = true;
        }
      }

       if(grains[i].m_active)
      {

        std::span<double> outSamps{
            grains[i].incr((long)inputs.interp_type.value)};

        const int nw = std::min(out_channels, (int)outSamps.size());
        for(int j = 0; j < nw; j++)
        {
          outputs.audio.samples[j][k] += outSamps[j] * inputs.gain;
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
        density = inputs.density * (1 + dist(rd) * inputs.dens_j);
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
        density = inputs.density * (1 + dist(rd) * inputs.dens_j);
      }
      ++trigger_counter;
    }

  }

  outputs.active_grains = busyCount;

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
