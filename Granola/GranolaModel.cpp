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
  if(!inputs.sound || inputs.sound.channels() == 0)
    return;

  const int n_channels = CLAMP(inputs.src_channels, 1, inputs.sound.channels());
  const int ch_offset
      = CLAMP(inputs.channel_offset, 0, inputs.sound.channels() - n_channels);

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
      for(int i = 0; i < outputs.audio.channels; i++)
      {
        auto out = outputs.audio.channel(i, t.frames);
        out[k] = 0.0;
      }
    }
    return;
  }

  // Process incoming MIDI: update voice state for note on/off
  for(const auto& msg : inputs.midi)
  {
    if(msg.bytes.size() < 3)
      continue;
    const uint8_t status = msg.bytes[0] & 0xF0;
    const uint8_t note   = msg.bytes[1] & 0x7F;
    const uint8_t vel    = msg.bytes[2] & 0x7F;

    if(status == 0x90 && vel > 0) // note on
    {
      auto& v = midi_voices[note];
      v.active = true;
      v.trigger_counter = 0;
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

  auto dist = std::normal_distribution<float>(0., inputs.dens_j_r / 4);

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

    for(int i = 0; i < outputs.audio.channels; i++)
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
          float dur = inputs.dur + std::normal_distribution<float>
                                   (0., inputs.dur_j_r / 4)(rd) * inputs.dur_j;
          float rate;
          if(midi_active)
          {
            // Recompute from current inputs.rate so glissandi take effect immediately
            float base = inputs.rate * std::pow(2.f, (midi_pending_voice - 60) / 12.f);
            rate = base + std::normal_distribution<float>
                          (0., inputs.rate_j_r / 4)(rd) * inputs.rate_j
                                          * ((inputs.reverse) ? -1 : 1);
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
              windcoef, ampvec, inputs.sound,
              inputs.loopmode, inputs.window_mode, ch_offset, n_channels);
          alloccheck = true;
        }
      }

       if(grains[i].m_active && grains[i].m_buf_len <= inputs.sound.frames())
      {

        std::span<double> outSamps{
            grains[i].incr(inputs.sound, (long)inputs.interp_type.value)};

        for(int j = 0; j < n_channels; j++)
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
      if(trigger_counter >= inputs.sound.frames() * inputs.dur / (density * inputs.rate))
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
      if(trigger_counter >= inputs.sound.frames() * inputs.dur / (density * inputs.rate))
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
