// The processor: what each control does to the grains and the outputs.
#include "GranolaRig.hpp"

#include <score/tools/NaturalSort.hpp>

#include <avnd/introspection/input.hpp>
#include <avnd/wrappers/metadatas.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

using granola_test::Rig;
using scan_mode = std::decay_t<decltype(Granola::Granola{}.inputs.scan_mode.value)>;

namespace
{
// Each active grain's read position, once per tick
template <typename F>
void each_grain(Rig& r, int ticks, F&& f)
{
  for(int k = 0; k < ticks; k++)
  {
    r.tick();
    for(auto& gr : r.g->grains)
      if(gr.m_active)
        f(gr);
  }
}

// The read rate of each grain, measured over its first whole tick
std::vector<double> grain_rates(Rig& r, int ticks)
{
  std::vector<double> rates;
  for(int k = 0; k < ticks; k++)
  {
    std::vector<double> before;
    std::vector<long> phase;
    for(auto& gr : r.g->grains)
    {
      before.push_back(gr.m_active ? gr.position() : -1);
      phase.push_back(gr.m_phase_counter);
    }
    r.tick();
    for(std::size_t i = 0; i < r.g->grains.size(); i++)
    {
      auto& gr = r.g->grains[i];
      if(!gr.m_active || before[i] < 0 || gr.m_phase_counter != phase[i] + Rig::F)
        continue;
      if(phase[i] >= Rig::F) // each grain once: slow ones live longer
        continue;
      const double d = gr.position() - before[i];
      if(d != 0. && std::abs(d) < 0.2)
        rates.push_back(d * r.frames / Rig::F);
    }
  }
  return rates;
}

double energy(const std::vector<double>& v)
{
  double e = 0;
  for(double x : v)
    e += x * x;
  return e;
}
}

TEST_CASE("Scan moves Position through the sound, at its speed", "[scan]")
{
  Rig r;
  r.g->inputs.scan.value = 1.f;
  r.run(0.5);
  CHECK(r.g->scan_phase == Catch::Approx(0.5).margin(0.012));

  r.g->inputs.scan.value = 0.f;
  r.run(0.1);
  CHECK(r.g->scan_phase == 0.);

  Rig still;
  still.run(0.5);
  CHECK(!still.scanning);
}

TEST_CASE("Scan modes, at the region's end or where a whole grain fits", "[scan]")
{
  // Position 0.1, Duration 0.05, 1.25 s at the sound's speed
  const std::tuple<scan_mode, bool, double> expect[] = {
      {scan_mode::Loop, false, 0.40},    {scan_mode::PingPong, false, 0.55},
      {scan_mode::Straight, false, 0.95}, {scan_mode::Loop, true, 0.35},
      {scan_mode::PingPong, true, 0.65},  {scan_mode::Straight, true, 1.}};
  for(auto [mode, wrap, want] : expect)
  {
    CAPTURE(int(mode), wrap);
    Rig r;
    r.g->inputs.pos.value = 0.1f;
    r.g->inputs.scan.value = 1.f;
    r.g->inputs.scan_mode.value = mode;
    r.g->inputs.loopmode.value = wrap;
    r.run(1.25);
    CHECK(r.g->scanned(r.g->inputs.pos + r.g->scan_phase)
          == Catch::Approx(want).margin(0.02));
  }
}

TEST_CASE("Reverse Chance: that many grains backwards", "[pitch]")
{
  Rig r;
  r.g->inputs.reverse_chance.value = 0.5f;
  r.g->inputs.density.value = 200.f;
  int back = 0, fwd = 0;
  for(int k = 0; k < 400; k++)
  {
    std::vector<double> before;
    for(auto& gr : r.g->grains)
      before.push_back(gr.m_active ? gr.position() : -1);
    r.tick();
    for(std::size_t i = 0; i < r.g->grains.size(); i++)
    {
      auto& gr = r.g->grains[i];
      if(!gr.m_active || before[i] < 0 || gr.position() == before[i])
        continue;
      (gr.position() < before[i] ? back : fwd)++;
    }
  }
  CHECK(back > 0.35 * (back + fwd));
  CHECK(back < 0.65 * (back + fwd));
}

