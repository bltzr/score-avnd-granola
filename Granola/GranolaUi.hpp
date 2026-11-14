#pragma once
#include <Granola/GranolaModel.hpp>

#include <score/model/Skin.hpp>
#include <score/tools/NaturalSort.hpp>

#include <Crousti/SoundSet.hpp>

#include <avnd/introspection/range.hpp>
#include <halp/custom_widgets.hpp>
#include <halp/layout.hpp>
#include <halp/waveform_view.hpp>

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <tuple>

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

    // The window the grains play
    const auto window = window_shape::from_pad(mode, value.x, value.y);
    std::array<double, N + 1> level;
    for(int i = 0; i <= N; i++)
      level[i] = window(double(i) / N);

    ctx.begin_path();
    ctx.set_fill_color(halp::colors::background_darker);
    ctx.draw_rounded_rect(0., 0., w, h, 3.);
    ctx.fill();

    const auto point_x = [&](int i) { return pad + double(i) / N * (w - 2. * pad); };
    const auto point_y = [&](int i) { return h - pad - level[i] * (h - 2. * pad); };
    ctx.begin_path();
    ctx.move_to(pad, h - pad);
    for(int i = 0; i <= N; i++)
      ctx.line_to(point_x(i), point_y(i));
    ctx.line_to(w - pad, h - pad);
    ctx.close_path();
    auto fill = ctx.to_rgba(halp::colors::editable_value_dark);
    fill.a = 120;
    ctx.set_fill_color(fill);
    ctx.fill();

    ctx.begin_path();
    ctx.move_to(point_x(0), point_y(0));
    for(int i = 1; i <= N; i++)
      ctx.line_to(point_x(i), point_y(i));
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

//! The sound files Granola reads, as file name filters: *.wav and so on,
//! parsed as the host parses a sound set's extensions
inline const QStringList& sound_name_filters()
{
  static const QStringList filters = [] {
    QStringList f;
    for(const auto& ext : oscr::soundSetExtensions(Granola::sound_extensions))
      f.push_back("*." + ext);
    return f;
  }();
  return filters;
}

inline QString sound_file_filter()
{
  return "Sound files (" + sound_name_filters().join(' ') + ")";
}

// The files of the folder, for the waveform to show the one that plays
// before the processor runs
struct FolderFiles
{
  // The folder's files, in the order the host indexes them
  // (score::naturalLess)
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
      files = QDir{QString::fromStdString(folder)}.entryList(
          sound_name_filters(), QDir::Files);
      score::sortNaturally(files);
    }
  }

  // The file that plays: the one picked, the one at an index, or the first
  // one when the picked one is not in the folder, as the processor does
  QString current() const
  {
    if(files.empty())
      return {};
    if(auto i = halp::folder_index(value))
      return files[std::clamp(*i, 0, int(files.size()) - 1)];
    const auto v = QString::fromStdString(value);
    return files.contains(v) ? v : files.front();
  }
};

//! A time chooser's value as score keeps it: seconds (mode 0), or a
//! fraction of a whole note (mode 1 to 3, synced to the tempo)
struct time_value
{
  float x{0.1f};
  float mode{};
  bool synced() const noexcept { return mode != 0.f; }
  //! Its length at \p tempo
  double seconds(double tempo) const noexcept
  {
    return synced() ? 4. * x * 60. / tempo : x;
  }
  bool operator==(const time_value&) const noexcept = default;
};

//! Note values as score's time chooser names them: 1/8, 1/8. (dotted),
//! 1/8T (triplet), 2/1; else the fraction of a whole note
inline void note_name(char* out, std::size_t size, double x)
{
  const auto name = [&](double straight, const char* suffix) {
    if(straight >= 1.)
      std::snprintf(out, size, "%d/1%s", int(std::lround(straight)), suffix);
    else
      std::snprintf(out, size, "1/%d%s", int(std::lround(1. / straight)), suffix);
  };
  const auto power_of_two = [](double v) {
    const double e = std::log2(v);
    return v > 0. && std::abs(e - std::round(e)) < 1e-4;
  };
  if(power_of_two(x))
    name(x, "");
  else if(power_of_two(x / 1.5))
    name(x / 1.5, ".");
  else if(power_of_two(x * 1.5))
    name(x * 1.5, "T");
  else
    std::snprintf(out, size, "%.3g of a whole note", x);
}

// Where the sound comes from, as a dropdown of score draws it, with its +/-
// strip. A click picks the other source, + Folder and - File: the user's
// pick, which pick() sees, and which fills an empty Folder (see ui::bus).
struct SourceItem
{
  using source_type = std::decay_t<decltype(Granola::ins{}.source.value)>;
  static constexpr double width() { return 64.; }
  // As tall as score's dropdowns
  static constexpr double height() { return 23.; }
  static constexpr double strip = 12.; // score::Stepper::width

