#pragma once
#include <Granola/GranolaModel.hpp>
#include <halp/custom_widgets.hpp>
#include <halp/layout.hpp>

#include <QCursor>
#include <QMenu>

#include <cfloat>

namespace Granola
{
// Custom widget for the "Window coefs" XY port: paints the actual grain
// window shape for the current coefficients. The port semantics stay
// identical (an xy value); dragging maps x/y exactly like the standard pad.
// The curve mirrors the (angle, amount) -> (a, b) mapping of the tick path
// in GranolaModel.cpp and the per-mode math of GranuGrain::window().
struct WindowShapeItem
{
  static constexpr double width() { return 220.; }
  static constexpr double height() { return 120.; }

  halp::xy_type<float> value{};
  halp::transaction<halp::xy_type<float>> transaction;
  std::function<void()> update;
  std::function<void(halp::xy_type<float>)> set;
  int mode{0}; // Window mode port, synced in ui::on_control_update

  static constexpr int curve_points = 128;
  static constexpr double pad = 2.;

  void paint(auto ctx)
  {
    constexpr double w = width(), h = height();
    constexpr int N = curve_points;

    // Same mapping as the spawn path in GranolaModel.cpp
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

    // Title floating in the (empty) top-left corner, to the right of the dot.
    ctx.begin_path();
    ctx.set_fill_color({255, 255, 255, 255});
    ctx.set_font_size(9.);
    ctx.draw_text(16., 12., "Window coefficients");
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

// Bound to the window_mode enum port; addon-local (uses the custom-item mouse
// forwarding), so no score-side combobox change is needed.
struct WindowModeItem
{
  static constexpr double width() { return 50.; }
  static constexpr double height() { return 24.; }
  static constexpr std::array<const char*, 3> names{"Beta", "Cos", "Kuma"};

  int value{0};
  std::function<void(int)> set;
  std::function<void()> update;

  void paint(auto ctx)
  {
    ctx.begin_path();
    ctx.set_fill_color(halp::colors::background_darker);
    ctx.draw_rounded_rect(0., 0., width(), height(), 2.);
    ctx.fill();

    ctx.begin_path();
    ctx.set_fill_color({255, 255, 255, 255});
    ctx.set_font_size(11.);
    ctx.draw_text(8., 16., names[std::clamp(value, 0, 2)]);
    ctx.fill();
  }

  bool mouse_press(double, double)
  {
    QMenu menu;
    for(int i = 0; i < (int)names.size(); i++)
    {
      auto* a = menu.addAction(names[i]);
      a->setCheckable(true);
      a->setChecked(i == value);
      a->setData(i);
    }
    if(auto* chosen = menu.exec(QCursor::pos()))
      if(set)
        set(chosen->data().toInt());
    return true;
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
      // Window-mode selector above the live window-shape display.
      halp::custom_control<WindowModeItem, &ins::window_mode> window_mode;
      halp::custom_control<WindowShapeItem, &ins::win_coefs> win_coefs;
    } shape_box;
  } controls;

  // Called by score whenever a synced control value changes (and once after
  // the layout is built): keep the shape widget's mode in sync with the port.
  void on_control_update()
  {
    auto& sb = controls.shape_box;
    sb.win_coefs.mode = int(sb.window_mode.value);
    if(sb.win_coefs.update)
      sb.win_coefs.update();
    if(sb.window_mode.update)
      sb.window_mode.update();
  }

  //halp::item<&ins::interp_type> interp_type;
  //halp::item<&ins::loopmode> loopmode;
  //halp::item<&ins::src_channels> src_channels;
  //halp::item<&ins::channel_offset> channel_offset;
};
}