TEST_CASE("Pitch Deviation in semitones: whole semitones, both ways, forwards", "[pitch]")
{
  Rig r;
  r.g->inputs.rate_j.value = 1.f;
  r.g->inputs.rate_j_semitones.value = true;
  r.g->inputs.density.value = 200.f;
  int up = 0, down = 0, off_grid = 0;
  double lo = 1e9, hi = 0;
  for(double rate : grain_rates(r, 200))
  {
    REQUIRE(rate > 0.);
    (rate > 1. ? up : down)++;
    const double st = 12. * std::log2(rate);
    off_grid += std::abs(st - std::round(st)) > 0.05;
    lo = std::min(lo, rate);
    hi = std::max(hi, rate);
  }
  CHECK(off_grid == 0);
  CHECK(up > 0.3 * (up + down));
  CHECK(up < 0.7 * (up + down));
  CHECK(hi > 1.5);
  CHECK(lo < 0.67);
}

TEST_CASE("Gain Deviation: grains quieter only", "[output]")
{
  Rig r;
  r.g->inputs.gain_j.value = 1.f;
  r.g->inputs.density.value = 200.f;
  double lo = 1e9, hi = 0;
  each_grain(r, 100, [&](auto& gr) {
    lo = std::min(lo, gr.m_amp);
    hi = std::max(hi, gr.m_amp);
  });
  CHECK(lo >= 0.);
  CHECK(hi <= 1.);
  CHECK(hi - lo > 0.3);
}

TEST_CASE("Sample Start and End: no grain outside of the region", "[region]")
{
  for(bool wrap : {false, true})
    for(float scan : {0.f, 1.5f})
    {
      CAPTURE(wrap, scan);
      Rig r;
      r.g->inputs.sample_start.value = 0.6f; // the other way around
      r.g->inputs.sample_end.value = 0.2f;
      r.g->inputs.pos.value = 0.9f;
      r.g->inputs.pos_j.value = 1.f;
      r.g->inputs.dur.value = 0.5f;
      r.g->inputs.dur_j.value = 1.f;
      r.g->inputs.rate_j.value = 1.f;
      r.g->inputs.reverse_chance.value = 0.5f;
      r.g->inputs.density.value = 100.f;
      r.g->inputs.loopmode.value = wrap;
      r.g->inputs.scan.value = scan;
      double lo = 2, hi = -1;
      int n = 0;
      each_grain(r, 300, [&](auto& gr) {
        lo = std::min(lo, gr.position());
        hi = std::max(hi, gr.position());
        n++;
      });
      CHECK(n > 0);
      CHECK(lo >= 0.2 - 1e-4);
      CHECK(hi <= 0.6 + 1e-4);
    }

  Rig r;
  r.g->inputs.sample_start.value = 0.5f;
  r.g->inputs.sample_end.value = 1.f;
  r.g->inputs.scan.value = 1.f;
  r.g->inputs.pos.value = 0.f;
  r.run(0.25);
  CHECK(r.g->scan_phase == Catch::Approx(0.5).margin(0.012));
}