  source_type value{};
  // Edits the Source port (filled by the host)
  std::function<void(source_type)> set;
  std::function<void()> update;
  // The user picked a source (wired in ui::bus::init)
  std::function<void(source_type)> pick;

  void paint(auto ctx)
  {
    constexpr double w = width(), h = height();
    // In the colours and font of score's dropdowns
    const auto& skin = score::Skin::instance();
    const auto color = [&](const QColor& q) {
      auto c = ctx.to_rgba(halp::colors::mid);
      c.r = q.red();
      c.g = q.green();
      c.b = q.blue();
      c.a = q.alpha();
      return c;
    };
    const auto box = color(skin.Emphasis2.main.brush.color());
    const auto text = color(skin.Base4.main.brush.color());


    // The box, its orange edge
    ctx.begin_path();
    ctx.set_fill_color(box);
    ctx.draw_rounded_rect(1., 1., w - 2., h - 2., 1.);
    ctx.fill();

    ctx.begin_path();
    ctx.set_stroke_color(text);
    ctx.set_stroke_width(2.);
    ctx.move_to(2., 2.);
    ctx.line_to(2., h - 2.);
    ctx.stroke();
    // The strip: + in its upper half, - in its lower one
    ctx.begin_path();
    ctx.set_stroke_width(1.);
    const double cx = std::round(w - 1. - strip / 2.);
    const double up = std::round(1. + (h - 2.) / 4.);
    const double down = std::round(h - 1. - (h - 2.) / 4.);
    ctx.move_to(cx - 2., up);
    ctx.line_to(cx + 2., up);
    ctx.move_to(cx, up - 2.);
    ctx.line_to(cx, up + 2.);
    ctx.move_to(cx - 2., down);
    ctx.line_to(cx + 2., down);
    ctx.stroke();

    // The source, centred in the box left of the strip
    const std::string_view name = value == source_type::Folder ? "Folder" : "File";
    ctx.begin_path();
    ctx.set_fill_color(text);
    ctx.set_font(skin.Medium10Pt.family().toStdString());
    ctx.set_font_size(skin.Medium10Pt.pixelSize());
    const double left = 5., right = w - 1. - strip;
    ctx.draw_text(left + (right - left - ctx.text_width(name)) / 2., h / 2. + 4.5, name);
    ctx.fill();
  }

  bool mouse_press(double x, double y)
  {
    source_type v = value == source_type::Folder ? source_type::File : source_type::Folder;
    if(x >= width() - 1. - strip)
      v = y < height() / 2. ? source_type::Folder : source_type::File;
    if(v == value)
      return true;
    if(pick)
      pick(v);
    else if(set)
      set(v);
    return true;
  }
  void mouse_move(double, double) { }
  void mouse_release(double, double) { }
};

