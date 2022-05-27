#include "Granola.hpp"
#include "utils.hpp"


namespace Granola
{

void Granola::prepare(setup info)
{
  samplerate = info.rate;
  sampleinterval = 1.0 / samplerate;
  ms2samps = samplerate * 0.001;

  // create the appropriate number of grains:
  resize(inputs.num_voices);

}

void Granola::resize(int n){
    buf_soft_lock = true; /// TODO: use a mutex or something?
    grains.resize(n);
    clear();
    buf_soft_lock = false;
}

void Granola::clear() {
    for( long i = 0; i < grains.size(); i++)
        grains[i].reset();
}

void Granola::operator()(tick t)
{
  if(!inputs.sound)
        return;

  int n_channels = CLAMP(inputs.src_channels, 1, inputs.sound.channels());
  int ch_offset = CLAMP(inputs.channel_offset, 0, inputs.sound.channels()-n_channels);

  std::vector<double> ampvec(n_channels, sizeof(double));
  for (int i = 0; i<n_channels; i++){
      ampvec[i] = 1.0f; // useDefaultAmp ? 1.0f : amps[i]; // add this when we want to use amp vectors
                                                 // amps[i][j] if we use vectors from an audio port (as in granubuf_mc)
                                                 // also move below in for (int j = 0; j < t.frames; j++)
      maxAmp = ampvec[i] > maxAmp ? ampvec[i] : maxAmp;
  }

  std::vector<double> windcoef(2, sizeof(double));

/*
  qDebug() << "snd channels: " << inputs.sound.channels();
  qDebug() << "out channels: " << n_channels;
  qDebug() << "channel offset: " << ch_offset;
*/

  if( buf_soft_lock ){
          for ( int k = 0; k < t.frames; k++){
              for( int i = 0; i < outputs.audio.channels(); i++){
                  auto out = outputs.audio.channel(i, t.frames);
                  out[k] = 0.0;
               }
          }
          return;
  }

  for (int k = 0; k < t.frames; k++)
  {
      ++trigger_counter;
      //qDebug() << trigger_counter;
      trigger = trigger_counter >= inputs.sound.frames() * inputs.dur / inputs.density;
      if (trigger) { trigger_counter = 0; qDebug() << " triggering grain";}

      alloccheck = false;
      maxAmp = 1;
      busyCount = 0;

      for (int i = 0; i < outputs.audio.channels(); i++){
        // Output buffer for channel i, also a std::span.
        auto out = outputs.audio.channel(i, t.frames);
             out[k] = 0.;
      }

      for( long i = 0; i < inputs.num_voices; i++ )
      {
/*
          qDebug() <<
          "alloccheck" << alloccheck <<
          "trigger" << trigger <<
          "busyCount" << busyCount <<
          "current voice" << i <<
          "inputs.num_voices" << inputs.num_voices <<
          "maxAmp" << maxAmp <<
          "inputs.dur" << inputs.dur <<
          "inputs.rate" << inputs.rate;
*/

          if(!alloccheck &&
             trigger &&
             busyCount < inputs.num_voices &&
             maxAmp > 0. &&
             inputs.dur != 0. &&
             inputs.rate != 0. ){

              qDebug() << "grain triggered";
              if( !grains[i].m_active )
              {
                  for( int j = 0; j < 2; j++){
                      windcoef[j] = 1.; // replace from actual coefs from an XY widget (with polar conversion
                  }

                  grains[i].set(inputs.pos,
                                inputs.dur * ms2samps,
                                inputs.rate,
                                //_bufIdx,
                                windcoef,
                                ampvec,
                                inputs.sound,
                                //NULL, // future holder of optional window buffer
                                inputs.loopmode,
                                inputs.window_mode,
                                ch_offset,
                                n_channels);

                  alloccheck = true;
                  trigger = false;
              }

          }

          if( grains[i].m_active && grains[i].m_buf_len <= inputs.sound.frames() )
          {

              std::span<double> outSamps {grains[i].incr( inputs.sound, inputs.interp_type )};

              for( int j = 0; j < n_channels; j++)
              {
                  auto out = outputs.audio.channel(i, t.frames);
                  out[k] += outSamps[j];
              }

              busyCount++;
          }
      }
  }

  // somehow dispaly the current number of active grains
  //out[numGrainOuts][k] = (double)busyCount;

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
