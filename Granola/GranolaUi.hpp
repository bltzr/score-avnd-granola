#pragma once
#include <Granola/GranolaModel.hpp>
#include <halp/custom_widgets.hpp>
#include <halp/layout.hpp>

namespace Granola
{
// Custom widget for the "Window coefs" XY port: paints the actual grain
// window shape for the current coefficients. The port semantics stay
// identical (an xy value); dragging maps x/y exactly like the standard pad.
// The curve mirrors the (angle, amount) -> (a, b) mapping of the tick path
// in GranolaModel.cpp and the per-mode math of GranuGrain::window().
struct WindowShapeItem
{
  static constexpr double width() { return 170.; }
  static constexpr double height() { return 80.; }

  halp::xy_type<float> value{};
  halp::transaction<halp::xy_type<float>> transaction;
  std::function<void()> update;
  int mode{0}; // Window mode port, synced in ui::on_control_update

  static constexpr int curve_points = 128;
  static constexpr double pad = 2.;

  void paint(auto ctx)
  {
    constexpr double w = width(), h = height();
    constexpr int N = curve_points;

    const double angle = (1. - value.x) * PI / 2.;
    double a = 1. + value.y * Granola::wc_radius * std::cos(angle);
    double b = 1. + value.y * Granola::wc_radius * std::sin(angle);

    double norm = 1.;
    switch(mode)
    {
      case 0: { // Beta: same coefficient clamping as GranuGrain::setBeta_ab
        constexpr double minx = (1 / DBL_MAX) + 0.00001;
        const double sum = a + b;
        if(sum > 170.)
        {
          if(a > b)
          {
            a -= sum - 170.;
            b = std::max(b, minx);
          }
          else
          {
            b -= sum - 170.;
            a = std::max(a, minx);
          }
        }
        else
        {
          a = CLAMP(a, minx, 170.);
          b = CLAMP(b, minx, 170.);
        }
        norm = getBetaScalar(a, b, 1. / N);
        break;
      }
      case 2: // Kuma
        a = a < 1. ? 2. : a;
        b = b < 1. ? 2. : b;
        norm = 1. / kumaraswamy_peak(a, b);
        break;
      default:
        break;
    }

    auto curve = [a, b, norm, mode = mode](double phase) {
      double v;
      switch(mode)
      {
        case 0:
          v = betaNumerator(phase, a, b) * norm;
          break;
        case 1: {
          const double px = std::pow(phase, std::exp(a / 10.));
          v = std::pow(std::sin(PI * px), std::exp(b / 10.));
          break;
        }
        case 2:
          v = kumaraswamy(phase, a, b) * norm;
          break;
        default:
          v = 0.;
          break;
      }
      return CLAMP(v, 0., 1.);
    };

    ctx.begin_path();
    ctx.set_fill_color(halp::colors::background_darker);
    ctx.draw_rounded_rect(0., 0., w, h, 3.);
    ctx.fill();

    // Filled shape, then the curve stroked on top (without the baseline)
    ctx.begin_path();
    ctx.move_to(pad, h - pad);
    for(int i = 0; i <= N; i++)
    {
      const double phase = double(i) / N;
      ctx.line_to(
          pad + phase * (w - 2. * pad), h - pad - curve(phase) * (h - 2. * pad));
    }
    ctx.line_to(w - pad, h - pad);
    ctx.close_path();
    auto fill = ctx.to_rgba(halp::colors::editable_value_dark);
    fill.a = 120;
    ctx.set_fill_color(fill);
    ctx.fill();

    ctx.begin_path();
    ctx.move_to(pad, h - pad - curve(0.) * (h - 2. * pad));
    for(int i = 1; i <= N; i++)
    {
      const double phase = double(i) / N;
      ctx.line_to(
          pad + phase * (w - 2. * pad), h - pad - curve(phase) * (h - 2. * pad));
    }
    ctx.set_stroke_color(halp::colors::editable_value_light);
    ctx.set_stroke_width(1.5);
    ctx.stroke();

    // Marker showing the underlying xy value, as a drag affordance
    ctx.begin_path();
    ctx.draw_circle(
        pad + value.x * (w - 2. * pad), h - pad - value.y * (h - 2. * pad), 2.5);
    ctx.set_fill_color(halp::colors::lighter);
    ctx.fill();
  }