TEST_CASE("Spread among the outputs", "[spread]")
{
  SECTION("0: every output alike")
  {
    for(int outs : {2, 4, 100})
    {
      CAPTURE(outs);
      Rig r{1., 1, outs};
      r.g->inputs.out_channels.value = outs;
      r.run(0.2);
      REQUIRE(r.outputs() == outs);
      for(int c = 1; c < outs; c++)
        CHECK(r.out[c] == r.out[0]);
      CHECK(energy(r.out[0]) > 0.);
    }
  }
  SECTION("1: between two neighbouring outputs; 2: in one")
  {
    for(float sp : {1.f, 2.f})
    {
      CAPTURE(sp);
      constexpr int C = 8;
      Rig r{1., 1, C};
      r.g->inputs.out_channels.value = C;
      r.g->inputs.spread.value = sp;
      r.g->inputs.density.value = 200.f;
      int per[C]{}, in_two = 0, in_one = 0;
      each_grain(r, 400, [&](auto& gr) {
        const auto& p = gr.m_pan;
        REQUIRE(p.a >= 0);
        if(sp == 2.f)
          REQUIRE(p.all == 0.f);
        const bool la = p.at_a > 1e-6f, lb = p.at_b > 1e-6f;
        if(la && lb)
          in_two++;
        else
        {
          in_one++;
          per[la ? p.a : p.a + 1]++;
        }
      });
      if(sp == 1.f)
        CHECK(in_two > 0.8 * (in_two + in_one)); // the ends: at one
      else
      {
        CHECK(in_two == 0);
        const auto [lo, hi] = std::minmax_element(std::begin(per), std::end(per));
        CHECK(*lo > 0.7 * *hi);
      }
    }
  }
  SECTION("1 over a hundred loudspeakers: every one of them")
  {
    constexpr int C = 100;
    Rig r{1., 1, C};
    r.g->inputs.out_channels.value = C;
    r.g->inputs.spread.value = 1.f;
    r.g->inputs.density.value = 200.f;
    r.g->inputs.pos_j.value = 0.5f;
    std::vector<double> e(C);
    for(int k = 0; k < 400; k++)
    {
      r.tick();
      for(int c = 0; c < C; c++)
        e[c] += energy(r.out[c]);
    }
    CHECK(std::ranges::none_of(e, [](double v) { return v == 0.; }));
  }
}

TEST_CASE("Output Channels: as many outputs as asked, more or fewer", "[output]")
{
  Rig r;
  r.g->inputs.out_channels.value = 16;
  r.run(0.05);
  CHECK(r.outputs() == 16);
  r.g->inputs.out_channels.value = 12;
  r.run(0.05);
  CHECK(r.outputs() == 12);
  r.g->inputs.out_channels.value = 0;
  r.run(0.05);
  CHECK(r.outputs() == 1); // a mono sound
}

TEST_CASE("Controls out of their ranges, NaN or infinite: no crash, finite output", "[safety]")
{
  const float bad[] = {std::numeric_limits<float>::quiet_NaN(),
                       std::numeric_limits<float>::infinity(),
                       -std::numeric_limits<float>::infinity(), 1e30f, -1e30f};
  for(float v : bad)
  {
    CAPTURE(v);
    Rig r{1., 12, 2};
    auto& in = r.g->inputs;
    in.pos.value = v;
    in.pos_j.value = v;
    in.dur.value = v;
    in.dur_j.value = v;
    in.rate.value = v;
    in.rate_j.value = v;
    in.density.value = v;
    in.dens_j.value = v;
    in.gain.value = v;
    in.gain_j.value = v;
    in.spread.value = v;
    in.scan.value = v;
    in.reverse_chance.value = v;
    in.sample_start.value = v;
    in.sample_end.value = -v;
    in.win_coefs.value = {v, v};
    in.num_voices.value = 100000;
    in.src_channels.value = 50;
    in.channel_offset.value = 50;
    in.out_channels.value = 100000;
    r.run(0.2);
    for(int c = 0; c < r.outputs(); c++)
      for(double x : r.out[c])
        REQUIRE(std::isfinite(x));
    // Back in range
    CHECK(in.num_voices.value <= (int)r.g->grains.size());
    CHECK(in.out_channels.value <= Granola::Granola::max_outputs);
    CHECK(std::isfinite(in.pos.value));
    CHECK(std::isfinite(in.win_coefs.value.x));
  }
}

TEST_CASE("Grains at the very end of the sound, or of a region at its end", "[safety]")
{
  for(auto interp : {0, 1, 2})
    for(bool wrap : {false, true})
    {
      CAPTURE(interp, wrap);
      Rig r{0.1, 2, 2};
      auto& in = r.g->inputs;
      in.interp_type.value = decltype(in.interp_type.value)(interp);
      in.loopmode.value = wrap;
      in.pos.value = 1.f;
      in.reverse_chance.value = 0.5f;
      in.density.value = 200.f;
      r.run(0.2);
      in.sample_start.value = 1.f;
      in.sample_end.value = 1.f;
      in.pos.value = 0.5f;
      r.run(0.2);
      for(double x : r.out[0])
        REQUIRE(std::isfinite(x));
    }
}

