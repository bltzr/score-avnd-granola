#pragma once
#include <Granola/GranolaModel.hpp>
#include <halp/layout.hpp>

namespace Granola
{
struct Granola::ui
{
  using enum halp::colors;
  using enum halp::layouts;

  halp_meta(name, "Main")
  halp_meta(layout, vbox)
  halp_meta(background, dark)

  halp::label title{"Granulator"};
  halp::item<&ins::sound> sound;
  //halp::item<&ins::sound> win; not supported yet
  halp::item<&ins::pos> pos;
  halp::item<&ins::dur> dur;
  halp::item<&ins::gain> gain;
  //halp::item<&ins::interp_type> interp_type;
  //halp::item<&ins::loopmode> loopmode;
  //halp::item<&ins::src_channels> src_channels;
  //halp::item<&ins::channel_offset> channel_offset;
};
}
