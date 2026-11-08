#pragma once
#include <Granola/GranolaModel.hpp>
#include <halp/custom_widgets.hpp>
#include <halp/layout.hpp>
#include <halp/waveform_view.hpp>

#include <QCollator>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <memory>
#include <optional>

namespace Granola
{
// The "Window coefs" XY port as the grain window it makes, for the current
// window shape. Dragging maps x/y like a pad.
struct WindowShapeItem
{
  static constexpr double width() { return 180.; }
  static constexpr double height() { return 124.; }

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

// Presentation of the controls, as in the other synthesizers of score
inline constexpr halp::look large_knob{.size = halp::control_size::large};
consteval halp::look small_knob_as(std::string_view name)
{
  auto l = halp::label_as(name);
  l.size = halp::control_size::compact;
  l.widget = halp::control_widget::knob;
  return l;
}
consteval halp::look dropdown_as(std::string_view name)
{
  auto l = halp::label_as(name);
  l.widget = halp::control_widget::combo;
  return l;
}
consteval halp::look small_slider_as(std::string_view name)
{
  auto l = halp::label_as(name);
  l.size = halp::control_size::compact;
  return l;
}
// A dropdown the section title already names
inline constexpr halp::look bare_dropdown{
    .hide_label = true, .widget = halp::control_widget::combo};

// A control whose value the panel can also set, e.g. from the waveform's
// gestures
template <auto F, halp::look L = halp::look{}>
struct settable : halp::control<F, L>
{
  std::function<void(typename halp::control<F, L>::control_value_type)> set;
};

// A file or folder port in score's own widget, with its path for the rest of
// the panel. `set` edits it, as one undo step.
template <auto F, halp::look L = halp::look{}>
struct path_control : halp::item_base
{
  static constexpr halp::look presentation = L;
  decltype(F) model = F;
  std::string value;
  std::function<void(std::string)> set;
};

inline QString sound_file_filter()
{
  QString filter = "Sound files (";
  for(const auto& ext :
      QString::fromUtf8(
          Granola::sound_extensions.data(), qsizetype(Granola::sound_extensions.size()))
          .split(' ', Qt::SkipEmptyParts))
    filter += "*." + ext + " ";
  return filter.trimmed() + ")";
}

// The sound that plays, drawn by the host, with the grain window, the jitter
// ranges and the playheads over it; its name, length and channels above.
// Gestures: horizontal drag = position; vertical drag = duration in the window
// centre, position jitter at the start edge, duration jitter at the end edge.
// Empty, a click chooses a sound file, or a folder for the Folder source.
// The files of the folder, for the waveform to show the one that plays
// before the processor runs
struct FolderFiles
{
  // The folder's files, in the order the processor indexes them: natural
  // order, without case
  std::string folder;
  std::string value; // Sound in folder: a file name, or an index
  QStringList files;
  void set_folder(const std::string& f)
  {
    if(f == folder)
      return;
    folder = f;
    files.clear();
    if(!folder.empty())
    {
      QStringList filters;
      for(const auto& ext : QString::fromUtf8(
                                Granola::sound_extensions.data(),
                                qsizetype(Granola::sound_extensions.size()))
                                .split(' ', Qt::SkipEmptyParts))
        filters.push_back("*." + ext);
      files = QDir{QString::fromStdString(folder)}.entryList(filters, QDir::Files);
      QCollator order;
      order.setNumericMode(true);
      order.setCaseSensitivity(Qt::CaseInsensitive);
      std::sort(files.begin(), files.end(), order);
    }
  }

  static std::optional<int> as_index(const std::string& v)
  {
    double d{};
    if(v.empty()
       || std::from_chars(v.data(), v.data() + v.size(), d).ptr != v.data() + v.size())
      return std::nullopt;
    return int(d);
  }