TEST_CASE("Preparing again, e.g. for other outputs, keeps the grains playing", "[safety]")
{
  Rig r;
  r.g->inputs.density.value = 100.f;
  r.run(0.2);
  const auto active = std::ranges::count_if(r.g->grains, [](auto& g) { return g.m_active; });
  REQUIRE(active > 0);
  r.g->prepare({.rate = double(Rig::rate), .frames = Rig::F});
  CHECK(std::ranges::count_if(r.g->grains, [](auto& g) { return g.m_active; }) == active);
}

TEST_CASE("Windows: in [0, 1], peaking at 1, at 0 at both ends", "[window]")
{
  using Granola::window_shape;
  for(int kind : {0, 1, 2})
    for(double x = 0.; x <= 1.; x += 0.125)
      for(double y = 0.; y <= 1.; y += 0.125)
      {
        CAPTURE(kind, x, y);
        const auto w = window_shape::from_pad(kind, x, y);
        double peak = 0.;
        for(int i = 0; i <= 20000; i++)
        {
          const double v = w(i / 20000.);
          REQUIRE(std::isfinite(v));
          REQUIRE(v >= 0.);
          REQUIRE(v <= 1.);
          peak = std::max(peak, v);
        }
        CHECK(peak == Catch::Approx(1.).margin(kind == 1 ? 1e-3 : 1e-6));
        // Rounded at both ends: a and b above 1
        if(w.a > 1.01 && w.b > 1.01)
        {
          CHECK(w(0.) == Catch::Approx(0.).margin(1e-9));
          CHECK(w(1.) == Catch::Approx(0.).margin(1e-9));
        }
      }

  // The default, Beta(3.3, 3.3): a bell, symmetric, its maximum in the middle
  const auto bell = window_shape::from_pad(0, 0.5, 0.05);
  CHECK(bell(0.5) == Catch::Approx(1.));
  CHECK(bell(0.3) == Catch::Approx(bell(0.7)));
  // the exact values, not pow_fast's (0.52 at 0.3)
  CHECK(bell(0.3) == Catch::Approx(std::pow(0.3 * 0.7 / 0.25, bell.a - 1.)).epsilon(1e-9));
}

TEST_CASE("Window tables: the window, closely; grains keep theirs while it changes", "[window]")
{
  using Granola::window_shape;
  Granola::window_table t;
  for(int kind : {0, 1, 2})
    for(double x : {0., 0.3, 0.5, 1.})
      for(double y : {0., 0.05, 0.5, 1.})
      {
        CAPTURE(kind, x, y);
        const auto w = window_shape::from_pad(kind, x, y);
        t.build(w);
        double worst = 0.;
        for(int i = 0; i <= 50000; i++)
          worst = std::max(worst, std::abs(t(i / 50000.) - w(i / 50000.)));
        // A table said to be accurate is: the others are not read
        if(t.accurate)
          CHECK(worst < 2e-3);
        else
          CHECK(kind == 1); // only cosine spikes
      }
  // Most windows are read from tables
  int accurate = 0, all = 0;
  for(int kind : {0, 1, 2})
    for(double x = 0.; x <= 1.; x += 0.1)
      for(double y = 0.; y <= 1.; y += 0.1)
      {
        t.build(window_shape::from_pad(kind, x, y));
        accurate += t.accurate;
        all++;
      }

  CHECK(accurate > 0.9 * all);

  Rig r;
  r.g->inputs.density.value = 100.f;
  r.g->inputs.dur.value = 0.3f;
  r.run(0.1);
  std::vector<const Granola::window_table*> before;
  for(auto& g : r.g->grains)
    if(g.m_active)
      before.push_back(g.m_table);
  REQUIRE(!before.empty());
  const auto first = r.g->grains[0].m_win;
  // Another window: the grains already playing keep reading theirs
  for(int k = 0; k < 10; k++)
  {
    r.g->inputs.win_coefs.value = {0.1f * k, 0.1f};
    r.tick();
    for(auto& g : r.g->grains)
      if(g.m_active && g.m_table)
        REQUIRE(g.m_table->shape == g.m_win);
  }
  CHECK(std::ranges::any_of(r.g->grains, [&](auto& g) { return g.m_active && g.m_win == first; }));
}

