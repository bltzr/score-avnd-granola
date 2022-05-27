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

  /*
  qDebug() << "snd channels: " << inputs.sound.channels();
  qDebug() << "out channels: " << n_channels;
  qDebug() << "channel offset: " << ch_offset;
  */

  // create the right number of channels in our bus:
  //outputs.audio.channels = n_channels;

  if( buf_soft_lock ){
          for ( int k = 0; k < t.frames; k++){
              for( int i = 0; i < outputs.audio.channels(); i++){
                  auto out = outputs.audio.channel(i, t.frames);
                  out[k] = 0.0;
               }
          }
          return;
  }

  for (int j = 0; j < t.frames; j++)
  {
      alloccheck = false;
      maxAmp = 0;
      busyCount = 0;

      for (int i = 0; i < outputs.audio.channels(); i++){
        // Output buffer for channel i, also a std::span.
        auto out = outputs.audio.channel(i, t.frames);
        for (int j = 0; j < t.frames; j++)
             out[j] = 0.;
      }
  }



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
}
}