  // The file that plays: the one picked, the one at an index, or the first
  // one when the picked one is not in the folder, as the processor does
  QString current() const
  {
    if(files.empty())
      return {};
    if(auto i = as_index(value))
      return files[std::clamp(*i, 0, int(files.size()) - 1)];
    const auto v = QString::fromStdString(value);
    return files.contains(v) ? v : files.front();
  }
};



struct WaveformItem
{
  // As wide as the panel: the host gives it the width of its section, which
  // is the widest row's
  static constexpr double min_width = 400.;
  double m_width{min_width};
  double width() const noexcept { return m_width; }
  void set_width(double w) noexcept { m_width = std::max(w, min_width); }
  static constexpr double height() { return 160.; }
  static constexpr double pad = 2.;
  static constexpr double left_pad = pad;
  static constexpr double header = 15.; // the name, length and channels
  static constexpr double top() { return pad + header; }
  static constexpr double bottom() { return height() - pad; }

  // Drawn by the host from the file itself, see halp::waveform_view
  halp::waveform_view waveform;
  std::string name;
  // Where the grains of the shown sound read, and whether it could be decoded,
  // from the processor
  Granola::playheads heads{};
  halp::sound_status status{halp::sound_status::ready};

  // The ports the waveform shows and edits, in the order of ui's
  // custom_multi_control, normalized: their ranges are [0, 1]. Kept up to date
  // by the host; a drag edits them through the transaction, as one undo step.
  enum : int { POS, DUR, POS_J, DUR_J };
  std::array<double, 4> values{0., 0.1, 0., 0.};
  halp::multi_transaction transaction;
  float pos() const noexcept { return float(values[POS]); }
  float dur() const noexcept { return float(values[DUR]); }
  float pos_j() const noexcept { return float(values[POS_J]); }
  float dur_j() const noexcept { return float(values[DUR_J]); }
  bool folder_mode{};

  std::function<void()> update;

  // The Sound file's path, synced in on_control_update (the ui struct itself
  // only holds widgets)
  std::string sound_path;
  FolderFiles folder_files;
  // The source as last seen, once seen: a switch to the folder is a change
  // from it, not the panel being built
  bool seen{};
  bool was_folder{};
  // The folder file the processor plays, while it runs: it follows an index
  // sent to Sound in folder, and the first file when the picked one is not in
  // the folder
  std::string playing_file;

  // The file shown: the Sound file, or the file picked in the folder
  void show_file(const std::string& path)
  {
    if(waveform.file == path)
      return;
    waveform.file = path;
    const auto slash = path.find_last_of("/\\");
    name = slash == std::string::npos ? path : path.substr(slash + 1);
    if(update)
      update();
  }
  bool has_sound() const noexcept { return waveform.ready; }

  // Drop onto the waveform: a file plays it, a folder plays its files.
  // Wired in ui::bus::init.
  std::function<void(std::string)> set_sound, set_folder;
  bool drop(const std::vector<std::string>& paths)
  {
    if(paths.empty())
      return false;
    const QFileInfo fi(QString::fromStdString(paths[0]));
    if(fi.isDir())
    {
      if(set_folder)
        set_folder(fi.absoluteFilePath().toStdString());
      return true;
    }
    if(set_sound)
      set_sound(fi.absoluteFilePath().toStdString());
    return true;
  }

  double draw_w() const noexcept { return width() - left_pad - pad; }
  // Position, where Scan has moved it while the grains play
  double shown_pos() const
  {
    if(heads.scan == 0)
      return pos();
    const double p = pos() + heads.scan / 65535.;
    return p - std::floor(p);
  }
  double win_x0() const
  {
    return left_pad + std::clamp(shown_pos(), 0., 1.) * draw_w();
  }
  double win_x1() const
  {
    const double p = shown_pos();
    // The shortest grains are a line: Duration at its minimum is ~0
    const double wdur = dur() >= 1.f ? 1. : std::clamp(std::min(p + double(dur()), 1.) - p, 0., 1.);
    return std::min(win_x0() + wdur * draw_w(), width() - pad);
  }