namespace
{
libremidi::message note(uint8_t status, int n, int vel)
{
  return libremidi::message{{status, uint8_t(n), uint8_t(vel)}, 0};
}
// Grains made over \p seconds, MIDI \p events sent at the start
uint64_t grains_over(Rig& r, double seconds, std::vector<libremidi::message> events = {})
{
  const auto before = r.g->grains_made;
  for(auto& e : events)
    r.g->inputs.midi.midi_messages.push_back(e);
  r.tick();
  r.g->inputs.midi.midi_messages.clear();
  r.run(seconds - double(Rig::F) / Rig::rate);
  return r.g->grains_made - before;
}
}

TEST_CASE("Density: as many grains as it says, past whole samples", "[timing]")
{
  // Grains of 0.05 s at Density d: d / 0.05 grains a second
  for(float d : {1.f, 20.f, 100.f})
  {
    CAPTURE(d);
    Rig r;
    r.g->inputs.density.value = d;
    r.run(0.2);
    const double per_second = grains_over(r, 2.) / 2.;
    CHECK(per_second == Catch::Approx(d / 0.05).epsilon(0.02));
  }
  // An interval of 1.5 samples: not one grain every 2
  Rig r;
  r.g->inputs.dur.value = 24.f / Rig::rate; // 24 samples
  r.g->inputs.density.value = 16.f;         // one every 1.5 samples
  r.run(0.1);
  CHECK(grains_over(r, 1.) == Catch::Approx(Rig::rate / 1.5).epsilon(0.02));
}

TEST_CASE("MIDI: a grain at once, each note at its own pace, none after its end", "[midi]")
{
  for(int n : {36, 60, 84})
  {
    CAPTURE(n);
    Rig r{4.};
    r.g->inputs.playing.value = false;
    r.g->inputs.density.value = 4.f;
    r.g->inputs.dur.value = 0.01f;
    // The first grain at once: in the first tick its first samples play
    r.g->inputs.midi.midi_messages.push_back(note(0x90, n, 100));
    r.tick();
    r.g->inputs.midi.midi_messages.clear();
    CHECK(r.g->grains_made >= 1);
    CHECK(r.out[0][0] == 0.); // the window starts at 0
    CHECK(std::ranges::any_of(r.out[0], [](double x) { return x != 0.; }));
    // As many grains at a time whatever the note: its grains are as long
    // as its pitch makes them, and come as often
    int active = 0, ticks = 0;
    for(int k = 0; k < 200; k++, ticks++)
    {
      r.tick();
      active += std::ranges::count_if(r.g->grains, [](auto& g) { return g.m_active; });
    }
    CHECK(double(active) / ticks == Catch::Approx(4.).epsilon(0.15));
    // Released: no more grains
    r.g->inputs.midi.midi_messages.push_back(note(0x80, n, 0));
    r.tick();
    r.g->inputs.midi.midi_messages.clear();
    const auto made = r.g->grains_made;
    r.run(0.5);
    CHECK(r.g->grains_made == made);
  }
}

TEST_CASE("Gain follows the control smoothly; fewer voices fade out", "[output]")
{
  Rig r;
  r.g->inputs.density.value = 50.f;
  r.run(0.1);
  r.g->inputs.gain.value = 2.f;
  r.tick();
  CHECK(r.g->gain_now > 0.5);
  CHECK(r.g->gain_now < 2.);
  r.run(0.05);
  CHECK(r.g->gain_now == Catch::Approx(2.).epsilon(1e-3));

  // From 256 voices to 1: the others stop within a few milliseconds, faded
  r.g->inputs.num_voices.value = 1;
  r.g->inputs.num_voices.update(*r.g);
  REQUIRE(std::ranges::count_if(r.g->grains, [](auto& g) { return g.m_active; }) > 1);
  r.tick();
  CHECK(std::ranges::count_if(r.g->grains, [](auto& g) { return g.m_active; }) <= 1);
}