// The sound that plays, drawn by the host, with the grain window, the
// deviations and the playheads over it; its name, length and channels above;
// the region (Sample Start, Sample End) with a handle at each edge.
// Gestures: horizontal drag = position; vertical drag = duration in the
// window's middle, position deviation at its start edge, duration deviation
// at its end edge. Empty, or on its name, a click chooses a sound file, or a
// folder for the Folder source.
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
  // custom_multi_control, as their own values: Duration Time a time
  // chooser's {x, mode}, Duration Mode an entry's name (or its index). Kept
  // up to date by the host; a drag edits them through the transactions, as
  // one undo step. Duration Mode is only shown.
  enum : int { POS, DUR, POS_J, DUR_J, START, END, DUR_TIME, DUR_MODE };
  using values_type
      = std::tuple<float, float, float, float, float, float, time_value, std::string>;
  values_type values{0.f, 0.1f, 0.f, 0.f, 0.f, 1.f, time_value{}, std::string{"Region"}};
  // What execution gives the same ports, while it does: drawn apart from
  // values, which stay the ones the user edits. Kept by the host. Duration
  // Time's is in seconds, at the tempo, in x; its mode means nothing.
  std::tuple<
      std::optional<float>, std::optional<float>, std::optional<float>,
      std::optional<float>, std::optional<float>, std::optional<float>,
      std::optional<time_value>, std::optional<std::string>>
      execution_values{};
  std::tuple<
      halp::transaction<float>, halp::transaction<float>, halp::transaction<float>,
      halp::transaction<float>, halp::transaction<float>, halp::transaction<float>,
      halp::transaction<time_value>>
      transactions;

  //! The value of one of the six controls of the region, POS to END
  double value(int i) const noexcept
  {
    switch(i)
    {
      case POS: return std::get<POS>(values);
      case DUR: return std::get<DUR>(values);
      case POS_J: return std::get<POS_J>(values);
      case DUR_J: return std::get<DUR_J>(values);
      case START: return std::get<START>(values);
      default: return std::get<END>(values);
    }
  }
  //! What execution gives one of them, if it does
  std::optional<double> execution(int i) const noexcept
  {
    const auto& e = execution_values;
    switch(i)
    {
      case POS: return std::get<POS>(e);
      case DUR: return std::get<DUR>(e);
      case POS_J: return std::get<POS_J>(e);
      case DUR_J: return std::get<DUR_J>(e);
      case START: return std::get<START>(e);
      default: return std::get<END>(e);
    }
  }
  static bool time_mode(const std::string& m) noexcept { return m == "Time" || m == "1"; }

  float pos() const noexcept { return std::get<POS>(values); }
  float dur() const noexcept { return std::get<DUR>(values); }
  float pos_j() const noexcept { return std::get<POS_J>(values); }
  float dur_j() const noexcept { return std::get<DUR_J>(values); }
  const time_value& dur_time() const noexcept { return std::get<DUR_TIME>(values); }
  bool timed() const noexcept { return time_mode(std::get<DUR_MODE>(values)); }
  // The region the others are fractions of, as the processor reads it
  double region_lo() const noexcept
  {
    return std::clamp(std::min(value(START), value(END)), 0., 1.);
  }
  double region_len() const noexcept
  {
    return std::clamp(std::max(value(START), value(END)), 0., 1.) - region_lo();
  }
  //! The tempo the processor plays at, or score's own until it says
  double tempo() const noexcept { return heads.tempo > 0.f ? heads.tempo : 120.; }
  //! \p seconds of the sound as a part of a region of \p len
  double seconds_in_region(double seconds, double len) const noexcept
  {
    if(!(waveform.duration > 0.) || !(len > 0.))
      return 0.;
    return seconds / (waveform.duration * len);
  }
  //! The grain at speed 1 as a part of the region: Duration, or Duration
  //! Time in the sound shown
  double grain_part() const noexcept
  {
    return timed() ? seconds_in_region(dur_time().seconds(tempo()), region_len()) : dur();
  }
  //! x of a fraction of the sound
  double x_of(double abs) const noexcept { return left_pad + abs * draw_w(); }
  //! x of a fraction of the region
  double x_in_region(double rel) const noexcept
  {
    return x_of(region_lo() + std::clamp(rel, 0., 1.) * region_len());
  }
  bool folder_mode{};

  std::function<void()> update;

  // The Sound file's path, synced in on_control_update (the ui struct itself
  // only holds widgets)
  std::string sound_path;
  FolderFiles folder_files;
  // Expires with the panel (the ui struct this is part of)
  std::shared_ptr<char> m_alive = std::make_shared<char>();
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
    return heads.scanning ? heads.scan / Granola::playheads::pos_scale : pos();
  }
  double win_x0() const { return x_in_region(shown_pos()); }
  double win_x1() const
  {
    // The shortest grains are a line: Duration at its minimum is ~0. A
    // grain longer than what is left of the region ends at its end, as the
    // processor's do.
    return x_in_region(std::clamp(shown_pos(), 0., 1.) + std::clamp(grain_part(), 0., 1.));
  }

  //! The window as execution plays it, while execution gives Position, the
  //! grain's length or mode, or the region, and it is not the one set: x0, x1
  std::optional<std::pair<double, double>> execution_window() const
  {
    const auto& e = execution_values;
    const auto& e_time = std::get<DUR_TIME>(e);
    const auto& e_mode = std::get<DUR_MODE>(e);
    if(!execution(POS) && !execution(DUR) && !execution(START) && !execution(END) && !e_time
       && !e_mode)
      return std::nullopt;
    auto at = [&](int i) { return std::clamp(execution(i).value_or(value(i)), 0., 1.); };
    const double lo = std::min(at(START), at(END)), len = std::max(at(START), at(END)) - lo;
    const double p = heads.scanning ? heads.scan / Granola::playheads::pos_scale : at(POS);
    const bool timed = time_mode(e_mode.value_or(std::get<DUR_MODE>(values)));
    const double part
        = !timed ? at(DUR)
                 : seconds_in_region(
                       e_time ? e_time->x : dur_time().seconds(tempo()), len);
    auto x = [&](double rel) { return x_of(lo + std::clamp(rel, 0., 1.) * len); };
    const std::pair w{x(p), x(std::clamp(p, 0., 1.) + std::clamp(part, 0., 1.))};
    // The same window as the one set, e.g. Duration Time's seconds for its
    // note value at this tempo: nothing apart to show
    if(std::abs(w.first - win_x0()) < 0.5 && std::abs(w.second - win_x1()) < 0.5)
      return std::nullopt;
    return w;
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
      // Outside of the region: dimmed, and its edges
      const double rx0 = x_of(region_lo()), rx1 = x_of(region_lo() + region_len());
      auto outside = ctx.to_rgba(halp::colors::background_darker);
      outside.a = 190;
      ctx.begin_path();
      if(rx0 > left_pad)
        ctx.draw_rect(left_pad, y0, rx0 - left_pad, wave_h);
      if(rx1 < left_pad + draw_w())
        ctx.draw_rect(rx1, y0, left_pad + draw_w() - rx1, wave_h);
      ctx.set_fill_color(outside);
      ctx.fill();
      if(region_len() < 1.)
      {
        ctx.begin_path();
        ctx.draw_line(rx0, top() - 4., rx0, y1);
        ctx.draw_line(rx1, top() - 4., rx1, y1);
        ctx.set_stroke_color(halp::colors::lighter);
        ctx.set_stroke_width(1.);
        ctx.stroke();
      }
      // Handles in the header to drag them by
      ctx.begin_path();
      for(double hx : {rx0, rx1})
      {
        ctx.move_to(hx - 4., pad + 2.);
        ctx.line_to(hx + 4., pad + 2.);
        ctx.line_to(hx, top() - 3.);
        ctx.line_to(hx - 4., pad + 2.);
      }
      ctx.set_fill_color(halp::colors::lighter);
      ctx.fill();

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
      // duration around the end edge at the bottom. A Deviation d spreads as
      // N(0, d / 4): the whiskers span two of those.
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
      // Fractions of the region; in Time mode, Duration Deviation as a
      // factor of the grain, 2^(d / 2) either way (two of its deviations)
      if(const double pj = pos_j() / 2. * region_len() * draw_w(); pj > 0.5)
        whisker(x0 - pj, x0 + pj, y0 + 5.);
      if(timed())
      {
        const double f = std::exp2(dur_j() / 2.);
        if(dur_j() > 0.f && (x1 - x0) * (f - 1. / f) > 1.)
          whisker(x0 + (x1 - x0) / f, x0 + (x1 - x0) * f, y1 - 5.);
      }
      else if(const double dj = dur_j() / 2. * region_len() * draw_w(); dj > 0.5)
        whisker(x1 - dj, x1 + dj, y1 - 5.);

      // The window execution plays, outlined in the colour of runtime values
      if(const auto ew = execution_window())
      {
        const auto [ex0, ex1] = *ew;
        auto fill = ctx.to_rgba(halp::colors::runtime_value_mid);
        fill.a = 40;
        ctx.begin_path();
        ctx.draw_rect(ex0, y0, ex1 - ex0, wave_h);
        ctx.set_fill_color(fill);
        ctx.fill();
        ctx.begin_path();
        ctx.draw_rect(ex0, y0 + 1., ex1 - ex0, wave_h - 2.);
        ctx.set_stroke_color(halp::colors::runtime_value_light);
        ctx.set_stroke_width(1.);
        ctx.stroke();
      }

      // One bar per playing grain, fading with the grain's window
      auto head = ctx.to_rgba(halp::colors::runtime_value_light);
      ctx.set_stroke_width(1.);
      for(int i = 0; i < std::min<int>(heads.count, Granola::playheads::max_heads); i++)
      {
        const double px = left_pad + heads.pos[i] / Granola::playheads::pos_scale * draw_w();
        head.a = uint8_t(64 + heads.amp[i] * 191 / int(Granola::playheads::amp_scale));
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

    // Header: name, length, channels, in Time mode the grain's; or why
    // there is no waveform
    char label[340];
    if(name.empty())
      label[0] = 0;
    else if(status == halp::sound_status::failed)
      std::snprintf(label, sizeof(label), "%s, cannot be decoded", name.c_str());
    else if(status != halp::sound_status::ready)
      std::snprintf(label, sizeof(label), "%s, loading", name.c_str());
    else if(waveform.duration > 0. && waveform.channels > 0)
    {
      char grain[40] = "";
      if(timed())
      {
        char length[32];
        grain_length_name(length, sizeof(length));
        std::snprintf(grain, sizeof(grain), ", grain %s", length);
      }
      std::snprintf(
          label, sizeof(label), "%s, %.2f s, %d ch%s", name.c_str(), waveform.duration,
          waveform.channels, grain);
    }
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
      ctx.draw_text(width() - pad - 64., pad + 11., folder_mode ? "Folder" : "Change");
      ctx.fill();
    }
  }

  //! Duration Time as the header names it: its note value, or its time
  void grain_length_name(char* out, std::size_t size) const
  {
    const auto& t = dur_time();
    if(t.synced())
      note_name(out, size, t.x);
    else if(t.x >= 1.f)
      std::snprintf(out, size, "%.2f s", t.x);
    else if(t.x >= 0.01f)
      std::snprintf(out, size, "%.0f ms", t.x * 1000.);
    else
      std::snprintf(out, size, "%.1f ms", t.x * 1000.);
  }

  enum class drag_zone
  {
    none,
    window,     // vertical -> duration, scaled
    start_edge, // vertical -> position deviation, scaled with the duration
    end_edge,   // vertical -> duration deviation, scaled with the duration
    region      // a handle of the region, in the header: Sample Start or End
  };
  int m_region_edge{START};
  drag_zone m_zone{drag_zone::none};
  double m_press_x{}, m_press_y{};
  float m_pos0{}, m_dur0{}, m_pos_j0{}, m_dur_j0{};
  time_value m_time0{};
  bool m_timed{};
  static constexpr float px_per_doubling = 40.f;
  static constexpr float max_doublings = 64.f;
  bool m_dragging{false};

  //! The note value \p x moved by \p steps doublings, within the values of
  //! its feel score's time chooser offers: each feel is a series of
  //! doublings, straight 1/64 to 4/1, dotted 1/32. to 2/1., triplet 1/32T
  //! to 1/1T
  static double step_note(double x, int steps) noexcept
  {
    const auto feel_of = [](double v) {
      const double e = std::log2(v);
      if(std::abs(e - std::round(e)) < 1e-4)
        return 0; // straight
      const double d = std::log2(v / 1.5);
      return std::abs(d - std::round(d)) < 1e-4 ? 1 : 2; // dotted, else triplet
    };
    static constexpr double lo[]{1. / 64., 3. / 64., 1. / 48.};
    static constexpr double hi[]{4., 3., 2. / 3.};
    const int f = x > 0. ? feel_of(x) : 0;
    if(!(x > 0.))
      x = 1. / 8.;
    return std::clamp(x * std::exp2(steps), lo[f], hi[f]);
  }
  void start_gesture()
  {
    if(auto& s = std::get<POS>(transactions).start)
      s();
  }
  void commit_gesture()
  {
    if(auto& c = std::get<POS>(transactions).commit)
      c();
  }

  bool mouse_press(double x, double y)
  {
    // A handle of the region
    if(has_sound() && y < top())
    {
      constexpr double grab = 6.;
      const double rx0 = x_of(region_lo()), rx1 = x_of(region_lo() + region_len());
      if(std::abs(x - rx0) <= grab || std::abs(x - rx1) <= grab)
      {
        // The nearest; the end when both are at the same place and it is
        // dragged from the left of the sound
        const bool lo = std::abs(x - rx0) < std::abs(x - rx1)
                        || (rx0 == rx1 && rx0 > left_pad + draw_w() / 2.);
        // Which port is the low one: Start and End may be the other way
        const bool start_is_lo = value(START) <= value(END);
        m_region_edge = (lo == start_is_lo) ? START : END;
        m_zone = drag_zone::region;
        m_dragging = true;
        start_gesture();
        return true;
      }
    }

    // No sound, or a click on its name: choose one
    if(!has_sound() || y < top())
    {
      // Once the click is handled: a dialog runs an event loop, in which the
      // host may rebuild the panel, deleting the item the click is for
      QTimer::singleShot(0, qApp, [this, alive = std::weak_ptr<char>{m_alive}] {
        if(alive.expired())
          return;
        const QString current = QString::fromStdString(waveform.file);
        if(folder_mode)
        {
          const auto dir = QFileDialog::getExistingDirectory(
              nullptr, "Folder of sounds", QFileInfo{current}.absolutePath());
          // The panel may be gone by now as well
          if(!dir.isEmpty() && !alive.expired() && set_folder)
            set_folder(dir.toStdString());
        }
        else
        {
          const auto file = QFileDialog::getOpenFileName(
              nullptr, "Sound file", current, sound_file_filter());
          if(!file.isEmpty() && !alive.expired() && set_sound)
            set_sound(file.toStdString());
        }
      });
      return true;
    }

    m_press_x = x;
    m_press_y = y;
    m_pos0 = pos();
    // The window's width, as a part of the region, in either mode
    m_timed = timed();
    m_dur0 = float(std::clamp(grain_part(), 0., 1.));
    m_time0 = dur_time();
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
    start_gesture();
    return true;
  }

  void mouse_move(double x, double y)
  {
    if(!m_dragging)
      return;
    if(m_zone == drag_zone::region)
    {
      set(m_region_edge, float(std::clamp((x - left_pad) / draw_w(), 0., 1.)));
      return;
    }
    // A fraction of the region
    const float dxn = float((x - m_press_x) / (draw_w() * std::max(region_len(), 1e-6)));
    // Up and down scale what is dragged, by two every px_per_doubling, the
    // same at any length of the sound and of the grain. Past max_doublings
    // either way it is all of the region, or none of it.
    const float doublings = std::clamp(
        float(m_press_y - y) / px_per_doubling, -max_doublings, max_doublings);
    const float scale = std::exp2(doublings);
    // What scales is at least a pixel of the region: from a Duration of ~0 a
    // drag still grows it, a pixel more for the first doubling
    const float pixel = float(1. / std::max(draw_w() * region_len(), 1.));
    const float base = std::max(m_dur0, pixel);
    // Duration grows by what `base` grows: by its own scale when it is longer
    // than a pixel. A Deviation may be 0: Deviation + base is what scales, so
    // that from 0 one doubling adds a Duration, and down it comes back to 0.
    auto deviation = [&](float j0) {
      return std::clamp(j0 * scale + base * (scale - 1.f), 0.f, 1.f);
    };
    float new_pos = m_pos0 + dxn;
    switch(m_zone)
    {
      case drag_zone::window:
        if(m_timed)
        {
          // Seconds scale as Duration does; a note value steps to the next
          // one of its feel every px_per_doubling, and stays a note value
          time_value t = m_time0;
          if(t.synced())
            t.x = float(step_note(t.x, int(std::lround(doublings))));
          else
            t.x = std::clamp(
                float(t.x * scale), float(Granola::min_grain_seconds), float(max_time));
          const double was = m_dur0;
          const double now
              = std::clamp(seconds_in_region(t.seconds(tempo()), region_len()), 0., 1.);
          new_pos -= float(now - was) / 2.f;
          set_time(t);
        }
        else
        {
          const float new_dur = std::clamp(m_dur0 + base * (scale - 1.f), 1e-8f, 1.f);
          new_pos -= (new_dur - m_dur0) / 2.f;
          set(DUR, new_dur);
        }
        break;
      case drag_zone::start_edge:
        set(POS_J, deviation(m_pos_j0));
        break;
      case drag_zone::end_edge:
        // Time mode: a factor of the grain, as the processor reads it: 1 +
        // Deviation scales
        if(m_timed)
          set(DUR_J, std::clamp((m_dur_j0 + 1.f) * scale - 1.f, 0.f, 1.f));
        else
          set(DUR_J, deviation(m_dur_j0));
        break;
      default:
        break;
    }
    set(POS, std::clamp(new_pos, 1e-8f, 1.f));
  }

  // Duration Time's range
  static constexpr double max_time
      = avnd::get_range<std::decay_t<decltype(Granola::ins{}.dur_time)>>().max;

  //! One of the six controls of the region, POS to END
  void set(int index, float v)
  {
    const auto go = [v](auto& tr) {
      if(tr.update)
        tr.update(v);
    };
    auto& t = transactions;
    switch(index)
    {
      case POS: go(std::get<POS>(t)); break;
      case DUR: go(std::get<DUR>(t)); break;
      case POS_J: go(std::get<POS_J>(t)); break;
      case DUR_J: go(std::get<DUR_J>(t)); break;
      case START: go(std::get<START>(t)); break;
      case END: go(std::get<END>(t)); break;
      default: break;
    }
  }
  void set_time(const time_value& v)
  {
    if(auto& u = std::get<DUR_TIME>(transactions).update)
      u(v);
  }

  void mouse_release(double x, double y)
  {
    if(!m_dragging)
      return;
    mouse_move(x, y);
    m_dragging = false;
    m_zone = drag_zone::none;
    commit_gesture();
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

    // Where the sound comes from and which file: one row, the labels above
    // the controls, the cells at the top and left
    struct source_t
    {
      halp_meta(layout, grid)
      // The Source, then the file or the folder's
      halp_meta(columns, 2)
      halp_meta(alignment, halp::alignments::left)
      halp_meta(spacing, 6)
      // The Source selector as a control of score: its label above it, from
      // its left edge, its port beside it
      struct
      {
        halp_meta(layout, vbox)
        halp_meta(padding, 0)
        struct
        {
          halp_meta(layout, hbox)
          halp_meta(padding, 0)
          // Over the port and the gap after it, as score's labels start at
          // their control
          halp::spacing port{.width = 22, .height = 0};
          halp::label text{"Source"};
        } label;
        struct
        {
          halp_meta(layout, hbox)
          halp_meta(padding, 0)
          halp::custom_control<SourceItem, &ins::source> source;
        } box;
      } selector;

      struct pages_t
      {
        halp_meta(layout, tabs)
        halp_flag(hide_tabs);
        halp_meta(padding, 0)
        static constexpr auto model = &ins::source;
        struct
        {
          halp_meta(name, "File")
          halp_meta(layout, hbox)
          halp_meta(alignment, halp::alignments::left)
          // Small, score's widget for a sound file is its name, as for the
          // folder: the waveform is below, with the grains over it
          path_control<&ins::sound, halp::look{.size = halp::control_size::compact}>
              file;
        } file;
        struct
        {
          halp_meta(name, "Folder")
          halp_meta(layout, grid)
          halp_meta(columns, 3)
          halp_meta(alignment, halp::alignments::left)
          halp_meta(padding, 0)
          halp_meta(spacing, 6)
          path_control<&ins::sounds, halp::label_as("Folder")> folder;
          struct : settable<&ins::sound_file, dropdown_as("File")>
          {
            // Room for file names, whatever the folder's files are
            static constexpr double width() { return 150.; }
          } sound_file;
          // Which file each grain plays
          halp::item<&ins::order, dropdown_as("Order")> order;
        } folder;
      } pages;
    } source;

    halp::custom_multi_control<
        WaveformItem, &ins::pos, &ins::dur, &ins::pos_j, &ins::dur_j,
        &ins::sample_start, &ins::sample_end, &ins::dur_time, &ins::dur_mode>
        waveform;
  } sound;

  // What plays the grains, then in signal order: where and how long grains
  // are and how many, their pitch, their level, their shape. Named structs
  // down to Duration's pages, as for the source's
  struct params_t
  {
    halp_meta(layout, hbox)

    struct
    {
      halp_meta(name, "Play")
      halp_meta(layout, section)
      halp_meta(background, background_mid)
      // When grains come and how many at once
      struct
      {
        halp_meta(layout, grid)
        halp_meta(columns, 2)
        halp_meta(alignment, halp::alignments::left)
        halp::item<&ins::playing> playing;
        halp::item<&ins::trig> trigger;
        halp::item<&ins::density, small_knob_as("Density")> density;
        halp::item<&ins::dens_j, small_slider_as("Deviation")> dens_jitter;
        halp::item<&ins::num_voices, halp::label_as("Max voices")> voices;
        // How much velocity sets the level of the notes' grains
        halp::item<&ins::velocity, small_knob_as("Velocity")> velocity;
        // The keys of On MIDI's slices and of By MIDI note's files, over it
        halp::item<&ins::low_note, halp::label_as("Low note")> low_note;
        halp::item<&ins::high_note, halp::label_as("High note")> high_note;
        // The grid grains start on
        halp::item<&ins::quantize, halp::label_as("Quantize")> quantize;
      } controls;
      // What MIDI notes do, the files they pick being Order's: under the
      // others, as wide as its longest entry
      struct
      {
        halp_meta(layout, hbox)
        halp_meta(padding, 0)
        halp_meta(alignment, halp::alignments::left)
        halp::item<&ins::on_midi, dropdown_as("On MIDI")> on_midi;
      } midi;
    } play;

    struct grain_t
    {
      halp_meta(name, "Grain")
      halp_meta(layout, section)
      halp_meta(background, background_mid)
      // Where in the sound grains read, how long, and how: Position and
      // Duration, the region, then Scan, which moves Position, in a column
      struct knobs_t
      {
        halp_meta(layout, grid)
        halp_meta(columns, 3)
        // Each column flush left: its items line up under each other
        halp_meta(alignment, halp::alignments::left)
        halp::item<&ins::pos, small_knob_as("Position")> position;
        halp::item<&ins::pos_j, small_slider_as("Deviation")> pos_jitter;
        // Position moving by itself
        halp::item<&ins::scan, small_knob_as("Scan")> scan;
        // Duration Mode's control: the part of the region, or the time
        struct duration_t
        {
          halp_meta(layout, tabs)
          halp_flag(hide_tabs);
          halp_meta(padding, 0)
          static constexpr auto model = &ins::dur_mode;
          struct
          {
            halp_meta(name, "Region")
            halp_meta(layout, hbox)
            halp_meta(padding, 0)
            halp::item<&ins::dur, small_knob_as("Duration")> duration;
          } region;
          struct
          {
            halp_meta(name, "Time")
            halp_meta(layout, hbox)
            halp_meta(padding, 0)
            // score's time chooser: its knob and its readout of seconds or
            // a note value
            halp::item<&ins::dur_time, small_slider_as("Duration")> duration;
          } time;
        } duration;
        halp::item<&ins::dur_j, small_slider_as("Deviation")> dur_jitter;
        halp::item<&ins::scan_mode, dropdown_as("Scan mode")> scan_mode;
        // The part of the sound grains read; also by the handles over the
        // waveform
        halp::item<&ins::sample_start, small_slider_as("Start")> sample_start;
        halp::item<&ins::sample_end, small_slider_as("End")> sample_end;
        // With the folder's files: only for them
        struct
        {
          halp_meta(layout, hbox)
          halp_meta(padding, 0)
          halp_meta(alignment, halp::alignments::left)
          static constexpr auto condition()
          {
            using source = std::decay_t<decltype(ins{}.source.value)>;
            return halp::enabled_when<&ins::source, source::Folder>{};
          }
          halp::item<&ins::scan_folder, dropdown_as("Scan in folder")> scan_folder;
        } scan_folder;
        // Grains reaching the end of the region go on from its start
        halp::item<&ins::loopmode, halp::label_as("Wrap at end")> wrap;
        // Whether Duration is a part of the region or a time
        halp::item<&ins::dur_mode, dropdown_as("Duration mode")> dur_mode;
        // The same length and pace at any pitch
        halp::item<&ins::keep_time> keep_time;
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
        // Each column flush left: its items line up under each other
        halp_meta(alignment, halp::alignments::left)
        halp::item<&ins::rate, small_knob_as("Pitch")> pitch;
        halp::item<&ins::rate_j, small_slider_as("Deviation")> jitter;
        // Under Pitch: the deviation in whole semitones, at any pitch
        halp::item<&ins::rate_j_semitones, halp::label_as("Semitones")> semitones;
        // How many grains play backwards
        halp::item<&ins::reverse_chance, small_knob_as("Backwards")> backwards;
        // How the sound is read between its samples, at any speed
        halp::item<&ins::interp_type, dropdown_as("Interpolation")> interpolation;
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
        // Each column flush left: its items line up under each other
        halp_meta(alignment, halp::alignments::left)
        halp::item<&ins::gain, small_knob_as("Gain")> gain;
        halp::item<&ins::gain_j, small_slider_as("Deviation")> jitter;
        halp::item<&ins::spread, small_knob_as("Spread")> spread;
        // Spread places the grains among them
        halp::item<&ins::out_channels, halp::label_as("Outputs")> outputs;
        // The sound's channels the grains read, from Offset
        halp::item<&ins::src_channels, halp::label_as("Channels")> channels;
        halp::item<&ins::channel_offset, halp::label_as("Offset")> offset;
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
    // Only the panel changes here: this runs for every value a port gets,
    // from execution too, and on undo and redo
    wf.folder_files.set_folder(page.folder.value);
    wf.folder_files.value = page.sound_file.value;

    show_current_sound();
    if(wf.update)
      wf.update();
  }

  bool from_folder() const noexcept
  {
    using source_type = std::decay_t<decltype(sound.source.selector.box.source.value)>;
    return sound.source.selector.box.source.value == source_type::Folder;
  }

  // The waveform shows the Sound file, or the file picked in the folder.
  // Folder with no folder set: the Sound file, which the processor plays then
  void show_current_sound()
  {
    auto& wf = sound.waveform;
    auto& page = sound.source.pages.folder;
    std::string file = wf.sound_path;
    if(from_folder() && !page.folder.value.empty())
    {
      file.clear();
      const std::string name = !wf.playing_file.empty()
                                   ? wf.playing_file
                                   : wf.folder_files.current().toStdString();
      if(!page.folder.value.empty() && !name.empty())
        file = QDir{QString::fromStdString(page.folder.value)}
                   .filePath(QString::fromStdString(name))
                   .toStdString();
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
      using source_type = std::decay_t<decltype(src.selector.box.source.value)>;
      wf.set_sound = [&src](std::string v) {
        if(auto& c = src.pages.file.file; c.set)
          c.set(std::move(v));
        if(src.selector.box.source.set)
          src.selector.box.source.set(source_type::File);
      };
      wf.set_folder = [&src](std::string v) {
        if(auto& c = src.pages.folder.folder; c.set)
          c.set(std::move(v));
        if(src.selector.box.source.set)
          src.selector.box.source.set(source_type::Folder);
      };
      // The user picks Folder while it is empty: the Sound file's folder, with
      // that file picked, in the same gesture, one undo step. The Sound file's
      // path is the one score located; score stores the folder relative to the
      // document as it stores the folders one picks.
      src.selector.box.source.pick = [&src](source_type v) {
        if(!src.selector.box.source.set)
          return;
        src.selector.box.source.set(v);
        auto& page = src.pages.folder;
        const auto& sound = src.pages.file.file.value;
        if(v == source_type::Folder && page.folder.value.empty() && !sound.empty()
           && page.folder.set && page.sound_file.set)
        {
          const QFileInfo fi(QString::fromStdString(sound));
          page.folder.set(fi.absolutePath().toStdString());
          page.sound_file.set(fi.fileName().toStdString());
        }
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
      // Its name, NUL-terminated by the processor; within the array anyway
      wf.playing_file = cs.index >= 0
                            ? std::string{cs.name, ::strnlen(cs.name, sizeof(cs.name))}
                            : std::string{};
      self.show_current_sound();
      if(wf.update)
        wf.update();
    }
  };
};
}
