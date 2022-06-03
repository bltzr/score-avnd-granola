#pragma once
#include <Granola/GranolaModel.hpp>
#include <halp/layout.hpp>
#include <avnd/concepts/painter.hpp>

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
  struct {
    halp_meta(name, "Sound")
    halp_meta(layout, hbox)
    halp_meta(background, dark)
    halp::item<&ins::sound> sound;
    halp::item<&ins::ta_range> ta_range;
  } sound_box;
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
    struct {
      halp_meta(name, "Waveform")
      halp_meta(layout, vbox)
      halp_meta(background, dark)
      struct custom_anim
      {
        halp_meta(name, "Waveform Display")
        //halp_meta(layout, vbox)
        halp_meta(background, dark)
        // Static item metadatas: mandatory
        static constexpr double width() { return 200.; }
        static constexpr double height() { return 200.; }
        static constexpr double layout() { enum { custom } d{}; return d; }

        // In practice with the helpers, we use a type with the mandatory parts
        // already defined and just focus on our item's specificities ; this is
        // enabled by this typedef.
        using item_type = custom_anim;

        // Item properties: those are mandatory
        double x = 0.0;
        double y = 0.0;
        double scale = 1.0;

        // Our paint method. avnd::painter is a concept which maps to the most usual
        // canvas-like APIs. It is not necessary to indicate it - it just will give better
        // error messages in case of mistake, and code completion (yes) in IDEs such as QtCreator
        void paint(avnd::painter auto ctx)
        {
          constexpr double cx = 30., cy = 30.;
          constexpr double side = 40.;

          ctx.set_stroke_color({.r = 92, .g = 53, .b = 102, .a = 255});
          ctx.set_fill_color({173, 127, 168, 255});

          ctx.translate(100, 100);
          ctx.rotate(rot += 0.1);

          for(int i = 0; i < 10; i++)
          {
            ctx.translate(10, 10);
            ctx.rotate(5.+ 0.1 * rot);
            ctx.scale(0.8, 0.8);
            ctx.begin_path();

            ctx.draw_rect(-side / 2., -side / 2., side, side);
            ctx.fill();
            ctx.stroke();
          }

          ctx.update();
        }

        double rot{};
      };
    } wf_box;
  } controls;

  //halp::item<&ins::interp_type> interp_type;
  //halp::item<&ins::loopmode> loopmode;
  //halp::item<&ins::src_channels> src_channels;
  //halp::item<&ins::channel_offset> channel_offset;
};
}