TEST_CASE("Spread: the same power wherever a grain is between two outputs", "[spread]")
{
  Rig r{1., 1, 4};
  r.g->inputs.out_channels.value = 4;
  r.g->inputs.spread.value = 1.f;
  r.g->inputs.density.value = 100.f;
  each_grain(r, 50, [](auto& g) {
    const auto& p = g.m_pan;
    REQUIRE(p.a >= 0);
    CHECK(p.at_a * p.at_a + p.at_b * p.at_b == Catch::Approx(1.f).epsilon(1e-5));
  });
}

TEST_CASE("A Sound file loaded again stops the grains reading the old one", "[safety]")
{
  Rig r;
  r.g->inputs.density.value = 50.f;
  r.run(0.1);
  REQUIRE(std::ranges::any_of(r.g->grains, [](auto& g) { return g.m_active; }));
  // The same file, loaded again: other data
  auto copy = r.sound;
  std::vector<const float*> ptrs;
  for(auto& c : copy)
    ptrs.push_back(c.data());
  r.g->inputs.sound.soundfile.data = ptrs.data();
  r.g->inputs.playing.value = false;
  r.tick();
  for(auto& g : r.g->grains)
    if(g.m_active)
      CHECK(g.m_source.data == ptrs.data());
}

TEST_CASE("The ports, by the names documents and presets find them by", "[ports]")
{
  // score finds a saved port by its name and type: renaming one, or giving it
  // another type, drops its value, cables and automation in saved documents
  const std::vector<std::string_view> expected{
      "Sound", "Position", "Position Deviation", "Scan", "Duration", "Duration Deviation",
      "Spread", "Pitch", "Pitch Deviation", "Pitch Deviation in Semitones", "Reverse",
      "Density", "Density Deviation", "Reverse Chance", "Gain", "Gain Deviation",
      "Window coefs", "Interpolation mode", "Window mode", "Loop",
      "Source Channels", "Channel Offset", "Max Voices", "Continuous", "Trigger",
      "MIDI In", "listening to MIDI", "Source", "Sounds", "Sound in folder", "Random",
      "Scan Across Sounds", "Sample Start", "Sample End", "Output Channels", "Scan Mode"};
  std::vector<std::string_view> names;
  Granola::Granola g;
  avnd::input_introspection<Granola::Granola>::for_all(
      g.inputs, [&]<typename F>(F&) { names.push_back(avnd::get_name<F>()); });
  CHECK(names == expected);
}

TEST_CASE("The panel lists a folder in the host's order, and reads indices as halp", "[ui]")
{
  QStringList names{"b10.wav", "B2.wav", "a.wav", "b1.wav", "b02.wav", "c.wav"};
  score::sortNaturally(names);
  // Equal but for case and leading zeros: by their characters, B before b
  CHECK(names == QStringList{"a.wav", "b1.wav", "B2.wav", "b02.wav", "b10.wav", "c.wav"});

  for(std::string v : {"-1", "3", "3.7", "1e2", "inf", "nan", "x.wav", "", "2e10"})
  {
    CAPTURE(v);
    halp::folder_combobox<"F", "Sounds", "wav"> combo;
    combo.value = v;
    CHECK(combo.index() == halp::folder_index(v));
  }
  CHECK(halp::folder_index("2e10") == std::numeric_limits<int>::max());
  CHECK(halp::folder_index("inf") == std::numeric_limits<int>::max());
  CHECK(!halp::folder_index("-1"));
  CHECK(!halp::folder_index("nan"));
}

