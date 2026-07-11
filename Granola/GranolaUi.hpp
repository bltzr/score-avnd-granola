#pragma once
#include <Granola/GranolaModel.hpp>
#include <halp/custom_widgets.hpp>
#include <halp/layout.hpp>

#include <cstdio>

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

// Waveform display for the sound picked by the Sound index port. Peaks come
// from the processor over the message bus (see Granola::processor_to_ui);
// the playback window overlay (position + duration, jitter bands at the
// edges) is synced from the control ports in ui::on_control_update.
struct WaveformItem
{
  static constexpr double width() { return 360.; }
  static constexpr double height() { return 100.; }

  std::vector<float> min_peaks, max_peaks;
  std::string name;
  float duration_s{};

  // control port values, synced in ui::on_control_update
  float pos{}, pos_j{}, pos_j_r{1.f};
  float dur{}, dur_j{}, dur_j_r{1.f};

  std::function<void()> update;

  void paint(auto ctx)
  {
    constexpr double w = width(), h = height();
    constexpr double pad = 2.;
    const double mid = h / 2.;
    const double yscale = (h / 2.) - pad;

    ctx.begin_path();
    ctx.set_fill_color(halp::colors::background_darker);
    ctx.draw_rounded_rect(0., 0., w, h, 3.);
    ctx.fill();

    if(!min_peaks.empty() && min_peaks.size() == max_peaks.size())
    {
      // envelope: top edge follows max peaks, bottom edge min peaks
      const std::size_t N = min_peaks.size();
      const double dx = (w - 2. * pad) / N;
      ctx.begin_path();
      ctx.move_to(pad, mid - max_peaks[0] * yscale);
      for(std::size_t i = 1; i < N; i++)
        ctx.line_to(pad + i * dx, mid - max_peaks[i] * yscale);
      for(std::size_t i = N; i-- > 0;)
        ctx.line_to(pad + i * dx, mid - min_peaks[i] * yscale);
      ctx.close_path();
      auto env = ctx.to_rgba(halp::colors::runtime_value_dark);
      env.a = 180;
      ctx.set_fill_color(env);
      ctx.fill();
      ctx.set_stroke_color(halp::colors::runtime_value_mid);
      ctx.set_stroke_width(1.);
      ctx.stroke();

      // playback window: [pos, pos + dur], full file when dur is 0 or >= 1
      // (same rule as GranuGrain::set)
      const double x0 = pad + std::clamp(double(pos), 0., 1.) * (w - 2. * pad);
      const double wdur = (dur <= 0.f || dur >= 1.f)
                              ? 1.
                              : std::min(double(pos) + double(dur), 1.) - double(pos);
      const double x1 = std::min(x0 + wdur * (w - 2. * pad), w - pad);
      ctx.begin_path();
      ctx.draw_rect(x0, pad, x1 - x0, h - 2. * pad);
      auto win = ctx.to_rgba(halp::colors::editable_value_mid);
      win.a = 60;
      ctx.set_fill_color(win);
      ctx.fill();
      ctx.begin_path();
      ctx.draw_line(x0, pad, x0, h - pad);
      ctx.draw_line(x1, pad, x1, h - pad);
      ctx.set_stroke_color(halp::colors::editable_value_light);
      ctx.set_stroke_width(1.);
      ctx.stroke();

      // jitter bands (±2σ; σ = jitter * range / 4): position at the start
      // edge, duration at the end edge
      auto band = ctx.to_rgba(halp::colors::editable_value_light);
      band.a = 40;
      const double pj = pos_j * pos_j_r / 2. * (w - 2. * pad);
      if(pj > 0.5)
      {
        ctx.begin_path();
        ctx.draw_rect(x0 - pj, pad, 2. * pj, h - 2. * pad);
        ctx.set_fill_color(band);
        ctx.fill();
      }
      const double dj = dur_j * dur_j_r / 2. * (w - 2. * pad);
      if(dj > 0.5)
      {
        ctx.begin_path();
        ctx.draw_rect(x1 - dj, pad, 2. * dj, h - 2. * pad);
        ctx.set_fill_color(band);
        ctx.fill();
      }
    }

    // label: sound name + duration
    char label[256];
    if(!name.empty() && duration_s > 0.f)
      std::snprintf(label, sizeof(label), "%s  (%.2f s)", name.c_str(), duration_s);
    else if(!name.empty())
      std::snprintf(label, sizeof(label), "%s", name.c_str());
    else
      std::snprintf(label, sizeof(label), "(no sound bank)");
    ctx.set_fill_color(halp::colors::lighter);
    ctx.set_font_size(9.);
    ctx.draw_text(pad + 3., pad + 10., label);
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
  halp::custom_actions_item<WaveformItem> waveform;
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
        halp::control<&ins::pos> pos;
        halp::control<&ins::pos_j> pos_j;
        halp::control<&ins::pos_j_r> pos_j_r;
      } pos_box;
      struct
      {
        halp_meta(name, "Duration Controls")
        halp_meta(layout, hbox)
        halp_meta(background, background_dark)
        halp::control<&ins::dur> dur;
        halp::control<&ins::dur_j> dur_j;
        halp::control<&ins::dur_j_r> dur_j_r;
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
  // the layout is built): keep the custom widgets in sync with the ports.
  void on_control_update()
  {
    auto& wb = controls.shape_box.win_box;
    wb.win_coefs.mode = int(wb.window_mode.value);
    if(wb.win_coefs.update)
      wb.win_coefs.update();

    auto& sb = controls.shape_box;
    waveform.pos = sb.pos_box.pos.value;
    waveform.pos_j = sb.pos_box.pos_j.value;
    waveform.pos_j_r = sb.pos_box.pos_j_r.value;
    waveform.dur = sb.dur_box.dur.value;
    waveform.dur_j = sb.dur_box.dur_j.value;
    waveform.dur_j_r = sb.dur_box.dur_j_r.value;
    if(waveform.update)
      waveform.update();
  }

  struct bus
  {
    // ui -> processor
    std::function<void(ui_to_processor)> send_message;
    void init(ui& self)
    {
      // The panel may be (re)created long after execution started: ask the
      // processor to resend the current sound's envelope.
      send_message(ui_to_processor{});
    }

    // processor -> ui
    static void process_message(ui& self, processor_to_ui msg)
    {
      auto& wf = self.waveform;
      wf.min_peaks = std::move(msg.min_peaks);
      wf.max_peaks = std::move(msg.max_peaks);
      wf.name = std::move(msg.name);
      wf.duration_s = msg.duration_s;
      if(wf.update)
        wf.update();
    }
  };

  //halp::item<&ins::interp_type> interp_type;
  //halp::item<&ins::loopmode> loopmode;
  //halp::item<&ins::src_channels> src_channels;
  //halp::item<&ins::channel_offset> channel_offset;
};
}