  void paint(auto ctx)
  {
    const double w = width(), h = height();
    constexpr double y0 = top(), y1 = bottom(), wave_h = bottom() - top();

    // Inset by pad to match score::GraphicsLayout's own background rect.
    ctx.begin_path();
    ctx.set_fill_color(halp::colors::background_darker);
    ctx.draw_rounded_rect(pad, pad, w - 2. * pad, h - 2. * pad, 3.);
    ctx.fill();

    if constexpr(requires { ctx.draw_waveform(waveform, 0., 0., 1., 1.); })
      ctx.draw_waveform(waveform, left_pad, y0, draw_w(), wave_h);

    if(has_sound())
    {
      const double x0 = win_x0();
      const double x1 = win_x1();
      ctx.begin_path();
      ctx.draw_rect(x0, y0, x1 - x0, wave_h);
      auto win = ctx.to_rgba(halp::colors::editable_value_mid);
      win.a = 60;
      ctx.set_fill_color(win);
      ctx.fill();
      ctx.begin_path();
      ctx.draw_line(x0, y0, x0, y1);
      ctx.draw_line(x1, y0, x1, y1);
      ctx.set_stroke_color(halp::colors::editable_value_light);
      ctx.set_stroke_width(1.);
      ctx.stroke();

      // Deviations, both symmetric (±), as whiskers so that the window's
      // fill keeps its edges: position around the start edge at the top,
      // duration around the end edge at the bottom. A jitter j spreads as
      // N(0, j / 4): the whiskers span two deviations.
      auto whisker_color = ctx.to_rgba(halp::colors::editable_value_light);
      whisker_color.a = 200;
      auto whisker = [&](double xa, double xb, double y) {
        ctx.begin_path();
        ctx.move_to(xa, y);
        ctx.line_to(xb, y);
        ctx.move_to(xa, y - 3.);
        ctx.line_to(xa, y + 3.);
        ctx.move_to(xb, y - 3.);
        ctx.line_to(xb, y + 3.);
        ctx.set_stroke_color(whisker_color);
        ctx.set_stroke_width(1.);
        ctx.stroke();
      };
      if(const double pj = pos_j() / 2. * draw_w(); pj > 0.5)
        whisker(x0 - pj, x0 + pj, y0 + 5.);
      if(const double dj = dur_j() / 2. * draw_w(); dj > 0.5)
        whisker(x1 - dj, x1 + dj, y1 - 5.);

      // One bar per playing grain, fading with the grain's window
      auto head = ctx.to_rgba(halp::colors::runtime_value_light);
      ctx.set_stroke_width(1.);
      for(int i = 0; i < heads.count; i++)
      {
        const double px = left_pad + heads.pos[i] / 65535. * draw_w();
        head.a = uint8_t(64 + heads.amp[i] * 191 / 255);
        ctx.begin_path();
        ctx.set_stroke_color(head);
        ctx.move_to(px, y0);
        ctx.line_to(px, y1);
        ctx.stroke();
      }
    }
    else
    {
      ctx.begin_path();
      ctx.set_fill_color(halp::colors::mid);
      ctx.set_font_size(9.);
      ctx.draw_text(
          left_pad + 8., y0 + wave_h / 2. + 3.,
          folder_mode ? "Drop a folder of sounds, or click to choose one"
                      : "Drop a sound file, or click to choose one");
      ctx.fill();
    }

    // Header: name, length, channels; or why there is no waveform
    char label[300];
    if(name.empty())
      label[0] = 0;
    else if(status == halp::sound_status::failed)
      std::snprintf(label, sizeof(label), "%s · cannot be decoded", name.c_str());
    else if(status != halp::sound_status::ready)
      std::snprintf(label, sizeof(label), "%s · loading", name.c_str());
    else if(waveform.duration > 0. && waveform.channels > 0)
      std::snprintf(
          label, sizeof(label), "%s · %.2f s · %d ch", name.c_str(), waveform.duration,
          waveform.channels);
    else
      std::snprintf(label, sizeof(label), "%s", name.c_str());
    ctx.begin_path();
    ctx.set_fill_color(halp::colors::lighter);
    ctx.set_font_size(9.);
    ctx.draw_text(left_pad + 4., pad + 11., label);
    ctx.fill();

    // The header chooses another sound: say so
    if(!name.empty())
    {
      ctx.begin_path();
      ctx.set_fill_color(halp::colors::mid);
      ctx.draw_text(width() - pad - 64., pad + 11., folder_mode ? "Folder…" : "Change…");
      ctx.fill();
    }
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
    // No sound, or a click on its name: choose one
    if(!has_sound() || y < top())
    {
      const QString current = QString::fromStdString(waveform.file);
      if(folder_mode)
      {
        const auto dir = QFileDialog::getExistingDirectory(
            nullptr, "Folder of sounds", QFileInfo{current}.absolutePath());
        if(!dir.isEmpty() && set_folder)
          set_folder(dir.toStdString());
      }
      else
      {
        const auto file = QFileDialog::getOpenFileName(
            nullptr, "Sound file", current, sound_file_filter());
        if(!file.isEmpty() && set_sound)
          set_sound(file.toStdString());
      }
      return true;
    }

    m_press_x = x;
    m_press_y = y;
    m_pos0 = pos();
    m_dur0 = dur();
    m_pos_j0 = pos_j();
    m_dur_j0 = dur_j();

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
    if(transaction.start)
      transaction.start();
    return true;
  }

