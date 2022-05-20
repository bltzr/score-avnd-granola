#pragma once

#include <halp/audio.hpp>
#include <halp/controls.hpp>
#include <halp/meta.hpp>

#include <cmath>

#include "grain.hpp"

namespace Granola
{

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
    halp::soundfile_port<"Sound"> sound;
    halp::soundfile_port<"Window"> win;
    halp::hslider_f32<"Position", halp::range{0., 1., 0.}> pos;
    halp::knob_f32<"Gain", halp::range{.min = 0., .max = 10., .init = 0.5}> gain;
  } inputs;

  struct
  {
    halp::dynamic_audio_bus<"Output", double> audio;
  } outputs;

  using setup = halp::setup;
  void prepare(halp::setup info)
  {
    // Initialization, this method will be called with buffer size, etc.
  }

  // Do our processing for N samples
  using tick = halp::tick;

  // Defined in the .cpp
  void operator()(halp::tick t);

  // UI is defined in another file to keep things clear.
  struct ui;
};

}
