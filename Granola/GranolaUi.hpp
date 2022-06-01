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
  halp::item<&ins::pos_j> pos_j;
  halp::item<&ins::dur> dur;
  halp::item<&ins::dur_j> dur_j;
  halp::item<&ins::density> density;
  halp::item<&ins::dens_j> dens_j;
  halp::item<&ins::gain> gain;
  halp::item<&ins::rate> rate;
  halp::item<&ins::rate_j> rate_j;
  halp::item<&ins::reverse> reverse;
  halp::item<&ins::win_coefs> win_coefs;
  //halp::item<&ins::interp_type> interp_type;
  //halp::item<&ins::loopmode> loopmode;
  //halp::item<&ins::src_channels> src_channels;
  //halp::item<&ins::channel_offset> channel_offset;
};
}
