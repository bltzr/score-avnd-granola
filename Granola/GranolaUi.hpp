#pragma once
#include <Granola/GranolaModel.hpp>
#include <halp/custom_widgets.hpp>
#include <halp/layout.hpp>

#include <Media/AudioDecoder.hpp>

#include <QCursor>
#include <QDir>
#include <QFileInfo>
#include <QMenu>

#include <cfloat>
#include <cstdio>

namespace Granola
{
// Custom widget for the "Window coefs" XY port: paints the actual grain
// window shape for the current coefficients. Dragging maps x/y like a pad.
struct WindowShapeItem
{
  static constexpr double width() { return 220.; }
  static constexpr double height() { return 140.; }

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

    // Title in the header row above the widget, to the right of the dot.
    ctx.begin_path();
    ctx.set_fill_color({255, 255, 255, 255});
    ctx.set_font_size(9.);
    ctx.draw_text(16., -4., "Window coefficients");
    ctx.fill();

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

// Bound to the window_mode enum port; addon-local QMenu dropdown.
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

// A cable port shown as a small "label value" chip, carrying the port's value
// (synced) and a set hook. The pos/dur/jitter ports use this: the waveform
// gestures are the editor, this shows the current number and feeds the widget.
template <typename T, int W = 80>
struct PortDotValue
{
  static constexpr double width() { return double(W); }
  static constexpr double height() { return 16.; }

  std::string_view label;
  T value{};
  std::function<void(T)> set;

  void paint(auto ctx)
  {
    char buf[64];
    // Float ports are 0..1 normalized -> show as a rounded percentage.
    if constexpr(std::is_floating_point_v<T>)
      std::snprintf(buf, sizeof(buf), "%.*s %.0f%%", (int)label.size(), label.data(),
                    (double)value * 100.);
    else
      std::snprintf(buf, sizeof(buf), "%.*s", (int)label.size(), label.data());
    ctx.begin_path();
    ctx.set_fill_color(halp::colors::mid);
    ctx.set_font_size(8.);
    ctx.draw_text(2., 11., buf);
    ctx.fill();
  }
};

// Waveform display + direct-manipulation gestures for the current sound.
// Peaks arrive from the processor over the message bus; the position/duration
// window overlay (and jitter bands) is synced from the ports in
// ui::on_control_update. Gestures: horizontal drag = position; vertical drag =
// duration in the window centre, position jitter at the start edge, duration
// jitter at the end edge.
struct WaveformItem
{
  static constexpr double width() { return 380.; }
  static constexpr double height() { return 100.; }
  static constexpr double pad = 2.;
  static constexpr double left_pad = pad;

  std::vector<float> min_peaks, max_peaks;
  std::string name;
  float duration_s{};

  // control values, synced in ui::on_control_update
  float pos{}, pos_j{}, dur{}, dur_j{};

  std::function<void()> update;
  // wired in ui::bus::init to the pos/dur/jitter ports' document values
  std::function<void(float)> set_pos, set_dur, set_pos_j, set_dur_j;

  // Mono edit-mode display: the Sound file's path (from the sound chip, synced
  // in on_control_update). When it changes, decode UI-side so the waveform
  // shows before playback; during execution the processor's bus peaks arrive.
  std::string sound_path, m_loaded_path;
  void reload_from_path()
  {
    if(sound_path == m_loaded_path)
      return;
    m_loaded_path = sound_path;
    const QFileInfo fi(QString::fromStdString(sound_path));
    if(sound_path.empty() || !fi.isFile())
      return; // a folder or nothing: leave the bus-supplied peaks in place
    if(auto dec
       = Media::AudioDecoder::decode_synchronous(fi.absoluteFilePath(), 44100))
    {
      auto& d = dec->second;
      std::vector<const float*> ptrs;
      for(auto& ch : d)
        ptrs.push_back(ch.data());
      compute_peaks_raw(
          ptrs.data(), (int)ptrs.size(), (int64_t)(d.empty() ? 0 : d[0].size()),
          44100., min_peaks, max_peaks, duration_s);
      name = fi.fileName().toStdString();
      if(update)
        update();
    }
  }