TEST_CASE("Max Voices set before prepare(), as score does: a sound dropped plays", "[safety]")
{
  for(int voices : {1, 128, 256})
  {
    CAPTURE(voices);
    auto g = std::make_unique<Granola::Granola>();
    g->inputs.num_voices.value = voices;
    g->inputs.num_voices.update(*g);
    std::vector<double> o0(Rig::F), o1(Rig::F);
    double* outs[2] = {o0.data(), o1.data()};
    g->outputs.audio.samples = outs;
    g->outputs.audio.channels = 2;
    // Before prepare(): silence, no crash
    (*g)({.frames = Rig::F});
    g->prepare({.rate = double(Rig::rate), .frames = Rig::F});
    REQUIRE(g->free_from.size() == g->grains.size());

    // Then a sound is dropped
    std::vector<float> sound(Rig::rate, 0.25f);
    const float* chans[1] = {sound.data()};
    g->inputs.sound.soundfile.data = chans;
    g->inputs.sound.soundfile.frames = Rig::rate;
    g->inputs.sound.soundfile.channels = 1;
    g->inputs.density.value = 50.f;
    for(int k = 0; k < 50; k++)
      (*g)({.frames = Rig::F});
    CHECK(g->grains_made > 0);
  }
}

TEST_CASE("Density Deviation never stops the grains, whatever it draws", "[timing]")
{
  using G = Granola::Granola;
  const float inf = std::numeric_limits<float>::infinity();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  // Any draw: a finite factor within max_density_octaves of 1, as many
  // intervals shorter as longer
  for(float d : {0.f, 0.25f, 1.f})
    for(float z : {-inf, -1e30f, -100.f, -3.f, -1.f, 0.f, 1.f, 3.f, 100.f, 1e30f, inf, nan})
    {
      CAPTURE(d, z);
      const double f = G::density_factor_of(z, d);
      CHECK(std::isfinite(f));
      CHECK(f >= 1. / G::max_density_factor);
      CHECK(f <= G::max_density_factor);
      if(std::isfinite(z) && std::abs(z * d) < 8.f)
        CHECK(f == Catch::Approx(std::exp2(z * d / 2.)));
    }
  CHECK(G::density_factor_of(0.f, 1.f) == 1.);
  CHECK(G::density_factor_of(2.f, 1.f) == Catch::Approx(2.));
  CHECK(G::density_factor_of(-2.f, 1.f) == Catch::Approx(0.5));
  CHECK(G::density_factor_of(nan, 1.f) == 1.);

  // Each grain's draw the lowest there is, or one that no draw gives: the
  // next grains come, at Density / max_density_factor at the slowest.
  // Grains of 0.05 s at Density 20: 400 a second, 25 at the slowest.
  for(double forced : {G::density_factor_of(-inf, 1.f), 0., -1., 1e-300, double(nan)})
  {
    CAPTURE(forced);
    Rig r;
    r.g->inputs.dens_j.value = 1.f;
    r.run(0.05);
    int made = 0;
    for(int t = 0; t < Rig::rate / Rig::F; t++)
    {
      r.g->density_factor = forced;
      const auto before = r.g->grains_made;
      r.tick();
      made += int(r.g->grains_made - before);
    }
    CHECK(made >= 20);
    CHECK(std::isfinite(r.g->trigger_counter));
  }
}

TEST_CASE("Density 0: the grain clock goes on, slower, and back at once", "[timing]")
{
  for(float density : {0.f, -0.f, 1e-30f})
  {
    CAPTURE(density);
    Rig r;
    r.g->inputs.density.value = density;
    r.run(0.5);
    CHECK(std::isfinite(r.g->trigger_counter));
    // As Density comes back, so do the grains: the interval was finite
    r.g->inputs.density.value = 20.f;
    const auto before = r.g->grains_made;
    r.run(0.05);
    CHECK(r.g->grains_made - before >= 15);
  }
  // At the slowest, still a grain every interval / min_density: of 24
  // samples there, a grain about every half second
  Rig r;
  r.g->inputs.dur.value = 24.f / Rig::rate;
  r.g->inputs.density.value = 0.f;
  r.run(0.1);
  const auto before = r.g->grains_made;
  r.run(1.);
  CHECK(r.g->grains_made - before >= 1);
}

