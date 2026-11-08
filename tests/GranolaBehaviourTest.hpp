#pragma once
// A host for Granola as score is one: prepare(), the controls set, a tick
// at a time, MIDI on the MIDI input, the outputs kept. What the tests read is
// what came out: the output samples, and the grains as they started.
//
// The sounds say where they are read: sample i of a sound of `frames` holds
// tag + 0.5 i / frames, a tag per sound and channel. With a flat window, Gain
// 1 and one grain at a time, an output sample is the sample read, so the
// output alone gives which sound, which channel, where and how fast.
#include <Granola/Granola.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace granola_host
{
using G = Granola::Granola;
using on_midi = std::decay_t<decltype(G{}.inputs.on_midi.value)>;
using order = std::decay_t<decltype(G{}.inputs.order.value)>;
using scan_in = std::decay_t<decltype(G{}.inputs.scan_folder.value)>;
using source_kind = std::decay_t<decltype(G{}.inputs.source.value)>;
using scan_mode = std::decay_t<decltype(G{}.inputs.scan_mode.value)>;
using interp = std::decay_t<decltype(G{}.inputs.interp_type.value)>;
using window_mode = std::decay_t<decltype(G{}.inputs.window_mode.value)>;

inline constexpr int rate = 48000;
// The Sound file's channels are tags 20, 21...; a folder's files 1, 2...
inline constexpr int sound_tag = 20;

//! A sound of \p frames per channel: coded (tag + 0.5 i / frames), or a
//! constant \p dc in every sample
struct Sound
{
  int frames{};
  std::vector<std::vector<float>> data;
  std::vector<const float*> ptrs;

  Sound(int frames, int channels, int first_tag, float dc = 0.f)
      : frames{frames}
      , data(channels, std::vector<float>(frames))
  {
    for(int c = 0; c < channels; c++)
      for(int i = 0; i < frames; i++)
        data[c][i] = dc != 0.f ? dc : float(first_tag + c + 0.5 * double(i) / frames);
    for(auto& d : data)
      ptrs.push_back(d.data());
  }
};

//! A folder of \p n coded mono files, tags 1 to n, listed as a host lists it
struct Folder
{
  std::vector<std::unique_ptr<Sound>> sounds;
  std::vector<std::string> names;
  std::vector<halp::sound_view> views;
  uint64_t generation{};

  explicit Folder(int n, int frames = rate / 10)
  {
    for(int k = 0; k < n; k++)
      add("f" + std::to_string(k) + ".wav", k + 1, frames, (int)views.size());
  }
  //! A file of \p tag at \p at in the listing
  void add(std::string name, int tag, int frames, int at)
  {
    sounds.push_back(std::make_unique<Sound>(frames, 1, tag));
    names.insert(names.begin() + at, std::move(name));
    halp::sound_view v{
        .data = sounds.back()->ptrs.data(),
        .frames = frames,
        .channels = 1,
        .rate = rate,
        .status = halp::sound_status::ready};
    views.insert(views.begin() + at, v);
    for(std::size_t k = 0; k < views.size(); k++)
      views[k].name = names[k];
  }
  //! The folder as listed now, to \p g: as the host does between two ticks
  void list_into(G& g)
  {
    g.inputs.sounds.files = {views, "/folder", ++generation};
    g.inputs.sounds.value = "/folder";
  }
  void status(int k, halp::sound_status s) { views[k].status = s; }
};

//! A grain as it started, seen in the tick it started in
struct Started
{
  long at{};        // the sample it started at
  int voice{};
  int file{-2};     // in the folder; -1 the Sound file
  double amp{};
  Granola::Grain::Pan pan{};
  double pos{};     // where it read when seen first, a part of its sound
  long seen_at{};   // the samples it had played then
  double speed{};   // samples of its sound a sample, from the tick after
};

//! A run of samples of one grain in an output: what it read
struct Segment
{
  long start{};
  long length{};
  int tag{};
  double first{};  // the index read first, in samples of its sound
  double slope{};  // samples of its sound a sample: its speed
  double rel{};    // first as a part of its sound
};

struct Host
{
  std::unique_ptr<G> g = std::make_unique<G>();
  std::unique_ptr<Sound> sound;
  std::vector<Folder*> folders;
  int block{64};
  long now{};

  std::vector<std::vector<double>> out;
  std::vector<double*> out_ptrs;
  std::vector<std::vector<double>> rec; // every output sample since the start
  std::vector<libremidi::message> midi_next;
  std::vector<G::playheads> heads;      // as the panel gets them
  std::vector<long> heads_at;
  std::vector<Started> started;
  int most_active{};
  std::vector<long> last_counter;
  std::vector<long> started_of; // the Started of the grain of each voice

  //! A Sound file of \p seconds and \p channels, coded or of \p dc; Gain 1, a
  //! flat window, Linear interpolation, no Spread: the output is the samples
  //! read
  explicit Host(double seconds = 1., int channels = 1, float dc = 0.f, int prepare_frames = 512)
      : sound{std::make_unique<Sound>(int(seconds * rate), channels, sound_tag, dc)}
  {
    auto& in = g->inputs;
    in.sound.soundfile.data = sound->ptrs.data();
    in.sound.soundfile.frames = sound->frames;
    in.sound.soundfile.channels = channels;
    in.gain.value = 1.f;
    in.win_coefs.value = {0.5f, 0.f};
    in.interp_type.value = interp::Linear;
    in.pos.value = 0.1f;
    in.dur.value = 0.05f;
    in.density.value = 0.5f; // a grain, then as long without one
    // Every grain in every output alike, as many as the channels read
    in.spread.value = 0.f;
    in.out_channels.value = 0;
    set_outputs(channels);
    g->outputs.audio.request_channels = [this](int n) { set_outputs(n); };
    g->send_message = [this](G::processor_to_ui m) {
      if(auto* h = ossia::get_if<G::playheads>(&m.message))
      {
        heads.push_back(*h);
        heads_at.push_back(now);
      }
    };
    // As score does: the controls' update() first, then prepare()
    in.num_voices.update(*g);
    g->prepare({.rate = double(rate), .frames = prepare_frames});
    last_counter.assign(g->grains.size(), -1);
    started_of.assign(g->grains.size(), -1);
  }

  //! Channel \p c of the Sound file as \p f says, sample by sample
  template <typename F>
  void fill(int c, F f)
  {
    for(int i = 0; i < sound->frames; i++)
      sound->data[c][i] = float(f(i));
  }

  void set_outputs(int n)
  {
    if(n > (int)out.size())
    {
      out.resize(n);
      for(auto& c : out)
        c.resize(8192);
      out_ptrs.clear();
      for(auto& c : out)
        out_ptrs.push_back(c.data());
    }
    if(n > (int)rec.size())
      rec.resize(n, std::vector<double>(now, 0.));
    g->outputs.audio.samples = out_ptrs.data();
    g->outputs.audio.channels = n;
  }

  void use(Folder& f, order o = order::Single, std::string picked = "0")
  {
    folders.push_back(&f);
    f.list_into(*g);
    g->inputs.source.value = source_kind::Folder;
    g->inputs.order.value = o;
    g->inputs.sound_file.value = std::move(picked);
  }

  //! The file of the folder whose data is \p data; -1 the Sound file
  int file_of(const float* const* data) const
  {
    if(data == sound->ptrs.data())
      return -1;
    for(auto* f : folders)
      for(std::size_t k = 0; k < f->sounds.size(); k++)
        if(f->sounds[k]->ptrs.data() == data)
          return f->sounds[k]->data[0][0] >= 1.f ? int(f->sounds[k]->data[0][0]) - 1 : -2;
    return -2;
  }
  //! The frames of the sound of \p tag
  int frames_of(int tag) const
  {
    if(tag >= sound_tag)
      return sound->frames;
    for(auto* f : folders)
      for(auto& s : f->sounds)
        if(int(s->data[0][0]) == tag)
          return s->frames;
    return sound->frames;
  }

  void press(int note, int velocity = 127)
  {
    midi_next.push_back(libremidi::message{{0x90, uint8_t(note), uint8_t(velocity)}, 0});
  }
  void release(int note)
  {
    midi_next.push_back(libremidi::message{{0x80, uint8_t(note), 0}, 0});
  }
  void trigger() { g->inputs.trig.value.emplace(); }
  void continuous(bool on)
  {
    g->inputs.playing.value = on;
    g->inputs.playing.update(*g);
  }
  void voices(int n)
  {
    g->inputs.num_voices.value = n;
    g->inputs.num_voices.update(*g);
  }

  void tick(int n = -1)
  {
    if(n < 0)
      n = block;
    for(auto& c : out)
      std::fill_n(c.begin(), n, 0.);
    g->inputs.midi.midi_messages.assign(midi_next.begin(), midi_next.end());
    midi_next.clear();
    (*g)({.frames = n});
    g->inputs.trig.value.reset();
    g->inputs.midi.midi_messages.clear();
    g->inputs.density.values.clear();
    for(std::size_t c = 0; c < rec.size(); c++)
    {
      const bool live = c < (std::size_t)g->outputs.audio.channels;
      for(int s = 0; s < n; s++)
        rec[c].push_back(live ? out[c][s] : 0.);
    }
    now += n;
    // The grains that started in this tick: a grain that goes on has
    // played n more samples
    int active = 0;
    for(std::size_t i = 0; i < g->grains.size(); i++)
    {
      const auto& gr = g->grains[i];
      if(!gr.m_active)
      {
        last_counter[i] = -1;
        continue;
      }
      active++;
      const long c = gr.m_phase_counter;
      if(last_counter[i] < 0 || c != last_counter[i] + n)
      {
        started_of[i] = (long)started.size();
        started.push_back(
            {now - c, int(i), file_of(gr.m_source.data), gr.m_amp, gr.m_pan, gr.position(), c});
      }
      else if(started_of[i] >= 0 && started[started_of[i]].speed == 0.)
      {
        auto& st = started[started_of[i]];
        st.speed = (gr.position() - st.pos) * (gr.m_source.frames - 1.) / double(c - st.seen_at);
      }
      last_counter[i] = c;
    }
    most_active = std::max(most_active, active);
  }
  void run(double seconds)
  {
    const long until = now + long(seconds * rate);
    while(now < until)
      tick(int(std::min<long>(block, until - now)));
  }
  //! Forgets what came out so far
  void forget()
  {
    for(auto& c : rec)
      c.clear();
    rec_from = now;
    started.clear();
    std::ranges::fill(started_of, -1);
    heads.clear();
    heads_at.clear();
    most_active = 0;
  }
  long rec_from{};

  //! The grains read in output \p ch, from what came out: one at a time
  std::vector<Segment> segments(int ch = 0) const
  {
    std::vector<Segment> res;
    const auto& x = rec[ch];
    const auto tag_of = [](double v) { return int(std::floor(v + 1e-9)); };
    const auto index = [&](long s) {
      const int t = tag_of(x[s]);
      return (x[s] - t) * 2. * frames_of(t);
    };
    long s = 0;
    const long n = (long)x.size();
    while(s < n)
    {
      if(x[s] == 0.)
      {
        s++;
        continue;
      }
      const long b = s;
      const int t = tag_of(x[b]);
      while(s < n && x[s] != 0. && tag_of(x[s]) == t)
        s++;
      // Cut by the start or the end of what is kept: not a whole grain
      if(b == 0 || s == n)
        continue;
      Segment seg{.start = rec_from + b, .length = s - b, .tag = t, .first = index(b)};
      const long span = std::max(1L, std::min(seg.length - 2, seg.length * 3 / 4));
      if(seg.length >= 3)
        seg.slope = (index(b + span) - index(b)) / double(span);
      seg.rel = seg.first / double(frames_of(t) - 1);
      res.push_back(seg);
    }
    return res;
  }
  //! The runs of non-zero samples of output \p ch, whole ones: a grain each
  //! when they come one at a time
  std::vector<std::vector<double>> runs(int ch = 0) const
  {
    std::vector<std::vector<double>> res;
    const auto& x = rec[ch];
    std::size_t s = 0;
    while(s < x.size())
    {
      if(x[s] == 0.)
      {
        s++;
        continue;
      }
      const std::size_t b = s;
      while(s < x.size() && x[s] != 0.)
        s++;
      if(b > 0 && s < x.size())
        res.emplace_back(x.begin() + b, x.begin() + s);
    }
    return res;
  }
  //! The level of each grain in output \p ch: the most of its run
  std::vector<double> levels(int ch = 0) const
  {
    std::vector<double> res;
    for(auto& r : runs(ch))
      res.push_back(*std::ranges::max_element(r, {}, [](double v) { return std::abs(v); }));
    return res;
  }
  bool silent() const
  {
    for(auto& c : rec)
      for(double v : c)
        if(v != 0.)
          return false;
    return true;
  }
};

inline double mean(const std::vector<double>& v)
{
  return v.empty() ? 0. : std::accumulate(v.begin(), v.end(), 0.) / double(v.size());
}
inline double stddev(const std::vector<double>& v)
{
  const double m = mean(v);
  double e = 0.;
  for(double x : v)
    e += (x - m) * (x - m);
  return v.size() > 1 ? std::sqrt(e / double(v.size() - 1)) : 0.;
}
//! The files of the folder the grains read, in order, from the output
inline std::vector<int> files_heard(const Host& h)
{
  std::vector<int> res;
  for(auto& s : h.segments())
    res.push_back(s.tag >= sound_tag ? -1 : s.tag - 1);
  return res;
}
}