  // Drop a sound onto the waveform: a file loads it; a folder enables multi
  // mode and anchors on its first audio file. (Needs the score-core drop
  // forwarding in Crousti/Painter.hpp.)
  std::function<void(std::string)> set_sound;
  std::function<void(bool)> set_multi;
  bool drop(const std::vector<std::string>& paths)
  {
    if(paths.empty())
      return false;
    const QFileInfo fi(QString::fromStdString(paths[0]));
    static const QStringList exts{"*.wav", "*.aif", "*.aiff", "*.flac",
                                  "*.mp3", "*.ogg",  "*.m4a"};
    if(fi.isDir())
    {
      QDir dir(fi.absoluteFilePath());
      const auto files = dir.entryList(exts, QDir::Files, QDir::Name);
      if(files.isEmpty())
        return false;
      if(set_multi)
        set_multi(true);
      if(set_sound)
        set_sound(dir.filePath(files[0]).toStdString());
      return true;
    }
    if(set_sound)
      set_sound(fi.absoluteFilePath().toStdString());
    return true;
  }

  static constexpr double draw_w() { return width() - left_pad - pad; }
  double win_x0() const
  {
    return left_pad + std::clamp(double(pos), 0., 1.) * draw_w();
  }
  double win_x1() const
  {
    const double wdur = (dur <= 0.f || dur >= 1.f)
                            ? 1.
                            : std::min(double(pos) + double(dur), 1.) - double(pos);
    return std::min(win_x0() + wdur * draw_w(), width() - pad);
  }

  void paint(auto ctx)
  {
    constexpr double w = width(), h = height();
    const double mid = h / 2.;
    const double yscale = (h / 2.) - pad;

    // Inset by pad to match score::GraphicsLayout's own background rect.
    ctx.begin_path();
    ctx.set_fill_color(halp::colors::background_darker);
    ctx.draw_rounded_rect(pad, pad, w - 2. * pad, h - 2. * pad, 3.);
    ctx.fill();

    if(!min_peaks.empty() && min_peaks.size() == max_peaks.size())
    {
      const std::size_t N = min_peaks.size();
      const double dx = draw_w() / N;
      ctx.begin_path();
      ctx.move_to(left_pad, mid - max_peaks[0] * yscale);
      for(std::size_t i = 1; i < N; i++)
        ctx.line_to(left_pad + i * dx, mid - max_peaks[i] * yscale);
      for(std::size_t i = N; i-- > 0;)
        ctx.line_to(left_pad + i * dx, mid - min_peaks[i] * yscale);
      ctx.close_path();
      auto env = ctx.to_rgba(halp::colors::runtime_value_dark);
      env.a = 180;
      ctx.set_fill_color(env);
      ctx.fill();
      ctx.set_stroke_color(halp::colors::runtime_value_mid);
      ctx.set_stroke_width(1.);
      ctx.stroke();

      const double x0 = win_x0();
      const double x1 = win_x1();
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

      // jitter bands: position (±, symmetric) at the start edge; duration
      // (add-only, one-sided) extending right from the end edge. Bands assume
      // a jitter range of 1 (the audio uses the real *Jitter Range* ports).
      auto band = ctx.to_rgba(halp::colors::editable_value_light);
      band.a = 40;
      const double pj = pos_j / 2. * draw_w();
      if(pj > 0.5)
      {
        ctx.begin_path();
        ctx.draw_rect(x0 - pj, pad, 2. * pj, h - 2. * pad);
        ctx.set_fill_color(band);
        ctx.fill();
      }
      const double dj = dur_j / 2. * draw_w();
      if(dj > 0.5)
      {
        ctx.begin_path();
        ctx.draw_rect(x1, pad, dj, h - 2. * pad);
        ctx.set_fill_color(band);
        ctx.fill();
      }
    }

    char label[256];
    if(!name.empty() && duration_s > 0.f)
      std::snprintf(label, sizeof(label), "%s  (%.2f s)", name.c_str(), duration_s);
    else if(!name.empty())
      std::snprintf(label, sizeof(label), "%s", name.c_str());
    else
      std::snprintf(label, sizeof(label), "load a sound above");
    ctx.begin_path();
    ctx.set_fill_color(halp::colors::lighter);
    ctx.set_font_size(9.);
    ctx.draw_text(left_pad + 3., pad + 12., label);
    ctx.fill();
  }

  enum class drag_zone
  {
    none,
    window,     // vertical -> duration
    start_edge, // vertical -> position jitter
    end_edge    // vertical -> duration jitter
  };
  drag_zone m_zone{drag_zone::none};
  double m_press_x{}, m_press_y{};
  float m_pos0{}, m_dur0{}, m_pos_j0{}, m_dur_j0{};
  bool m_dragging{false};