  void mouse_move(double x, double y)
  {
    if(!m_dragging)
      return;
    const float dxn = float((x - m_press_x) / draw_w());
    const float dyn = float((m_press_y - y) / (bottom() - top()));
    float new_pos = m_pos0 + dxn;
    switch(m_zone)
    {
      case drag_zone::window: {
        const float new_dur = std::clamp(m_dur0 + dyn, 1e-8f, 1.f);
        new_pos -= (new_dur - m_dur0) / 2.f;
        set(DUR, new_dur);
        break;
      }
      case drag_zone::start_edge:
        set(POS_J, std::clamp(m_pos_j0 + dyn, 0.f, 1.f));
        break;
      case drag_zone::end_edge:
        set(DUR_J, std::clamp(m_dur_j0 + dyn, 0.f, 1.f));
        break;
      default:
        break;
    }
    set(POS, std::clamp(new_pos, 1e-8f, 1.f));
  }

  void set(int index, float v)
  {
    if(transaction.update)
      transaction.update(index, v);
  }

  void mouse_release(double x, double y)
  {
    if(!m_dragging)
      return;
    mouse_move(x, y);
    m_dragging = false;
    m_zone = drag_zone::none;
    if(transaction.commit)
      transaction.commit();
  }
};

// In the layout of the other synthesizers of score: titled sections, compact
// knobs and dropdowns, the visual editors next to the controls they edit.
struct Granola::ui
{
  using enum halp::colors;
  using enum halp::layouts;

  halp_meta(name, "Main")
  halp_meta(layout, vbox)
  halp_meta(background, background_dark)

  // Named structs down to the source pages: they hold a static member.
  // Where the sound comes from, and the sound with its grains over it. In the
  // root column, which makes it as wide as the row below: the waveform fills
  // it.
  struct sound_t
  {
    halp_meta(name, "Sound")
    halp_meta(layout, section)
    halp_meta(background, background_mid)

    struct source_t
    {
      halp_meta(layout, hbox)
      settable<&ins::source, bare_dropdown> source;

      struct pages_t
      {
        halp_meta(layout, tabs)
        halp_flag(hide_tabs);
        static constexpr auto model = &ins::source;
        struct
        {
          halp_meta(name, "File")
          halp_meta(layout, hbox)
          // Small, score's widget for a sound file is its name, as for the
          // folder: the waveform is below, with the grains over it
          path_control<&ins::sound, halp::look{.size = halp::control_size::compact}>
              file;
        } file;
        struct
        {
          halp_meta(name, "Folder")
          halp_meta(layout, hbox)
          path_control<&ins::sounds> folder;
          struct : settable<&ins::sound_file, bare_dropdown>
          {
            // Room for file names, whatever the folder's files are
            static constexpr double width() { return 150.; }
          } sound_file;
          halp::item<&ins::random> random;
        } folder;
      } pages;
    } source;

