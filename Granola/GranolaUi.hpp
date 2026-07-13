#pragma once
#include <Granola/GranolaModel.hpp>
#include <halp/custom_widgets.hpp>
#include <halp/layout.hpp>

#include <QCursor>
#include <QFileDialog>
#include <QMenu>

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
  static constexpr double width() { return 220.; }
  static constexpr double height() { return 120.; }

  halp::xy_type<float> value{};
  halp::transaction<halp::xy_type<float>> transaction;
  std::function<void()> update;
  // wired by the layout builder: writes the port's document value (used by
  // the local-params write-back)
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

// halp::control plus the score-side setter hook: the layout builder wires
// `set` to write the port's document value (which then reaches the engine,
// the inspector and back into on_control_update).
template <auto F>
struct settable_control : halp::control<F>
{
  std::function<void(typename halp::control<F>::control_value_type)> set;
};

// A cable connection point without the port's widget: just a small label
// next to the port dot (the dot itself is the layout builder's port item).
struct PortDotItem
{
  static constexpr double width() { return 60.; }
  static constexpr double height() { return 14.; }

  std::string_view label;

  void paint(auto ctx)
  {
    ctx.begin_path();
    ctx.set_fill_color(halp::colors::mid);
    ctx.set_font_size(8.);
    ctx.draw_text(12., 10., label);
    ctx.fill();
  }
};

// Sound picker: a dropdown listing the folder's audio files, scanned in the
// UI itself so it is populated in edit mode too (not only during execution).
// Bound to the 0-based Sound index port; click opens the file menu.
struct SoundPickerItem
{
  static constexpr double width() { return 130.; }
  static constexpr double height() { return 18.; }

  std::string folder; // synced from the Sound folder port in on_control_update
  int value{0};       // Sound index port value (synced)
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
    ctx.begin_path();
    ctx.set_fill_color(halp::colors::background_darker);
    ctx.draw_rounded_rect(0., 0., width(), height(), 2.);
    ctx.fill();