  bool mouse_press(double x, double y)
  {
    if(min_peaks.empty())
      return false; // no sound: nothing to manipulate

    m_press_x = x;
    m_press_y = y;
    m_pos0 = pos;
    m_dur0 = dur;
    m_pos_j0 = pos_j;
    m_dur_j0 = dur_j;

    constexpr double edge = 10.;
    const double x0 = win_x0(), x1 = win_x1();
    if((x1 - x0) < 3. * edge)
    {
      m_zone = (x >= x0 - edge && x <= x1 + edge) ? drag_zone::window
                                                  : drag_zone::none;
    }
    else if(std::abs(x - x0) <= edge)
      m_zone = drag_zone::start_edge;
    else if(std::abs(x - x1) <= edge)
      m_zone = drag_zone::end_edge;
    else if(x > x0 && x < x1)
      m_zone = drag_zone::window;
    else
      m_zone = drag_zone::none;
    m_dragging = true;
    return true;
  }

  void mouse_move(double x, double y)
  {
    if(!m_dragging)
      return;
    const float dxn = float((x - m_press_x) / draw_w());
    const float dyn = float((m_press_y - y) / (height() - 2. * pad));
    float new_pos = m_pos0 + dxn;
    switch(m_zone)
    {
      case drag_zone::window: {
        const float new_dur = std::clamp(m_dur0 + dyn, 0.01f, 1.f);
        new_pos -= (new_dur - m_dur0) / 2.f;
        if(set_dur)
          set_dur(new_dur);
        break;
      }
      case drag_zone::start_edge:
        if(set_pos_j)
          set_pos_j(std::clamp(m_pos_j0 + dyn, 0.f, 1.f));
        break;
      case drag_zone::end_edge:
        if(set_dur_j)
          set_dur_j(std::clamp(m_dur_j0 + dyn, 0.f, 1.f));
        break;
      default:
        break;
    }
    if(set_pos)
      set_pos(std::clamp(new_pos, 1e-8f, 1.f));
  }

  void mouse_release(double x, double y)
  {
    mouse_move(x, y);
    m_dragging = false;
    m_zone = drag_zone::none;
  }
};

// --- Sound-source controls, gated by the "Multiple sound support" toggle.
// When off: only the enable button shows (click -> turns multi on). When on:
// the file picker + random toggle show. `multi` and `folder` are synced from
// the ports in on_control_update; each widget's `set` drives its bound port.

// Bound to the multi toggle: the enable button (off) / mono affordance (on).
struct MultiButton
{
  static constexpr double width() { return 110.; }
  static constexpr double height() { return 20.; }

  bool value{false}; // multi toggle
  std::function<void(bool)> set;
  std::function<void()> update;

  void paint(auto ctx)
  {
    if(!value)
    {
      ctx.begin_path();
      ctx.set_fill_color(halp::colors::background_darker);
      ctx.draw_rounded_rect(0., 0., width(), height(), 3.);
      ctx.fill();
      ctx.begin_path();
      ctx.set_fill_color({255, 255, 255, 255});
      ctx.set_font_size(9.);
      ctx.draw_text(6., 14., "multiple sound support");
      ctx.fill();
    }
    else
    {
      ctx.begin_path();
      ctx.set_fill_color(halp::colors::mid);
      ctx.set_font_size(8.);
      ctx.draw_text(2., 13., "multi ✓  (click → mono)");
      ctx.fill();
    }
  }

  bool mouse_press(double, double)
  {
    if(set)
      set(!value);
    return true;
  }
};

// Bound to the Sound index port. Lists the folder's files (from the sound
// path, synced UI-side so it works in edit mode); click opens a file menu.
// Hidden when multi is off.
struct SoundPickerItem
{
  static constexpr double width() { return 160.; }
  static constexpr double height() { return 20.; }

  std::string folder;       // containing folder (from the bus)
  std::string current_file; // current sound basename (from the bus)
  int value{0};             // Sound index (synced)
  bool multi{false};        // synced from the multi toggle
  bool m_prev_multi{false}; // to catch the multi off->on transition
  std::function<void(int)> set;
  std::function<void()> update;

