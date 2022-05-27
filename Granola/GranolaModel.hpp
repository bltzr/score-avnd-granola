#pragma once

#include <halp/audio.hpp>
#include <halp/controls.hpp>
#include <halp/meta.hpp>
#include <halp/sample_accurate_controls.hpp>

#include <cmath>
#include "utils.hpp"

#include "grain.hpp"
#include <QDebug>

#define NCHAN 8

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

  //static const int NCHAN{8};

  struct ins
  {
    struct : halp::soundfile_port<"Sound"> {
       void update(Granola& self) {
          qDebug() << "sound changed: " << soundfile.filename.data();//std::string{name()}.c_str();
       }
    } sound;
    //halp::soundfile_port<"Window", double> win; // not supported yet
    halp::hslider_f32<"Position", halp::range{0., 1., 0.}> pos;
    halp::hslider_f32<"Duration", halp::range{0., 1., 0.}> dur;
    halp::knob_f32<"Rate", halp::range{-10., 10., 0.}> rate;
    halp::knob_f32<"Gain", halp::range{.min = 0., .max = 4., .init = 0.5}> gain;
    struct { halp__enum_combobox("Interpolation mode", Cubic, None, Linear, Cubic) } interp_type;
    halp::toggle<"Loop"> loopmode;
    struct : halp::spinbox_i32<"Source Channels", halp::range{1, NCHAN, 1}> {
        void update(Granola& self) {
           value = CLAMP(value, 1, self.inputs.sound.channels());
        }
    } src_channels;
    struct : halp::spinbox_i32<"Channel Offset", halp::range{0, NCHAN-1, 0}> {
        void update(Granola& self) {
           value = CLAMP(value, 0, self.inputs.sound.channels()-self.inputs.src_channels-1);
        }
    } channel_offset;
    struct : halp::spinbox_i32<"Max Voices", halp::range{0, 1024, 16}> {
       void update(Granola& self) {
          qDebug() << "num voices changed: " << value;//std::string{name()}.c_str();
          self.grains.resize(value);
       }
    } num_voices;

  } inputs;

  struct
  {
    halp::fixed_audio_bus<"Output", double, NCHAN> audio;
  } outputs;

  struct ui;

  GrainVec  grains;

  bool      buf_soft_lock{false}; // useful?
  bool      alloccheck{false};
  int       busyCount{0};
  double    maxAmp{0};
  float     samplerate;
  float     sampleinterval;
  double    ms2samps;
  long      numoutputs;

  // t_critical  lock; // is there an equivalent?

  using setup = halp::setup;
  void prepare(setup info);

  void resize(int n);
  void clear();

  using tick = halp::tick;
  void operator()(tick t);

};

}
