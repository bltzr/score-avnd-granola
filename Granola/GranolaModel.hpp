#pragma once

#include <halp/audio.hpp>
#include <halp/controls.hpp>
#include <halp/meta.hpp>
#include <halp/sample_accurate_controls.hpp>

#include <cmath>

#include "grain.hpp"

namespace Granola
{

typedef  std::vector<GranuGrain>    GrainVec;

class Granola
{
public:
  halp_meta(name, "Granola")
  halp_meta(category, "Audio")
  halp_meta(c_name, "granola")
  halp_meta(uuid, "38F9684D-54A6-4F48-91E4-3B251F0956EA")

  // See the docs at https://github.com/celtera/avendish
  struct ins
  {
    halp::soundfile_port<"Sound", double> sound;
    halp::soundfile_port<"Window", double> win; // not supported yet
    halp::hslider_f32<"Position", halp::range{0., 1., 0.}> pos;
    halp::knob_f32<"Gain", halp::range{.min = 0., .max = 10., .init = 0.5}> gain;
    halp__enum("Interpolation mode", Cubic, None, Linear, Cubic) interp;
    halp::toggle<"Loop"> loopmode;

  } inputs;

  struct
  {
    halp::dynamic_audio_bus<"Output", double> audio;
  } outputs;

  using setup = halp::setup;
  void prepare(halp::setup info)
  {
    samplerate = info.rate;
    sampleinterval = 1.0 / samplerate;
    ms2samps = samplerate * 0.001;

    for( long i = 0; i < grains.size(); i++)
            grains[i].reset();
  }

  // Do our processing for N samples
  using tick = halp::tick;

  // Defined in the .cpp
  void operator()(tick t);

  // UI is defined in another file to keep things clear.
  struct ui;

  GrainVec    grains;

  long        numbufs;
  bool        buf_soft_lock; // useful?

  long        src_channels;
  long        channel_offset;

  long        interp_type;

  long        busy;

  float       samplerate;
  float       sampleinterval;

  double      ms2samps;

  long        numoutputs;

  long        num_voices;

  long        loopmode;

  // t_critical  lock; // is there an equivalent?

};

}