  bool mouse_press(double x, double y)
  {
    transaction.start();
    mouse_move(x, y);
    return true;
  }

  void mouse_move(double x, double y)
  {
    halp::xy_type<float> v;
    v.x = float(std::clamp((x - pad) / (width() - 2. * pad), 0., 1.));
    v.y = float(std::clamp(1. - (y - pad) / (height() - 2. * pad), 0., 1.));
    transaction.update(v);
  }

  void mouse_release(double x, double y)
  {
    mouse_move(x, y);
    transaction.commit();
  }
};

struct Granola::ui
{
  using enum halp::colors;
  using enum halp::layouts;

  halp_meta(name, "Main")
  halp_meta(layout, vbox)
  halp_meta(background, background_darker)
  halp::label title{"Granulator"};
  halp::item<&ins::sound> sound;
  //halp::item<&ins::sound> win; not supported yet
  struct
  {
    halp_meta(name, "Controls")
    halp_meta(layout, hbox)
    halp_meta(background, background_dark)
    struct
    {
      halp_meta(name, "Controls")
      halp_meta(layout, vbox)
      halp_meta(background, background_dark)
      struct
      {
        halp_meta(name, "Gain Controls")
        halp_meta(layout, hbox)
        halp_meta(background, background_dark)
        halp::item<&ins::gain> gain;
        halp::item<&ins::gain_j> gain_j;
      } gain_box;
      struct
      {
        halp_meta(name, "Density Controls")
        halp_meta(layout, hbox)
        halp_meta(background, background_dark)
        halp::item<&ins::density> density;
        halp::item<&ins::dens_j> dens_j;
      } dens_box;
      struct
      {
        halp_meta(name, "Pitch Controls")
        halp_meta(layout, hbox)
        halp_meta(background, background_dark)
        halp::item<&ins::rate> rate;
        struct
        {
          halp_meta(name, "Pitch_extra")
          halp_meta(layout, vbox)
          halp_meta(background, background_dark)
          halp::item<&ins::rate_j> rate_j;
          halp::item<&ins::reverse> reverse;
        } pitch_xtra_box;
      } rate_box;
      struct
      {
        halp_meta(name, "Pitch_extra")
        halp_meta(layout, hbox)
        halp_meta(background, background_dark)
        halp::item<&ins::playing> playing;
        halp::item<&ins::trig> trig;
      } play_box;
    } params_box;
    struct
    {
      halp_meta(name, "Controls")
      halp_meta(layout, vbox)
      halp_meta(background, background_dark)
      struct
      {
        halp_meta(name, "Position Controls")
        halp_meta(layout, hbox)
        halp_meta(background, background_dark)
        halp::item<&ins::pos> pos;
        halp::item<&ins::pos_j> pos_j;
      } pos_box;
      struct
      {
        halp_meta(name, "Duration Controls")
        halp_meta(layout, hbox)
        halp_meta(background, background_dark)
        halp::item<&ins::dur> dur;
        halp::item<&ins::dur_j> dur_j;
      } dur_box;
      struct
      {
        halp_meta(name, "Window Controls")
        halp_meta(layout, hbox)
        halp_meta(background, background_dark)
        halp::custom_control<WindowShapeItem, &ins::win_coefs> win_coefs;
        halp::control<&ins::window_mode> window_mode;
      } win_box;
    } shape_box;
  } controls;

  // Called by score whenever a synced control value changes (and once after
  // the layout is built): keep the window widget in sync with the mode port.
  void on_control_update()
  {
    auto& wb = controls.shape_box.win_box;
    wb.win_coefs.mode = int(wb.window_mode.value);
    if(wb.win_coefs.update)
      wb.win_coefs.update();
  }

  //halp::item<&ins::interp_type> interp_type;
  //halp::item<&ins::loopmode> loopmode;
  //halp::item<&ins::src_channels> src_channels;
  //halp::item<&ins::channel_offset> channel_offset;
};
}