  std::vector<std::string> files() const
  {
    std::vector<std::string> out;
    if(folder.empty())
      return out;
    static const QStringList exts{"*.wav", "*.aif", "*.aiff", "*.flac",
                                  "*.mp3", "*.ogg",  "*.m4a"};
    QDir dir(QString::fromStdString(folder));
    for(const auto& f : dir.entryList(exts, QDir::Files, QDir::Name))
      out.push_back(f.toStdString());
    return out;
  }

  void paint(auto ctx)
  {
    if(!multi)
      return;
    ctx.begin_path();
    ctx.set_fill_color(halp::colors::background_darker);
    ctx.draw_rounded_rect(0., 0., width(), height(), 2.);
    ctx.fill();

    const auto fl = files();
    const char* label = fl.empty() ? "(no folder)"
                        : (value >= 0 && value < (int)fl.size()) ? fl[value].c_str()
                                                                 : "-";
    ctx.begin_path();
    ctx.set_fill_color({255, 255, 255, 255});
    ctx.set_font_size(9.);
    ctx.draw_text(4., 14., label);
    ctx.fill();
  }

  bool mouse_press(double, double)
  {
    if(!multi)
      return false;
    const auto fl = files();
    if(fl.empty())
      return true;
    QMenu menu;
    for(int i = 0; i < (int)fl.size(); i++)
    {
      auto* a = menu.addAction(QString::fromStdString(fl[i]));
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

// Bound to the Random toggle; a checkbox shown only when multi is on.
struct RandomToggle
{
  static constexpr double width() { return 70.; }
  static constexpr double height() { return 20.; }

  bool value{false}; // random toggle
  bool multi{false}; // synced from the multi toggle
  std::function<void(bool)> set;
  std::function<void()> update;

  void paint(auto ctx)
  {
    if(!multi)
      return;
    ctx.begin_path();
    ctx.set_fill_color(halp::colors::mid);
    ctx.set_font_size(9.);
    ctx.draw_text(2., 14., value ? "random ✓" : "random ☐");
    ctx.fill();
  }

  bool mouse_press(double, double)
  {
    if(multi && set)
      set(!value);
    return true;
  }
};

// Small static text label with a controllable baseline (a halp::label's
// vertical placement can't be nudged). Used for "win modes".
struct WinModesLabel
{
  static constexpr double width() { return 58.; }
  static constexpr double height() { return 24.; }
  void paint(auto ctx)
  {
    ctx.begin_path();
    ctx.set_fill_color(halp::colors::mid);
    ctx.set_font_size(9.);
    ctx.draw_text(2., 18., "win modes");
    ctx.fill();
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

  // The Sound port and pos/dur/their jitters as dot+label chips. The soundfile
  // chooser stays in the inspector (load there); the panel just shows the dot.
  // The Sound port and pos/dur/their jitters as dot+label chips in one row.
  struct
  {
    halp_meta(name, "Ports")
    halp_meta(layout, hbox)
    halp_meta(background, background_dark)
    halp::custom_control<PortDotValue<std::string, 48>, &ins::sound> sound{
        {.label = "sound"}};
    halp::custom_control<PortDotValue<float, 68>, &ins::pos> position{
        {.label = "position"}};
    halp::custom_control<PortDotValue<float, 68>, &ins::pos_j> pos_jit{
        {.label = "± deviation"}};
    halp::custom_control<PortDotValue<float, 68>, &ins::dur> duration{
        {.label = "duration"}};
    halp::custom_control<PortDotValue<float, 68>, &ins::dur_j> dur_jit{
        {.label = "± deviation"}};
  } ports;

  struct
  {
    halp_meta(name, "Wave")
    halp_meta(layout, hbox)
    halp_meta(background, background_darker)
    halp::custom_actions_item<WaveformItem> waveform;
  } wave_box;

  // Sound-source row (below the waveform): the multi enable button, and (when
  // multi is on) the file picker + random toggle.
  struct
  {
    halp_meta(name, "Source")
    halp_meta(layout, hbox)
    halp_meta(background, background_dark)
    halp::custom_control<MultiButton, &ins::multi> multi_btn;
    halp::custom_control<SoundPickerItem, &ins::sound_index> picker;
    halp::custom_control<RandomToggle, &ins::random> random_tgl;
  } source_box;

  struct
  {
    halp_meta(name, "Controls")
    halp_meta(layout, hbox)
    halp_meta(background, background_dark)
    struct
    {
      halp_meta(name, "Params")
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
        halp::item<&ins::rate_j> rate_j;
      } rate_box;
      struct
      {
        halp_meta(name, "Play")
        halp_meta(layout, hbox)
        halp_meta(background, background_dark)
        halp::item<&ins::playing> playing;
        halp::item<&ins::trig> trig;
      } play_box;
    } params_box;
    struct
    {
      halp_meta(name, "Shape")
      halp_meta(layout, vbox)
      halp_meta(background, background_dark)
      // Reverse to the left of the window-mode selector.
      struct
      {
        halp_meta(name, "Win")
        halp_meta(layout, hbox)
        halp_meta(background, background_dark)
        halp::item<&ins::reverse> reverse;
        halp::custom_control<WindowModeItem, &ins::window_mode> window_mode;
        halp::custom_actions_item<WinModesLabel> wm_label;
      } wm_box;
      // Its title is drawn inside the widget (top-left), like flucoma.
      halp::custom_control<WindowShapeItem, &ins::win_coefs> win_coefs;
    } shape_box;
  } controls;

  void on_control_update()
  {
    auto& sb = controls.shape_box;
    sb.win_coefs.mode = int(sb.wm_box.window_mode.value);
    if(sb.win_coefs.update)
      sb.win_coefs.update();
    if(sb.wm_box.window_mode.update)
      sb.wm_box.window_mode.update();

    auto& wf = wave_box.waveform;
    wf.pos = ports.position.value;
    wf.pos_j = ports.pos_jit.value;
    wf.dur = ports.duration.value;
    wf.dur_j = ports.dur_jit.value;
    wf.sound_path = ports.sound.value; // mono edit-mode display
    wf.reload_from_path();
    if(wf.update)
      wf.update();

    // Sound-source row: the multi toggle gates the picker + random; the picker
    // lists the Sound path's parent folder.
    const bool m = source_box.multi_btn.value;
    auto& pk = source_box.picker;
    pk.multi = m;
    source_box.random_tgl.multi = m;
    // Folder + current file come from the bus (process_message). Enabling multi
    // keeps the current file selected instead of jumping to index 0.
    if(m && !pk.m_prev_multi && !pk.folder.empty())
    {
      const auto files = pk.files();
      for(int i = 0; i < (int)files.size(); i++)
        if(files[i] == pk.current_file)
        {
          if(pk.set)
            pk.set(i);
          break;
        }
    }
    pk.m_prev_multi = m;
    if(pk.update)
      pk.update();
    if(source_box.random_tgl.update)
      source_box.random_tgl.update();
    if(source_box.multi_btn.update)
      source_box.multi_btn.update();
  }

  struct bus
  {
    // ui -> processor
    std::function<void(ui_to_processor)> send_message;
    void init(ui& self)
    {
      // Wire the waveform gestures to the ports' document values. The chip
      // items' `set` hooks are filled by the layout builder (after init).
      auto& wf = self.wave_box.waveform;
      wf.set_pos = [&self](float v) {
        if(auto& c = self.ports.position; c.set)
          c.set(v);
      };
      wf.set_pos_j = [&self](float v) {
        if(auto& c = self.ports.pos_jit; c.set)
          c.set(v);
      };
      wf.set_dur = [&self](float v) {
        if(auto& c = self.ports.duration; c.set)
          c.set(v);
      };
      wf.set_dur_j = [&self](float v) {
        if(auto& c = self.ports.dur_jit; c.set)
          c.set(v);
      };
      // Drop onto the waveform -> load the Sound port / enable multi.
      wf.set_sound = [&self](std::string v) {
        if(auto& c = self.ports.sound; c.set)
          c.set(std::move(v));
      };
      wf.set_multi = [&self](bool v) {
        if(auto& c = self.source_box.multi_btn; c.set)
          c.set(v);
      };
      // A late-created panel asks the processor to resend the envelope.
      send_message(ui_to_processor{});
    }

    // processor -> ui
    static void process_message(ui& self, processor_to_ui msg)
    {
      auto& wf = self.wave_box.waveform;
      wf.min_peaks = std::move(msg.min_peaks);
      wf.max_peaks = std::move(msg.max_peaks);
      wf.name = msg.name;
      wf.duration_s = msg.duration_s;
      if(wf.update)
        wf.update();

      // The soundfile port's path isn't visible to the UI, so the picker takes
      // its folder + current file from the processor here.
      auto& pk = self.source_box.picker;
      pk.folder = std::move(msg.folder);
      pk.current_file = std::move(msg.name);
      if(pk.update)
        pk.update();
    }
  };
};
}