    const auto fl = files();
    const char* label = fl.empty() ? "(no folder)"
                        : (value >= 0 && value < (int)fl.size())
                            ? fl[value].c_str()
                            : "-";
    ctx.begin_path();
    ctx.set_fill_color({255, 255, 255, 255});
    ctx.set_font_size(9.);
    ctx.draw_text(4., 13., label);
    ctx.fill();
  }

  bool mouse_press(double, double)
  {
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

// Window-mode picker: a click dropdown (QMenu) over the three window shapes.
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

// Like PortDotItem but carries the port's value (synced) and a set hook, so
// the waveform can read the current value and drive it. Used for the cable
// ports whose editing widgets are replaced by the waveform gestures.
template <typename T>
struct PortDotValue
{
  static constexpr double width() { return 50.; }
  static constexpr double height() { return 14.; }

  std::string_view label;
  T value{};
  std::function<void(T)> set;

  void paint(auto ctx)
  {
    ctx.begin_path();
    ctx.set_fill_color(halp::colors::mid);
    ctx.set_font_size(8.);
    ctx.draw_text(12., 10., label);
    ctx.fill();
  }
};

// Waveform display for the sound picked by the Sound index port. Peaks come
// from the processor over the message bus (see Granola::processor_to_ui);
// the playback window overlay (position + duration, jitter bands at the
// edges) is synced from the control ports in ui::on_control_update.
//
// Gestures (user-specified map): drag left-right anywhere moves position;
// vertical drags change duration in the central part of the window,
// position jitter near its start edge, duration jitter near its end edge.
struct WaveformItem
{
  static constexpr double width() { return 380.; }
  static constexpr double height() { return 100.; }
  static constexpr double pad = 2.;
  static constexpr double left_pad = pad;

  // Folder path, synced from the Sound folder port this widget is bound to
  std::string value;
  // Sound index port value, synced in ui::on_control_update (0-based)
  int index{0};

  std::vector<float> min_peaks, max_peaks;
  std::string name;
  float duration_s{};

  // Loads the displayed sound UI-side so the widget works in edit mode too
  // (the message bus only runs during execution). Same list/sort/index rules
  // as scan_folder.
  std::string m_loaded_folder;
  int m_loaded_index{-1};

  void maybe_reload()
  {
    if(value == m_loaded_folder && index == m_loaded_index)
      return;
    m_loaded_folder = value;
    m_loaded_index = index;

    min_peaks.clear();
    max_peaks.clear();
    name.clear();
    duration_s = 0.f;
    if(!value.empty())
    {
      static const QStringList exts{"*.wav", "*.aif", "*.aiff", "*.flac",
                                    "*.mp3", "*.ogg",  "*.m4a"};
      QDir dir(QString::fromStdString(value));
      const auto files = dir.entryList(exts, QDir::Files, QDir::Name);
      if(!files.isEmpty())
      {
        const int idx = std::clamp(index, 0, (int)files.size() - 1);
        constexpr double rate = 44100.;
        if(auto dec
           = Media::AudioDecoder::decode_synchronous(dir.filePath(files[idx]), rate))
        {
          BankSound snd;
          snd.data = std::move(dec->second);
          compute_peaks(snd, rate);
          min_peaks = std::move(snd.min_peaks);
          max_peaks = std::move(snd.max_peaks);
          duration_s = snd.duration_s;
          name = files[idx].toStdString();
        }
      }
    }
    if(update)
      update();
  }

  // control port values, synced in ui::on_control_update
  float pos{}, pos_j{}, pos_j_r{1.f};
  float dur{}, dur_j{}, dur_j_r{1.f};

  std::function<void()> update;

  // port setters: `set` is wired by the layout builder to the Sound folder
  // port this widget is bound to; the others are wired in ui::bus::init
  std::function<void(std::string)> set;
  std::function<void(float)> set_pos, set_dur, set_pos_j, set_dur_j;
  std::function<void(int)> set_index;

  // Drop of a sound folder (-> Sound folder port) or of an audio file
  // (-> its folder + its stable name-sorted index, matching scan_folder)
  bool drop(const std::vector<std::string>& paths)
  {
    if(paths.empty())
      return false;
    const QFileInfo fi(QString::fromStdString(paths[0]));
    if(fi.isDir())
    {
      if(set)
        set(fi.absoluteFilePath().toStdString());
      return true;
    }
    static const QStringList exts{"*.wav", "*.aif", "*.aiff", "*.flac",
                                  "*.mp3", "*.ogg",  "*.m4a"};
    const QDir dir = fi.absoluteDir();
    const auto files = dir.entryList(exts, QDir::Files, QDir::Name);
    const int idx = files.indexOf(fi.fileName());
    if(idx < 0)
      return false; // not an audio file
    if(set)
      set(dir.absolutePath().toStdString());
    if(set_index)
      set_index(idx); // 0-based file index
    return true;
  }

  // Empty-state click: native file picker. Choosing a sound loads its folder
  // as the bank and selects it. (Whole folders load by drag & drop.)
  void open_load_dialog()
  {
    const QString f = QFileDialog::getOpenFileName(
        nullptr, "Open sound file", QString::fromStdString(value),
        "Audio (*.wav *.aif *.aiff *.flac *.mp3 *.ogg *.m4a)");
    if(!f.isEmpty())
      drop({f.toStdString()});
  }

  // playback window geometry, shared by paint and gesture hit-testing;
  // full file when dur is 0 or >= 1 (same rule as GranuGrain::set)
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

    ctx.begin_path();
    ctx.set_fill_color(halp::colors::background_darker);
    ctx.draw_rounded_rect(0., 0., w, h, 3.);
    ctx.fill();

    if(!min_peaks.empty() && min_peaks.size() == max_peaks.size())
    {
      // envelope: top edge follows max peaks, bottom edge min peaks
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

      // jitter bands (±2σ; σ = jitter * range / 4): position at the start
      // edge, duration at the end edge
      auto band = ctx.to_rgba(halp::colors::editable_value_light);
      band.a = 40;
      const double pj = pos_j * pos_j_r / 2. * draw_w();
      if(pj > 0.5)
      {
        ctx.begin_path();
        ctx.draw_rect(x0 - pj, pad, 2. * pj, h - 2. * pad);
        ctx.set_fill_color(band);
        ctx.fill();
      }
      const double dj = dur_j * dur_j_r / 2. * draw_w();
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
      std::snprintf(label, sizeof(label), "drop a sound file or folder here");
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
    // Empty (no sound loaded): a click opens the load dialog instead of
    // starting a gesture — no click/drag ambiguity since the two states are
    // mutually exclusive.
    if(min_peaks.empty())
    {
      open_load_dialog();
      return true;
    }

    m_press_x = x;
    m_press_y = y;
    m_pos0 = pos;
    m_dur0 = dur;
    m_pos_j0 = pos_j;
    m_dur_j0 = dur_j;

    constexpr double edge = 10.;
    const double x0 = win_x0(), x1 = win_x1();
    if(std::abs(x - x0) <= edge)
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
    // horizontal: position, always; vertical: zone-dependent, up = increase
    const float dxn = float((x - m_press_x) / draw_w());
    const float dyn = float((m_press_y - y) / (height() - 2. * pad));
    float new_pos = m_pos0 + dxn;
    switch(m_zone)
    {
      case drag_zone::window: {
        // window grows symmetrically: half the duration delta each side
        const float new_dur = std::clamp(m_dur0 + dyn, 1e-8f, 1.f);
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

struct Granola::ui
{
  using enum halp::colors;
  using enum halp::layouts;

  halp_meta(name, "Main")
  halp_meta(layout, vbox)
  halp_meta(background, background_darker)
  halp::label title{"Granulator"};

  // Cable-connection row: folder, file and the position/duration/jitter ports
  // whose editing widgets are replaced by the waveform gestures. Each is a
  // dot + label (no editing widget); the value/set-carrying ones feed the
  // waveform.
  struct
  {
    halp_meta(name, "Ports")
    halp_meta(layout, hbox)
    halp_meta(background, background_dark)
    halp::custom_control<PortDotValue<std::string>, &ins::sound_folder> sound_folder{
        {.label = "sound"}};
    halp::custom_control<PortDotValue<float>, &ins::pos> position{{.label = "pos"}};
    halp::custom_control<PortDotValue<float>, &ins::pos_j> pos_jit{{.label = "posjit"}};
    halp::custom_control<PortDotValue<float>, &ins::dur> duration{{.label = "dur"}};
    halp::custom_control<PortDotValue<float>, &ins::dur_j> dur_jit{{.label = "durjit"}};
  } ports;

  // Drop a folder/file here, or click when empty to open the load dialog.
  // The waveform insets its own content (left_pad) to line up with the
  // port-dotted rows above/below it.
  struct
  {
    halp_meta(name, "Wave")
    halp_meta(layout, hbox)
    halp_meta(background, background_darker)
    halp::custom_actions_item<WaveformItem> waveform;
  } wave_box;

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
        halp::item<&ins::rate_j> rate_j;
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
      halp_meta(name, "Shape")
      halp_meta(layout, vbox)
      halp_meta(background, background_dark)
      // Top row: sound picker + random toggle.
      struct
      {
        halp_meta(name, "Sound")
        halp_meta(layout, hbox)
        halp_meta(background, background_dark)
        halp::custom_control<SoundPickerItem, &ins::sound_index> sound;
        halp::item<&ins::random> random;
      } pick_box;
      // Next row: reverse + window mode.
      struct
      {
        halp_meta(name, "Win")
        halp_meta(layout, hbox)
        halp_meta(background, background_dark)
        halp::item<&ins::reverse> reverse;
        halp::custom_control<WindowModeItem, &ins::window_mode> window_mode;
        halp::label wm_label{"win modes"};
      } wm_box;
      // The window curve fills the remaining space to the bottom; its title
      // is drawn inside the widget (top-left), on the same line as its dot.
      halp::custom_control<WindowShapeItem, &ins::win_coefs> win_coefs;
    } shape_box;
  } controls;

  // Called by score whenever a synced control value changes (and once after
  // the layout is built): keep the custom widgets in sync with the ports.
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
    wf.value = ports.sound_folder.value; // folder path
    wf.index = sb.pick_box.sound.value;
    wf.maybe_reload(); // folder / index changes

    // Keep the sound picker's folder + selection in sync
    sb.pick_box.sound.folder = ports.sound_folder.value;
    if(sb.pick_box.sound.update)
      sb.pick_box.sound.update();

    if(wf.update)
      wf.update();
  }

  struct bus
  {
    // ui -> processor
    std::function<void(ui_to_processor)> send_message;
    void init(ui& self)
    {
      // Wire the waveform gestures to the ports' document values. The
      // items' set hooks are filled by the layout builder, which runs after
      // init_bus — hence the call-time indirection.
      auto& wf = self.wave_box.waveform;
      wf.set = [&self](std::string v) {
        if(auto& c = self.ports.sound_folder; c.set)
          c.set(std::move(v));
      };
      wf.set_index = [&self](int v) {
        if(auto& c = self.controls.shape_box.pick_box.sound; c.set)
          c.set(v);
      };
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

      // The panel may be (re)created long after execution started: ask the
      // processor to resend the current sound's envelope.
      send_message(ui_to_processor{});
    }

    // processor -> ui
    static void process_message(ui& self, processor_to_ui msg)
    {
      auto& wf = self.wave_box.waveform;
      wf.min_peaks = std::move(msg.min_peaks);
      wf.max_peaks = std::move(msg.max_peaks);
      wf.name = std::move(msg.name);
      wf.duration_s = msg.duration_s;
      if(wf.update)
        wf.update();

      // Local-params write-back: push the restored file params into the
      // ports' document values so the inspector and widgets follow.
      if(msg.has_params)
      {
        const auto set = [](auto& control, float v) {
          if(control.set)
            control.set(v);
        };
        set(self.ports.position, msg.params.pos);
        set(self.ports.pos_jit, msg.params.pos_j);
        set(self.ports.duration, msg.params.dur);
        set(self.ports.dur_jit, msg.params.dur_j);
        if(auto& c = self.controls.shape_box.win_coefs; c.set)
          c.set(halp::xy_type<float>{msg.params.win_x, msg.params.win_y});
      }
    }
  };

  //halp::item<&ins::interp_type> interp_type;
  //halp::item<&ins::loopmode> loopmode;
  //halp::item<&ins::src_channels> src_channels;
  //halp::item<&ins::channel_offset> channel_offset;
};
}
