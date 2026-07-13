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

  request_scan();
}

void Granola::request_scan()
{
  if(bank_scan_inflight)
    return;
  bank_scan_inflight = true;
  auto rq = std::make_shared<scan_request>();
  rq->folder = inputs.sound_folder.value;
  rq->map_file = inputs.midi_map.value;
  rq->params_set = inputs.params_set.value;
  rq->rate = samplerate;
  rq->previous = bank;

  // Debounced local-params save, riding the ~2x/s rescan cycle: only attach
  // once edits have settled for ~300ms.
  if(local_dirty && !local_save_inflight
     && local_dirty_age >= (long)(samplerate * 0.3))
  {
    rq->save = std::make_shared<const std::map<std::string, ParamSnapshot>>(
        local_store);
    local_dirty = false;
    local_save_inflight = true;
  }

  worker.request(std::move(rq));
}

// Local-params mode. RAM (local_store) is authoritative for the current set;
// the disk file is adopted on set change or external edits, never while our
// own unsaved edits are pending. Values are applied to the processor's own
// inputs immediately (audio stays correct with no panel open) and flagged for
// write-back to the ports' document values through the UI bus.
void Granola::handle_local_params(std::string_view cur_name, bool have_bank_sound)
{
  if(!inputs.local_params || !have_bank_sound)
  {
    local_active = false;
    return;
  }

  const auto& wanted_set
      = inputs.params_set.value.empty() ? "default" : inputs.params_set.value;
  if(bank.params_set == wanted_set && !local_dirty && !local_save_inflight
     && bank.params_mtime != params_adopted_mtime)
  {
    local_store = bank.params;
    params_adopted_mtime = bank.params_mtime;
    // Re-apply only when the adopted values actually differ (adopting our own
    // save back would otherwise cause a redundant apply every round-trip)
    if(auto it = local_store.find(local_file);
       it == local_store.end() || !(it->second == local_last))
      local_file.clear();
  }

  if(local_file != cur_name || !local_active)
  {
    // File switch (or mode just enabled): restore the file's stored values;
    // a file never seen in this set starts from the current port values.
    if(auto it = local_store.find(std::string(cur_name)); it != local_store.end())
    {
      apply_params(it->second);
      ui_send_params = true;
    }
    else
    {
      local_store.emplace(std::string(cur_name), param_snapshot());
      local_dirty = true;
      local_dirty_age = 0;
    }
    local_file = cur_name;
    local_last = param_snapshot();
  }
  else if(auto snap = param_snapshot(); snap != local_last)
  {
    // Edits (inspector, waveform gestures, automation) update the store
    local_last = snap;
    local_store[local_file] = snap;
    local_dirty = true;
    local_dirty_age = 0;
  }
  local_active = true;
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

  // Periodic folder rescan (worker thread; ~2x/second), immediate on
  // folder/map/params-set change.
  if(local_dirty)
    local_dirty_age += t.frames;
  bank_scan_phase += t.frames;
  const auto& wanted_set
      = inputs.params_set.value.empty() ? "default" : inputs.params_set.value;
  const bool paths_changed = bank.folder != inputs.sound_folder.value
                             || bank.map_file != inputs.midi_map.value
                             || bank.params_set != wanted_set;
  if(paths_changed || bank_scan_phase >= (long)(samplerate / 2))
  {
    bank_scan_phase = 0;
    request_scan();
  }

  // Current sound: bank entry selected by index, falling back to the legacy
  // Sound port when no folder is set / the bank is empty.
  GrainSource cur{};
  std::shared_ptr<const void> cur_hold{};
  std::string_view cur_name;
  const BankSound* cur_bank_snd{};
  // Random toggle: every grain picks its own sound; display shows the first.
  // Otherwise Sound index (0-based, clipped) selects the sound.
  const bool random_pick = !bank.sounds.empty() && inputs.random;
  if(!bank.sounds.empty())
  {
    const int N = (int)bank.sounds.size();
    const int idx = random_pick ? 0 : CLAMP(inputs.sound_index.value, 0, N - 1);
    const auto& snd = bank.sounds[idx];
    cur = snd->view();
    cur_hold = snd;
    cur_name = random_pick ? std::string_view("(random)") : std::string_view(snd->name);
    cur_bank_snd = snd.get();
  }

  if(outputs.current_sound.value != cur_name)
    outputs.current_sound.value = std::string(cur_name);

  // Local-params mode: restore/capture per-file values for the index-picked
  // sound. Runs before the UI message so restored values ship with it.
  // Frozen in random-pick mode: there is no single current file.
  handle_local_params(cur_name, cur_bank_snd != nullptr && !random_pick);

  // Waveform UI: send the envelope of the index-picked sound when it changes
  // (name or on-disk content), when a (re)created UI asks for a refresh, or
  // when restored local params must be written back to the ports.
  const int64_t cur_mtime = cur_bank_snd ? cur_bank_snd->mtime : 0;
  if(send_message
     && (ui_refresh || ui_send_params || ui_sound_name != cur_name
         || ui_sound_mtime != cur_mtime))
  {
    processor_to_ui msg;
    msg.name = cur_name;
    if(cur_bank_snd)
    {
      msg.min_peaks = cur_bank_snd->min_peaks;
      msg.max_peaks = cur_bank_snd->max_peaks;
      msg.duration_s = cur_bank_snd->duration_s;
    }
    if(ui_send_params)
    {
      msg.has_params = true;
      msg.params = local_last;
    }
    send_message(std::move(msg));
    ui_refresh = false;
    ui_send_params = false;
    ui_sound_name = cur_name;
    ui_sound_mtime = cur_mtime;
  }

  if(!cur)
    return;

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

  auto dist = std::normal_distribution<float>(0., 1.0f / 4);

  double density = inputs.density * (1 + dist(rd) * inputs.dens_j);

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
                                   (0., 1.0f / 4)(rd) * inputs.pos_j;
          float dur = inputs.dur + std::normal_distribution<float>
                                   (0., 1.0f / 4)(rd) * inputs.dur_j;
          float rate;
          boost::container::static_vector<double, NCHAN> spawn_ampvec = ampvec;
          // Per-voice source: MIDI zones can map this note to another sound
          GrainSource spawn_src = cur;
          std::shared_ptr<const void> spawn_hold = cur_hold;
          int pitch_root = 60;
          if(random_pick)
          {
            // Sound index 0: every grain picks its own sound (zones still win)
            const auto& snd = bank.sounds[rd() % bank.sounds.size()];
            spawn_src = snd->view();
            spawn_hold = snd;
          }
          if(midi_active)
          {
            if(const auto* z = bank.zone_for(midi_pending_voice))
            {
              const auto& snd = bank.sounds[z->sound_index];
              spawn_src = snd->view();
              spawn_hold = snd;
              pitch_root = z->root;
            }
          }

          if(midi_active)
          {
            // Recompute pitch from current inputs.rate so glissandi take effect immediately
            float base = inputs.rate * std::pow(2.f, (midi_pending_voice - pitch_root) / 12.f);
            rate = base + std::normal_distribution<float>
                          (0., 1.0f / 4)(rd) * inputs.rate_j
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
                                 (0., 1.0f / 4)(rd) * inputs.rate_j
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
      if(trigger_counter >= cur.frames * inputs.dur / (density * inputs.rate))
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
      if(trigger_counter >= cur.frames * inputs.dur / (density * inputs.rate))
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
