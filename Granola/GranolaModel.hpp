#pragma once

#include "grain.hpp"
#include "utils.hpp"

#include <QDebug>

#include <halp/audio.hpp>
#include <halp/controls.hpp>
#include <halp/mappers.hpp>
#include <halp/meta.hpp>
#include <halp/sample_accurate_controls.hpp>
#include <rnd/random.hpp>

#include <cmath>

#include <random>

namespace Granola
{

typedef std::vector<GranuGrain> GrainVec;

class Granola
{
public:
  halp_meta(name, "Granola")
  halp_meta(category, "Audio")
  halp_meta(c_name, "granola")
  halp_meta(uuid, "38F9684D-54A6-4F48-91E4-3B251F0956EA")

  //static const int NCHAN{8};

  struct ins
  {
    struct : halp::soundfile_port<"Sound">
    {
      halp_flag(waveform);
      void update(Granola& self)
      {
        self.outputs.audio.request_channels(this->channels());
        //if (self.inputs.playing) self.trigger = true;
        qDebug() << "sound" << self.trigger;
      }
    } sound;
    //halp::soundfile_port<"Window", double> win; // not supported yet
    //halp::range_slider_f32<"In", halp::range_slider_range{-10, 100, {5, 20}}> ta_range;
    halp::hslider_f32<"Position", halp::range{0.00000001, 1., 0.00000001}> pos;
    halp::hslider_f32<"Position Jitter", halp::range{0., 1., 0.}> pos_j;
    halp::knob_f32<"Position Jitter Range", halp::range{0., 1., 1.}> pos_j_r;
    halp::hslider_f32<"Duration", halp::range{0., 1., 0.1}> dur;
    halp::hslider_f32<"Duration Jitter", halp::range{0., 1., 0.}> dur_j;
    halp::knob_f32<"Duration Jitter Range", halp::range{0., 1., 1.}> dur_j_r;
    struct : halp::knob_f32<"Pitch", halp::range{0.000001, 10., 1.}>
    {
      using mapper = halp::inverse_mapper<halp::pow_mapper<4>>;
    } rate;
    halp::vslider_f32<"Pitch Jitter", halp::range{0., 1., 0.}> rate_j;
    halp::knob_f32<"Pitch Jitter Range", halp::range{0., 1., 1.}> rate_j_r;
    halp::toggle<"Reverse"> reverse;
    struct : halp::toggle<"Continuous", halp::toggle_setup{.init = true}> {
      void update(Granola& self)
      {
        self.trigger = value;
        qDebug() << "cont" << self.trigger << "value"  << value;
      }
    }playing;
    struct : halp::impulse_button<"Trigger"> {
      /*void update(Granola& self)
      {
        self.trigger = true;
        qDebug() << "trig" << self.trigger ;
      }*/
    } trig;
    struct : halp::accurate<halp::knob_f32<"Density", halp::range{0., 256., 1.}>>
    {
      using mapper = halp::log_mapper<std::ratio<99, 100>>;
    } density;
    halp::vslider_f32<"Density Jitter", halp::range{0., 1., 0.}> dens_j;
    halp::hslider_f32<"Density Jitter Range", halp::range{0., 50., 1.}> dens_j_r;
    halp::knob_f32<"Gain", halp::range{.min = 0., .max = 4., .init = 0.5}> gain;
    halp::vslider_f32<"Gain Jitter", halp::range{0., 1., 0.}> gain_j;
    halp::knob_f32<"Gain Jitter Range", halp::range{0., 1., 1.}> gain_j_r;
    halp::xy_pad_f32<"Window coefs", halp::range{0.f, 0.f, 0.f}> win_coefs;
    struct
    {
      halp__enum_combobox("Interpolation mode", Cubic, None, Linear, Cubic)
    } interp_type;
    struct
    {
      halp__enum_combobox("Window mode", Beta, Beta, Cos, Kuma)
    } window_mode;
    halp::toggle<"Loop"> loopmode;
    struct : halp::spinbox_i32<"Source Channels", halp::range{1, NCHAN, 1}>
    {
      void update(Granola& self)
      {
        value = CLAMP(value, 1, self.inputs.sound.channels());
      }
    } src_channels;
    struct : halp::spinbox_i32<"Channel Offset", halp::range{0, NCHAN - 1, 0}>
    {
      void update(Granola& self)
      {
        value = CLAMP(
            value, 0, self.inputs.sound.channels() - self.inputs.src_channels - 1);
      }
    } channel_offset;
    struct : halp::spinbox_i32<"Max Voices", halp::range{0, 256, 128}>
    {
      void update(Granola& self) { self.grains.resize(value); }
    } num_voices;

  } inputs;

  struct
  {
    halp::variable_audio_bus<"Output", double> audio;
  } outputs;

  struct ui;

  GrainVec grains;

  bool buf_soft_lock{false}; // useful?
  bool alloccheck{false};
  bool trigger{false};
  long trigger_counter{0};
  int busyCount{0};
  bool useDefaultAmp{true};
  double maxAmp{0};
  float samplerate;
  float sampleinterval;
  double ms2samps;
  long numoutputs;

  // t_critical  lock; // is there an equivalent?

  using setup = halp::setup;
  void prepare(setup info);

  void resize(int n);
  void clear();

  using tick = halp::tick;
  void operator()(tick t);

private:
  float wc_radius{64.};
  rnd::pcg rd;
};

}
