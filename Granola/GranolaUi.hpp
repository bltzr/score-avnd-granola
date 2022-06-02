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
  halp_meta(background, darker)
  halp::label title{"Granulator"};
  halp::item<&ins::sound> sound;
  //halp::item<&ins::sound> win; not supported yet
  struct {
     halp_meta(name, "Controls")
     halp_meta(layout, hbox)
     halp_meta(background, dark)
     struct {
        halp_meta(name, "Controls")
        halp_meta(layout, vbox)
        halp_meta(background, dark)
         struct {
            halp_meta(name, "Gain Controls")
            halp_meta(layout, hbox)
            halp_meta(background, dark)
            halp::item<&ins::gain> gain;
            halp::item<&ins::gain_j> gain_j;
          } gain_box;
         struct {
            halp_meta(name, "Density Controls")
            halp_meta(layout, hbox)
            halp_meta(background, dark)
            halp::item<&ins::density> density;
            halp::item<&ins::dens_j> dens_j;
          } dens_box;
         struct {
            halp_meta(name, "Pitch Controls")
            halp_meta(layout, hbox)
            halp_meta(background, dark)
            halp::item<&ins::rate> rate;
            struct {
               halp_meta(name, "Pitch_extra")
               halp_meta(layout, vbox)
               halp_meta(background, dark)
               halp::item<&ins::rate_j> rate_j;
               halp::item<&ins::reverse> reverse;
            } pitch_xtra_box;
          } rate_box;
      } params_box;
     struct {
        halp_meta(name, "Controls")
        halp_meta(layout, vbox)
        halp_meta(background, dark)
         struct {
            halp_meta(name, "Position Controls")
            halp_meta(layout, hbox)
            halp_meta(background, dark)
            halp::item<&ins::pos> pos;
            halp::item<&ins::pos_j> pos_j;
          } pos_box;
         struct {
            halp_meta(name, "Duration Controls")
            halp_meta(layout, hbox)
            halp_meta(background, dark)
            halp::item<&ins::dur> dur;
            halp::item<&ins::dur_j> dur_j;
          } dur_box;
          halp::item<&ins::win_coefs> win_coefs;
     } shape_box;
   } controls;

  //halp::item<&ins::interp_type> interp_type;
  //halp::item<&ins::loopmode> loopmode;
  //halp::item<&ins::src_channels> src_channels;
  //halp::item<&ins::channel_offset> channel_offset;
};
}
