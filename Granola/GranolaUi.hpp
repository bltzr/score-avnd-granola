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
  halp::item<&ins::gain> gain;
  halp::item<&ins::interp> interp;
  halp::item<&ins::loopmode> loopmode;
};
}