    halp::custom_multi_control<
        WaveformItem, &ins::pos, &ins::dur, &ins::pos_j, &ins::dur_j>
        waveform;
  } sound;

  // In signal order: where and how long grains are and how many, their pitch,
  // their level, and what plays them
  struct
  {
    halp_meta(layout, hbox)

    struct
    {
      halp_meta(name, "Grain")
      halp_meta(layout, section)
      halp_meta(background, background_mid)
      struct
      {
        halp_meta(layout, grid)
        halp_meta(columns, 2)
        halp::item<&ins::pos, small_knob_as("Position")> position;
        halp::item<&ins::pos_j, small_slider_as("Deviation")> pos_jitter;
        halp::item<&ins::dur, small_knob_as("Duration")> duration;
        halp::item<&ins::dur_j, small_slider_as("Deviation")> dur_jitter;
        halp::item<&ins::density, small_knob_as("Density")> density;
        halp::item<&ins::dens_j, small_slider_as("Deviation")> dens_jitter;
      } knobs;
    } grain;

    struct
    {
      halp_meta(name, "Pitch")
      halp_meta(layout, section)
      halp_meta(background, background_mid)
      struct
      {
        halp_meta(layout, grid)
        halp_meta(columns, 2)
        halp::item<&ins::rate, small_knob_as("Pitch")> pitch;
        halp::item<&ins::rate_j, small_slider_as("Deviation")> jitter;
        // How many grains play backwards
        halp::item<&ins::reverse_chance, small_knob_as("Backwards")> backwards;
        struct
        {
          halp_meta(layout, vbox)
          halp::item<&ins::rate_j_semitones, halp::label_as("Semitones")> semitones;
          halp::item<&ins::reverse> reverse;
        } toggles;
      } knobs;
    } pitch;

    struct
    {
      halp_meta(name, "Output")
      halp_meta(layout, section)
      halp_meta(background, background_mid)
      struct
      {
        halp_meta(layout, grid)
        halp_meta(columns, 2)
        halp::item<&ins::gain, small_knob_as("Gain")> gain;
        halp::item<&ins::gain_j, small_slider_as("Deviation")> jitter;
        halp::item<&ins::spread, small_knob_as("Spread")> spread;
        struct
        {
          halp_meta(layout, vbox)
          halp::item<&ins::src_channels, halp::label_as("Channels")> channels;
          halp::item<&ins::channel_offset, halp::label_as("Offset")> offset;
        } channels;
      } knobs;
    } output;

    // The shape of each grain
    struct
    {
      halp_meta(name, "Window")
      halp_meta(layout, section)
      halp_meta(background, background_mid)
      halp::control<&ins::window_mode, bare_dropdown> shape;
      // In a row so that its port sits next to it, not above it
      struct
      {
        halp_meta(layout, hbox)
        halp::custom_control<WindowShapeItem, &ins::win_coefs> pad;
      } editor;
    } window;

    struct
    {
      halp_meta(name, "Play")
      halp_meta(layout, section)
      halp_meta(background, background_mid)
      struct
      {
        halp_meta(layout, hbox)
        halp::item<&ins::playing> playing;
        halp::item<&ins::trig> trigger;
        halp::item<&ins::midi_gate, halp::label_as("MIDI")> midi;
      } transport;
      struct
      {
        halp_meta(layout, hbox)
        halp::item<&ins::num_voices, halp::label_as("Voices")> voices;
        halp::item<&ins::interp_type, dropdown_as("Interpolation")> interpolation;
        halp::item<&ins::loopmode> loop;
      } engine;
      // Position moving by itself; with the folder, across its files or not
      struct
      {
        halp_meta(layout, hbox)
        halp::item<&ins::scan, small_knob_as("Scan")> scan;
        halp::item<&ins::scan_across, halp::label_as("Across sounds")> scan_across;
      } scan;
    } play;
  } params;

