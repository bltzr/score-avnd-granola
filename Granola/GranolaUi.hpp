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

  halp::label title{"My First Processor"};
  halp::item<&ins::gain> gain;
};
}