TEST_CASE("Density is sample-accurate: its values change the pace where they come", "[timing]")
{
  // Grains of 24 samples: at Density 1, one every 24 samples; at 8, one every
  // 3. Density goes from 1 to 8 at a sample of a tick.
  for(int at : {0, 128, 256, 511})
  {
    CAPTURE(at);
    Rig r;
    r.g->inputs.dur.value = 24.f / Rig::rate;
    r.g->inputs.density.value = 1.f;
    r.run(0.1);
    // As score gives it: the values of the tick, and the last as the value
    r.g->inputs.density.values[at] = 8.f;
    r.g->inputs.density.value = 8.f;
    const auto before = r.g->grains_made;
    r.tick();
    r.g->inputs.density.values.clear();
    const double made = double(r.g->grains_made - before);
    const double expected = at / 24. + (Rig::F - at) / 3.;
    CHECK(made == Catch::Approx(expected).margin(2.));
    // Then the pace the last value says
    const auto next = r.g->grains_made;
    r.tick();
    CHECK(double(r.g->grains_made - next) == Catch::Approx(Rig::F / 3.).margin(2.));
  }
  // Values out of range or not finite, as cables give them: in range
  Rig r;
  for(float v : {std::numeric_limits<float>::quiet_NaN(), -5.f, 1e9f})
  {
    r.g->inputs.density.values[100] = v;
    r.tick();
    r.g->inputs.density.values.clear();
    CHECK(std::isfinite(r.g->density_now));
    CHECK(r.g->density_now >= 0.);
    CHECK(r.g->density_now <= 256.);
  }
}

TEST_CASE("Folder with no folder set plays the Sound file", "[folder]")
{
  using source = std::decay_t<decltype(Granola::Granola{}.inputs.source.value)>;
  Rig r;
  int index = -2;
  r.g->send_message = [&](Granola::Granola::processor_to_ui m) {
    if(auto* c = ossia::get_if<Granola::Granola::current_sound>(&m.message))
      index = c->index;
  };
  r.g->inputs.source.value = source::Folder;
  r.g->inputs.density.value = 50.f;
  r.run(0.2);
  // The Sound file's grains, and the panel told it is the Sound file
  CHECK(r.g->grains_made > 0);
  CHECK(index == -1);
  for(auto& g : r.g->grains)
    if(g.m_active)
      CHECK(g.m_source.data == r.sound_ptrs.data());
  CHECK(std::ranges::any_of(r.out[0], [](double x) { return x != 0.; }));

  // A folder set: its files, none listed here, so nothing plays
  r.g->inputs.sounds.value = "/a/folder/of/sounds";
  r.run(0.2);
  CHECK(std::ranges::all_of(r.out[0], [](double x) { return x == 0.; }));
}

TEST_CASE("The panel in Folder with no folder set shows the Sound file, and edits nothing", "[ui]")
{
  using source = std::decay_t<decltype(Granola::Granola{}.inputs.source.value)>;
  auto u = std::make_unique<Granola::Granola::ui>();
  auto& src = u->sound.source;
  int sets = 0;
  src.source.set = [&](auto) { sets++; };
  src.pages.file.file.set = [&](auto) { sets++; };
  src.pages.folder.folder.set = [&](auto) { sets++; };
  src.pages.folder.sound_file.set = [&](auto) { sets++; };

  src.pages.file.file.value = "/sounds/a.wav";
  src.source.value = source::File;
  u->on_control_update();
  CHECK(u->sound.waveform.waveform.file == "/sounds/a.wav");

  // Switched to Folder, as automation, a cable or the user would: the Sound
  // file still, and no port edited
  src.source.value = source::Folder;
  u->on_control_update();
  CHECK(u->sound.waveform.waveform.file == "/sounds/a.wav");
  CHECK(u->sound.waveform.folder_mode);
  u->stop();
  CHECK(u->sound.waveform.waveform.file == "/sounds/a.wav");

  // A folder set: the file picked in it
  src.pages.folder.folder.value = "/other";
  src.pages.folder.sound_file.value = "b.wav";
  u->sound.waveform.playing_file = "b.wav";
  u->on_control_update();
  CHECK(u->sound.waveform.waveform.file == "/other/b.wav");
  CHECK(sets == 0);
}