  void on_control_update()
  {
    auto& win = params.window;
    win.editor.pad.mode = int(win.shape.value);
    if(win.editor.pad.update)
      win.editor.pad.update();

    auto& src = sound.source;
    auto& page = src.pages.folder;
    auto& wf = sound.waveform;
    wf.sound_path = src.pages.file.file.value;
    wf.folder_mode = from_folder();
    if(wf.update)
      wf.update();
    // Only the panel changes here, but for one case: this also runs on undo
    // and redo, where an edit would cut the redo history.
    wf.folder_files.set_folder(page.folder.value);
    wf.folder_files.value = page.sound_file.value;

    // Switching to the folder while it is empty: the Sound file's folder,
    // with that file picked. score applies these after the switch, and not
    // when the switch is undone or redone.
    const bool folder = from_folder();
    if(wf.seen && folder && !wf.was_folder && page.folder.value.empty()
       && !wf.sound_path.empty() && page.folder.set && page.sound_file.set)
    {
      const QFileInfo fi(QString::fromStdString(wf.sound_path));
      page.folder.set(fi.absolutePath().toStdString());
      page.sound_file.set(fi.fileName().toStdString());
    }
    wf.seen = true;
    wf.was_folder = folder;

    show_current_sound();
  }

  bool from_folder() const noexcept
  {
    using source_type = std::decay_t<decltype(sound.source.source.value)>;
    return sound.source.source.value == source_type::Folder;
  }

  // The waveform shows the Sound file, or the file picked in the folder
  void show_current_sound()
  {
    auto& wf = sound.waveform;
    auto& page = sound.source.pages.folder;
    std::string file = wf.sound_path;
    if(from_folder())
    {
      file.clear();
      const std::string name = !wf.playing_file.empty()
                                   ? wf.playing_file
                                   : wf.folder_files.current().toStdString();
      if(!page.folder.value.empty() && !name.empty())
        file = page.folder.value + "/" + name;
    }
    wf.show_file(file);
  }

  // Stopped: the picked file again, rather than the last one played
  void stop()
  {
    sound.waveform.playing_file.clear();
    show_current_sound();
  }

  struct bus
  {
    // ui -> processor
    std::function<void(ui_to_processor)> send_message;
    void init(ui& self)
    {
      // Drops and choices on the waveform edit the ports' document values. The
      // controls' `set` hooks are filled by the layout builder (after init).
      auto& wf = self.sound.waveform;
      // Drop onto the waveform: a file plays it, a folder plays its files
      auto& src = self.sound.source;
      using source_type = std::decay_t<decltype(src.source.value)>;
      wf.set_sound = [&src](std::string v) {
        if(auto& c = src.pages.file.file; c.set)
          c.set(std::move(v));
        if(src.source.set)
          src.source.set(source_type::File);
      };
      wf.set_folder = [&src](std::string v) {
        if(auto& c = src.pages.folder.folder; c.set)
          c.set(std::move(v));
        if(src.source.set)
          src.source.set(source_type::Folder);
      };
      // A late-created panel asks the processor to resend the current sound.
      send_message(ui_to_processor{});
    }

    // processor -> ui
    static void process_message(ui& self, processor_to_ui msg)
    {
      ossia_variant_alias::visit(
          [&self](const auto& m) { process(self, m); }, msg.message);
    }

    static void process(ui& self, const playheads& heads)
    {
      auto& wf = self.sound.waveform;
      wf.heads = heads;
      if(wf.update)
        wf.update();
    }

    static void process(ui& self, const current_sound& cs)
    {
      auto& wf = self.sound.waveform;
      wf.status = cs.status;
      wf.playing_file = cs.index >= 0 ? std::string{cs.name} : std::string{};
      self.show_current_sound();
      if(wf.update)
        wf.update();
    }
  };
};
}
