#include "Granola.hpp"

namespace Granola
{
void Granola::operator()(halp::tick t)
{
  // Process the input buffer
  for (int i = 0; i < outputs.audio.channels; i++)
  {
      if(!inputs.sound)
            return;

      // Just take the first channel of the soundfile.
      // in is a std::span
      const auto in = inputs.sound.channel(0);

      // We'll read at this position
      const int64_t start = std::floor(inputs.pos * inputs.sound.frames());

      // Copy it at the given position for each output
      for (int i = 0; i < outputs.audio.channels; i++)
      {
        // Output buffer for channel i, also a std::span.
        auto out = outputs.audio.channel(i, t.frames);

        for (int j = 0; j < t.frames; j++)
        {
          // If we're before the end of the file copy the sample
          if(start + j < inputs.sound.frames())
            out[j] = in[start + j];
          else
            out[j] = 0;
        }
      }
  }
}
}
