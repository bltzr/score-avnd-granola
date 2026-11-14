// The processor: what each control does to the grains and the outputs.
#include "GranolaRig.hpp"

#include <ossia/math/safe_math.hpp>

#include <avnd/introspection/input.hpp>
#include <avnd/wrappers/metadatas.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
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

TEST_CASE("Reverse Chance: that many grains backwards, all at 1, none at 0", "[pitch]")
{
  for(float chance : {0.f, 0.5f, 1.f})
  {
    CAPTURE(chance);
    Rig r;
    r.g->inputs.reverse_chance.value = chance;
    r.g->inputs.density.value = 200.f;
    int back = 0, fwd = 0;
    for(int k = 0; k < 400; k++)
    {
      std::vector<double> before;
      std::vector<long> phase;
      for(auto& gr : r.g->grains)
      {
        before.push_back(gr.m_active ? gr.position() : -1);
        phase.push_back(gr.m_phase_counter);
      }
      r.tick();
      // The same grain over the whole tick, not another one in its voice
      for(std::size_t i = 0; i < r.g->grains.size(); i++)
      {
        auto& gr = r.g->grains[i];
        if(!gr.m_active || before[i] < 0 || gr.m_phase_counter != phase[i] + Rig::F
           || gr.position() == before[i])
          continue;
        (gr.position() < before[i] ? back : fwd)++;
      }
    }
    REQUIRE(back + fwd > 100);
    if(chance == 0.f)
      CHECK(back == 0);
    else if(chance == 1.f)
      CHECK(fwd == 0);
    else
    {
      CHECK(back > 0.35 * (back + fwd));
      CHECK(back < 0.65 * (back + fwd));
    }
  }
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
        REQUIRE(ossia::safe_isfinite(x));
    // Back in range
    CHECK(in.num_voices.value <= (int)r.g->grains.size());
    CHECK(in.out_channels.value <= Granola::Granola::max_outputs);
    CHECK(ossia::safe_isfinite(in.pos.value));
    CHECK(ossia::safe_isfinite(in.win_coefs.value.x));
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
        REQUIRE(ossia::safe_isfinite(x));
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
          REQUIRE(ossia::safe_isfinite(v));
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
      "Spread", "Pitch", "Pitch Deviation", "Pitch Deviation in Semitones",
      "Density", "Density Deviation", "Reverse Chance", "Gain", "Gain Deviation",
      "Window coefs", "Interpolation mode", "Window mode", "Loop",
      "Source Channels", "Channel Offset", "Max Voices", "Continuous", "Trigger",
      "MIDI In", "On MIDI", "Velocity", "Keep time", "Source", "Sounds", "Sound in folder", "Sound Order",
      "Scan in folder", "Sample Start", "Sample End", "Output Channels", "Scan Mode",
      "Low note", "High note", "Duration Mode", "Duration Time", "Quantize"};
  std::vector<std::string_view> names;
  Granola::Granola g;
  avnd::input_introspection<Granola::Granola>::for_all(
      g.inputs, [&]<typename F>(F&) { names.push_back(avnd::get_name<F>()); });
  CHECK(names == expected);
}

TEST_CASE("The panel reads the indices of Sound in folder as the processor does", "[ui]")
{
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
      CHECK(ossia::safe_isfinite(f));
      CHECK(f >= 1. / G::max_density_factor);
      CHECK(f <= G::max_density_factor);
      if(ossia::safe_isfinite(z) && std::abs(z * d) < 8.f)
        CHECK(f == Catch::Approx(std::exp2(z * d / 2.)));
    }
  CHECK(G::density_factor_of(0.f, 1.f) == 1.);
  CHECK(G::density_factor_of(2.f, 1.f) == Catch::Approx(2.));
  CHECK(G::density_factor_of(-2.f, 1.f) == Catch::Approx(0.5));
  CHECK(G::density_factor_of(nan, 1.f) == 1.);

  // A grain drew the lowest factor there is, or one that no draw gives: the
  // next grain comes within the slowest interval, and then they come as
  // often as Density says. Grains of 0.05 s at Density 20: one every 120
  // samples, every 1920 at the slowest.
  for(double forced : {G::density_factor_of(-inf, 1.f), 0., -1., 1e-300, double(nan)})
  {
    CAPTURE(forced);
    Rig r;
    r.g->inputs.dens_j.value = 1.f;
    r.run(0.05);
    r.g->density_factor = forced;
    const auto before = r.g->grains_made;
    double longest = 0.;
    for(int t = 0; t < Rig::rate / Rig::F; t++)
    {
      r.tick();
      longest = std::max(longest, r.g->trigger_counter);
    }
    CHECK(longest <= 120. * G::max_density_factor + 1.);
    CHECK(r.g->grains_made - before >= 200);
  }
}

TEST_CASE("Density 0 makes no grains; above 0 they come at once", "[timing]")
{
  for(float zero : {0.f, -0.f})
  {
    CAPTURE(zero);
    Rig r;
    r.g->inputs.dur.value = 24.f / Rig::rate;
    r.g->inputs.dens_j.value = 1.f;
    r.run(0.1);
    REQUIRE(r.g->grains_made > 0);
    // Muted by Density 0, as if the last grain had drawn a factor of 0
    r.g->inputs.density.value = zero;
    r.g->density_factor = 0.;
    r.run(0.1);
    while(std::ranges::any_of(r.g->grains, [](auto& g) { return g.m_active; }))
      r.tick();
    const auto muted = r.g->grains_made;
    r.run(1.);
    CHECK(r.g->grains_made == muted);
    CHECK(std::ranges::all_of(r.out[0], [](double x) { return x == 0.; }));

    // Back above 0 at sample 300 of a block, to a grain every 240 samples,
    // the next ones without deviation: one grain, at that sample
    r.g->inputs.dens_j.value = 0.f;
    r.g->inputs.density.values[300] = 0.1f;
    r.g->inputs.density.value = 0.1f;
    r.tick();
    r.g->inputs.density.values.clear();
    CHECK(r.g->grains_made == muted + 1);
    CHECK(std::all_of(&r.out[0][0], &r.out[0][300], [](double x) { return x == 0.; }));
    CHECK(std::any_of(&r.out[0][300], &r.out[0][330], [](double x) { return x != 0.; }));
    // Then as often as Density says, whatever factors come: about 100
    r.g->inputs.dens_j.value = 1.f;
    const auto back = r.g->grains_made;
    r.run(0.5);
    CHECK(r.g->grains_made - back >= 50);
  }
}

TEST_CASE("Density's values reach the next tick when nothing plays", "[timing]")
{
  using source = std::decay_t<decltype(Granola::Granola{}.inputs.source.value)>;
  // Grains of 24 samples: one every 3 samples at Density 8, 12 at 2
  Rig r;
  r.g->inputs.dur.value = 24.f / Rig::rate;
  r.g->inputs.density.value = 1.f;
  r.run(0.1);
  // A tick with nothing to play, e.g. a file of the folder being decoded,
  // in which Density goes to 8
  r.g->inputs.source.value = source::Folder;
  r.g->inputs.sounds.value = "/a/folder/being/listed";
  r.g->inputs.density.values[100] = 8.f;
  r.g->inputs.density.value = 8.f;
  r.tick();
  r.g->inputs.density.values.clear();
  CHECK(r.g->density_now == 8.);
  // The next one plays at 8 until Density goes to 2 at its sample 400
  r.g->inputs.source.value = source::File;
  r.g->inputs.density.values[400] = 2.f;
  r.g->inputs.density.value = 2.f;
  const auto before = r.g->grains_made;
  r.tick();
  r.g->inputs.density.values.clear();
  CHECK(double(r.g->grains_made - before) == Catch::Approx(400. / 3. + 112. / 12.).margin(3.));
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
    CHECK(ossia::safe_isfinite(r.g->density_now));
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
  src.selector.box.source.set = [&](auto) { sets++; };
  src.pages.file.file.set = [&](auto) { sets++; };
  src.pages.folder.folder.set = [&](auto) { sets++; };
  src.pages.folder.sound_file.set = [&](auto) { sets++; };

  src.pages.file.file.value = "/sounds/a.wav";
  src.selector.box.source.value = source::File;
  u->on_control_update();
  CHECK(u->sound.waveform.waveform.file == "/sounds/a.wav");

  // Switched to Folder, as automation, a cable or the user would: the Sound
  // file still, and no port edited
  src.selector.box.source.value = source::Folder;
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

namespace
{
using order = std::decay_t<decltype(Granola::Granola{}.inputs.order.value)>;
using source_kind = std::decay_t<decltype(Granola::Granola{}.inputs.source.value)>;
using scan_in = std::decay_t<decltype(Granola::Granola{}.inputs.scan_folder.value)>;

// A folder of \p n mono files of a second, each of its own, as the host lists
// them
struct Folder
{
  std::vector<std::vector<float>> data;
  std::vector<const float*> ptrs;
  std::vector<halp::sound_view> views;
  std::vector<std::string> names;
  uint64_t generation{};

  explicit Folder(int n, int frames = Rig::rate)
      : data(n, std::vector<float>(frames))
  {
    for(int k = 0; k < n; k++)
      names.push_back("f" + std::to_string(k) + ".wav");
    for(int k = 0; k < n; k++)
      for(int i = 0; i < frames; i++)
        data[k][i] = 0.2f * std::sin(0.01f * (k + 1) * i);
    for(auto& d : data)
      ptrs.push_back(d.data());
    for(int k = 0; k < n; k++)
      views.push_back(
          {.name = names[k],
           .data = &ptrs[k],
           .frames = frames,
           .channels = 1,
           .rate = Rig::rate,
           .status = halp::sound_status::ready});
  }
  void into(Granola::Granola& g, order o)
  {
    g.inputs.sounds.files = {views, "/folder", ++generation};
    g.inputs.sounds.value = "/folder";
    g.inputs.source.value = source_kind::Folder;
    g.inputs.order.value = o;
  }
  void status(int k, halp::sound_status s) { views[k].status = s; }
  int index_of(const float* const* data) const
  {
    for(int k = 0; k < (int)views.size(); k++)
      if(views[k].data == data)
        return k;
    return -1;
  }
};

std::vector<int> sequence(Granola::Granola& g, int count, int picked = 0)
{
  std::vector<int> res;
  for(int k = 0; k < count; k++)
    res.push_back(g.next_in_order(picked));
  return res;
}
}

TEST_CASE("Sound Order: Single file plays the picked file, always", "[order]")
{
  Rig r;
  Folder f{4};
  f.into(*r.g, order::Single);
  CHECK(sequence(*r.g, 5, 2) == std::vector{2, 2, 2, 2, 2});
  // Grains read it
  r.g->inputs.sound_file.value = "2";
  r.g->inputs.density.value = 20.f;
  each_grain(r, 50, [&](auto& g) { CHECK(f.index_of(g.m_source.data) == 2); });
}

TEST_CASE("Sound Order: Alphabetical in the folder's order, around, past unready files", "[order]")
{
  Rig r;
  Folder f{5};
  f.into(*r.g, order::Alphabetical);
  CHECK(sequence(*r.g, 7) == std::vector{0, 1, 2, 3, 4, 0, 1});
  f.status(1, halp::sound_status::loading);
  f.status(3, halp::sound_status::failed);
  f.into(*r.g, order::Alphabetical);
  CHECK(sequence(*r.g, 5) == std::vector{2, 4, 0, 2, 4});
  // None ready: none, and back once they are
  for(int k = 0; k < 5; k++)
    f.status(k, halp::sound_status::loading);
  CHECK(sequence(*r.g, 2) == std::vector{-1, -1});
  f.status(3, halp::sound_status::ready);
  CHECK(sequence(*r.g, 2) == std::vector{3, 3});
}

TEST_CASE("Sound Order: Ping-pong to the last and back, the ends once", "[order]")
{
  Rig r;
  Folder f{5};
  f.into(*r.g, order::PingPong);
  CHECK(sequence(*r.g, 12) == std::vector{0, 1, 2, 3, 4, 3, 2, 1, 0, 1, 2, 3});
  Rig r2;
  Folder f2{5};
  f2.status(2, halp::sound_status::loading);
  f2.into(*r2.g, order::PingPong);
  CHECK(sequence(*r2.g, 8) == std::vector{0, 1, 3, 4, 3, 1, 0, 1});
  Rig r1;
  Folder one{1};
  one.into(*r1.g, order::PingPong);
  CHECK(sequence(*r1.g, 3) == std::vector{0, 0, 0});
}

TEST_CASE("Sound Order: Shuffle plays every file once a cycle, none twice in a row", "[order]")
{
  for(int n : {2, 6, 37, 5000})
  {
    CAPTURE(n);
    Rig r;
    Folder f{n, 8};
    f.into(*r.g, order::Shuffle);
    const int cycles = n > 100 ? 2 : 60;
    const auto seq = sequence(*r.g, n * cycles);
    for(int c = 0; c < cycles; c++)
    {
      std::vector<int> cycle(seq.begin() + c * n, seq.begin() + (c + 1) * n);
      std::ranges::sort(cycle);
      for(int k = 0; k < n; k++)
        REQUIRE(cycle[k] == k);
    }
    for(std::size_t k = 1; k < seq.size(); k++)
      REQUIRE(seq[k] != seq[k - 1]);
    // Not always the same order
    if(n == 6)
      CHECK(!std::equal(seq.begin(), seq.begin() + n, seq.begin() + n));
  }
  // Unready files skipped, the others still once a cycle
  Rig r;
  Folder f{6};
  f.status(4, halp::sound_status::failed);
  f.into(*r.g, order::Shuffle);
  auto seq = sequence(*r.g, 5);
  std::ranges::sort(seq);
  CHECK(seq == std::vector{0, 1, 2, 3, 5});
}

TEST_CASE("Sound Order: Random covers the folder evenly", "[order]")
{
  Rig r;
  Folder f{6};
  f.into(*r.g, order::Random);
  std::array<int, 6> count{};
  for(int i : sequence(*r.g, 6000))
    count[i]++;
  for(int c : count)
    CHECK(c == Catch::Approx(1000).epsilon(0.15));
}

TEST_CASE("Sound Order: Random walk steps to neighbours, back from the ends", "[order]")
{
  Rig r;
  Folder f{10};
  f.into(*r.g, order::RandomWalk);
  const auto seq = sequence(*r.g, 4000, 5);
  std::array<int, 10> visits{};
  int ones = 0, twos = 0, prev = 5;
  for(int i : seq)
  {
    REQUIRE(i >= 0);
    REQUIRE(i < 10);
    const int d = std::abs(i - prev);
    REQUIRE((d == 1 || d == 2));
    (d == 1 ? ones : twos)++;
    visits[i]++;
    prev = i;
  }
  CHECK(double(ones) / seq.size() == Catch::Approx(0.8).margin(0.04));
  for(int v : visits)
    CHECK(v > 100);
  // An unready file: the nearest ready one instead
  f.status(6, halp::sound_status::loading);
  f.into(*r.g, order::RandomWalk);
  for(int i : sequence(*r.g, 500))
    REQUIRE(i != 6);
}

namespace
{
// The files the grains made in \p ticks read, and their speeds
struct heard
{
  std::set<int> files;
  std::vector<double> rates;
};
heard listen(Rig& r, Folder& f, int ticks)
{
  heard h;
  each_grain(r, ticks, [&](auto& gr) { h.files.insert(f.index_of(gr.m_source.data)); });
  h.rates = grain_rates(r, 20);
  return h;
}
using on_midi = std::decay_t<decltype(Granola::Granola{}.inputs.on_midi.value)>;
// A rig with a folder of 4 files, file 0 picked, On MIDI and By MIDI note as
// asked
std::unique_ptr<Rig> midi_rig(Folder& f, on_midi mode, bool by_note, bool continuous)
{
  auto r = std::make_unique<Rig>(1.);
  f.into(*r->g, by_note ? order::ByNote : order::Single);
  auto& in = r->g->inputs;
  in.sound_file.value = "0";
  in.on_midi.value = mode;
  in.playing.value = continuous;
  in.density.value = 4.f;
  in.dur.value = 0.02f;
  // A key a file from note 60
  in.low_note.value = 60;
  in.high_note.value = 60 + f.views.size() - 1;
  return r;
}
void press(Rig& r, int n, bool on)
{
  r.g->inputs.midi.midi_messages.push_back(note(on ? 0x90 : 0x80, n, on ? 100 : 0));
  r.tick();
  r.g->inputs.midi.midi_messages.clear();
  // The grains of before end
  r.run(0.1);
}
bool all_near(const std::vector<double>& v, double want)
{
  return !v.empty()
         && std::ranges::all_of(v, [=](double x) { return std::abs(x - want) < 0.01; });
}
}

TEST_CASE("On MIDI, with and without By MIDI note", "[midi][order]")
{
  const double up2 = std::exp2(2. / 12.);
  for(bool by_note : {false, true})
  {
    CAPTURE(by_note);
    // Note 62: file 2 By MIDI note, else the picked file, 0
    const std::set<int> its_file{by_note ? 2 : 0};
    // Nothing: no grain of the notes, no pitch
    {
      INFO("Nothing: no grain of the notes, no pitch");
      Folder f{4};
      auto r = midi_rig(f, on_midi::Nothing, by_note, true);
      press(*r, 62, true);
      const auto h = listen(*r, f, 40);
      CHECK(h.files == its_file);
      CHECK(all_near(h.rates, 1.));
      r->g->inputs.playing.value = false;
      r->run(0.2);
      const auto made = r->g->grains_made;
      press(*r, 64, true);
      r->run(0.3);
      CHECK(r->g->grains_made == made);
    }
    // Trigger: grains while held, at Pitch
    {
      INFO("Trigger: grains while held, at Pitch");
      Folder f{4};
      auto r = midi_rig(f, on_midi::Trigger, by_note, false);
      r->run(0.2);
      CHECK(r->g->grains_made == 0);
      press(*r, 62, true);
      const auto h = listen(*r, f, 40);
      CHECK(h.files == its_file);
      CHECK(all_near(h.rates, 1.));
      press(*r, 62, false);
      const auto made = r->g->grains_made;
      r->run(0.3);
      CHECK(r->g->grains_made == made);
    }
    // Trigger and set pitch: grains while held, at the note's pitch
    {
      INFO("Trigger and set pitch: grains while held, at the note's pitch");
      Folder f{4};
      auto r = midi_rig(f, on_midi::TriggerPitch, by_note, false);
      press(*r, 62, true);
      const auto h = listen(*r, f, 40);
      CHECK(h.files == its_file);
      CHECK(all_near(h.rates, up2));
      press(*r, 62, false);
      const auto made = r->g->grains_made;
      r->run(0.3);
      CHECK(r->g->grains_made == made);
    }
    // Set pitch: Continuous's grains at the pitch of the note pressed last
    {
      INFO("Set pitch: Continuous's grains at the pitch of the note pressed last");
      Folder f{4};
      auto r = midi_rig(f, on_midi::SetPitch, by_note, true);
      CHECK(all_near(listen(*r, f, 20).rates, 1.));
      press(*r, 62, true);
      auto h = listen(*r, f, 40);
      CHECK(h.files == its_file);
      CHECK(all_near(h.rates, up2));
      press(*r, 67, true);
      CHECK(all_near(listen(*r, f, 20).rates, std::exp2(7. / 12.)));
      // Released: the one held before, then, none held, the last one still
      press(*r, 67, false);
      CHECK(all_near(listen(*r, f, 20).rates, up2));
      press(*r, 62, false);
      h = listen(*r, f, 40);
      CHECK(h.files == its_file);
      CHECK(all_near(h.rates, up2));
      // No grain of the notes themselves
      r->g->inputs.playing.value = false;
      r->run(0.2);
      const auto made = r->g->grains_made;
      press(*r, 62, true);
      r->run(0.3);
      CHECK(r->g->grains_made == made);
    }
  }
}

TEST_CASE("By MIDI note and Set pitch: the note goes on once released, until reset", "[midi][order]")
{
  Folder f{4};
  auto r = midi_rig(f, on_midi::SetPitch, true, true);
  // Note 50, below the keys of the files: the first file, a minor seventh
  // down
  const double down10 = std::exp2(-10. / 12.);
  press(*r, 50, true);
  press(*r, 50, false);
  auto h = listen(*r, f, 40);
  CHECK(h.files == std::set{0});
  CHECK(all_near(h.rates, down10));
  // Another note replaces it
  press(*r, 61, true);
  press(*r, 61, false);
  h = listen(*r, f, 40);
  CHECK(h.files == std::set{1});
  CHECK(all_near(h.rates, std::exp2(1. / 12.)));
  // Stopping and starting keep it
  r->g->inputs.playing.value = false;
  r->run(0.2);
  r->g->inputs.playing.value = true;
  r->g->inputs.playing.update(*r->g);
  CHECK(listen(*r, f, 40).files == std::set{1});
  // A file picked by hand ends it
  r->g->inputs.sound_file.value = "3";
  r->run(0.1); // the grains of before end
  h = listen(*r, f, 40);
  CHECK(h.files == std::set{3});
  CHECK(all_near(h.rates, 1.));
  // As does another Sound Order
  press(*r, 62, true);
  press(*r, 62, false);
  CHECK(listen(*r, f, 40).files == std::set{2});
  r->g->inputs.order.value = order::Single;
  r->tick();
  r->g->inputs.order.value = order::ByNote;
  r->run(0.1);
  h = listen(*r, f, 40);
  CHECK(h.files == std::set{3});
  CHECK(all_near(h.rates, 1.));

  // The notes playing their own grains: their own file while they play
  Folder g{4};
  auto t = midi_rig(g, on_midi::TriggerPitch, true, false);
  press(*t, 61, true);
  CHECK(listen(*t, g, 40).files == std::set{1});
  press(*t, 61, false);
  const auto made = t->g->grains_made;
  t->run(0.3);
  CHECK(t->g->grains_made == made);
}

TEST_CASE("On MIDI Trigger: notes 48 and 72 read at Pitch, not transposed", "[midi]")
{
  for(int n : {48, 72})
  {
    CAPTURE(n);
    Folder f{1};
    auto r = midi_rig(f, on_midi::Trigger, false, false);
    r->g->inputs.rate.value = 1.5f;
    press(*r, n, true);
    const auto rates = grain_rates(*r, 30);
    REQUIRE(!rates.empty());
    for(double v : rates)
      CHECK(v == Catch::Approx(1.5).margin(0.01));
  }
}

TEST_CASE("Velocity: how much a note's velocity sets the level of its grains", "[midi][velocity]")
{
  const auto level = [](float amount, int vel, on_midi mode, bool release) {
    Folder f{1};
    auto r = midi_rig(f, mode, false, mode == on_midi::SetPitch);
    r->g->inputs.velocity.value = amount;
    r->g->inputs.midi.midi_messages.push_back(note(0x90, 60, vel));
    r->tick();
    r->g->inputs.midi.midi_messages.clear();
    if(release)
      press(*r, 60, false);
    r->run(0.1);
    std::set<double> amps;
    each_grain(*r, 20, [&](auto& g) { amps.insert(std::round(g.m_amp * 1e6) / 1e6); });
    return amps;
  };
  const double c64 = (64. / 127.) * (64. / 127.);
  // At 0, velocity ignored; at 1, squared; between, as much of it
  CHECK(level(0.f, 64, on_midi::Trigger, false) == std::set{1.});
  CHECK(level(1.f, 127, on_midi::Trigger, false) == std::set{1.});
  CHECK(level(1.f, 64, on_midi::Trigger, false) == std::set{std::round(c64 * 1e6) / 1e6});
  CHECK(
      level(0.5f, 64, on_midi::TriggerPitch, false)
      == std::set{std::round((1. - 0.5 * (1. - c64)) * 1e6) / 1e6});
  // Set pitch: the grains of Continuous at the velocity of the note, once
  // released too
  CHECK(level(1.f, 64, on_midi::SetPitch, true) == std::set{std::round(c64 * 1e6) / 1e6});
}

namespace
{
// The samples a single grain sounds for, Triggered once, in the output
int grain_length(float pitch, bool keep_time, bool backwards = false, float pos = 0.1f,
                 float dur = 0.05f, bool wrap = false)
{
  Rig r{1.};
  auto& in = r.g->inputs;
  in.playing.value = false;
  in.rate.value = pitch;
  in.keep_time.value = keep_time;
  in.reverse_chance.value = backwards ? 1.f : 0.f;
  in.pos.value = pos;
  in.dur.value = dur;
  in.loopmode.value = wrap;
  in.trig.value.emplace();
  int first = -1, last = -1;
  long at = 0;
  for(int t = 0; t < 4 * Rig::rate / Rig::F; t++, at += Rig::F)
  {
    r.tick();
    in.trig.value.reset();
    for(int s = 0; s < Rig::F; s++)
      if(r.out[0][s] != 0.)
      {
        if(first < 0)
          first = int(at + s);
        last = int(at + s);
      }
  }
  return first < 0 ? 0 : last - first + 1;
}
}

TEST_CASE("Keep time: a grain lasts as long at any pitch", "[keep-time]")
{
  // 0.05 of a second-long sound: 2400 samples at speed 1
  for(float pitch : {0.5f, 1.f, 2.f})
  {
    CAPTURE(pitch);
    CHECK(grain_length(pitch, true) == Catch::Approx(2400).margin(4));
    CHECK(grain_length(pitch, true, true) == Catch::Approx(2400).margin(4));
    CHECK(grain_length(pitch, false) == Catch::Approx(2400 / pitch).margin(4));
  }
  // At a pitch where it would read past the region: it ends at its end;
  // with Wrap at end, it reads the region once at most
  const int fast = grain_length(8.f, true, false, 0.8f, 0.1f);
  CHECK(fast == Catch::Approx(0.2 * Rig::rate / 8.).margin(4));
  const int wrapped = grain_length(8.f, true, false, 0.8f, 0.5f, true);
  CHECK(wrapped == Catch::Approx(Rig::rate / 8.).margin(4));
}

TEST_CASE("Keep time: the grains keep their pace whatever their pitch", "[keep-time]")
{
  // Continuous: Density 20 of grains 0.05 of a 2 s sound, 200 a second, at
  // any pitch
  for(float pitch : {0.5f, 1.f, 2.f})
  {
    CAPTURE(pitch);
    Rig r{2.};
    r.g->inputs.keep_time.value = true;
    r.g->inputs.rate.value = pitch;
    r.run(0.2);
    CHECK(grains_over(r, 1.) == Catch::Approx(200.).epsilon(0.02));
  }
  // Notes 48 and 72 held: their grains start at the same instants (one a
  // sample at most: a sample apart)
  for(bool keep : {true, false})
  {
    CAPTURE(keep);
    Folder f{1};
    auto r = midi_rig(f, on_midi::TriggerPitch, false, false);
    r->g->inputs.keep_time.value = keep;
    r->g->inputs.density.value = 1.f;
    r->g->inputs.midi.midi_messages.push_back(note(0x90, 48, 127));
    r->g->inputs.midi.midi_messages.push_back(note(0x90, 72, 127));
    std::vector<long> starts;
    long at = 0;
    std::vector<long> before(r->g->grains.size(), -1);
    for(int t = 0; t < Rig::rate / Rig::F; t++, at += Rig::F)
    {
      r->tick();
      r->g->inputs.midi.midi_messages.clear();
      for(std::size_t i = 0; i < r->g->grains.size(); i++)
      {
        auto& g = r->g->grains[i];
        const long pc = g.m_active ? long(g.m_phase_counter) : -1;
        if(pc >= 0 && (before[i] < 0 || pc < before[i]))
          starts.push_back(at + Rig::F - pc);
        before[i] = pc;
      }
    }
    std::ranges::sort(starts);
    REQUIRE(starts.size() >= 8);
    bool paired = starts.size() % 2 == 0;
    for(std::size_t k = 0; paired && k + 1 < starts.size(); k += 2)
      paired = starts[k + 1] - starts[k] <= 1;
    CHECK(paired == keep);
  }
}

TEST_CASE("Sound Order: the grains read the files in order", "[order]")
{
  // One grain at a time: 2400 samples apart, longer than a tick
  Rig r;
  Folder f{3};
  f.into(*r.g, order::Alphabetical);
  r.g->inputs.density.value = 1.f;
  std::vector<int> files;
  for(int t = 0; t < 200 && files.size() < 7; t++)
  {
    const auto made = r.g->grains_made;
    r.tick();
    if(r.g->grains_made == made)
      continue;
    REQUIRE(r.g->grains_made == made + 1);
    const Granola::Grain* newest{};
    for(auto& g : r.g->grains)
      if(g.m_active && (!newest || g.m_phase_counter < newest->m_phase_counter))
        newest = &g;
    REQUIRE(newest);
    files.push_back(f.index_of(newest->m_source.data));
  }
  CHECK(files == std::vector{0, 1, 2, 0, 1, 2, 0});
}

TEST_CASE("Scan's play zone moves on whichever files of the folder the grains play", "[scan][folder]")
{
  using G = Granola::Granola;
  // Few grains on the file shown among many: the play zone the panel draws
  // still follows Scan, without standing still then jumping
  for(auto o : {order::Random, order::Alphabetical, order::Shuffle, order::RandomWalk})
    for(auto mode : {scan_in::Restart, scan_in::Continue})
    {
      CAPTURE(int(o), int(mode));
      Rig r;
      Folder f{40};
      f.into(*r.g, o);
      r.g->inputs.density.value = 2.f;
      r.g->inputs.scan.value = 1.f;
      r.g->inputs.scan_folder.value = mode;
      std::vector<std::pair<long, int>> zone; // when, where
      long now = 0;
      r.g->send_message = [&](G::processor_to_ui m) {
        if(auto* h = ossia::get_if<G::playheads>(&m.message); h && h->scanning)
          zone.emplace_back(now, h->scan);
      };
      for(int t = 0; t < 2 * Rig::rate / Rig::F; t++, now += Rig::F)
        r.tick();
      // A message a 30th of a second at most, at the end of a tick
      REQUIRE(zone.size() > 40);
      // Each as far from the last as Scan went meanwhile: through the file in
      // a second, around at the last place a grain fits
      const double around = r.g->scan_last() * G::playheads::pos_scale;
      for(std::size_t k = 1; k < zone.size(); k++)
      {
        const double expected
            = double(zone[k].first - zone[k - 1].first) / Rig::rate * G::playheads::pos_scale;
        const double d = zone[k].second - zone[k - 1].second;
        const bool step = std::abs(d - expected) < 200. || std::abs(d + around - expected) < 200.;
        CAPTURE(k, d, expected);
        REQUIRE(step);
      }
    }
}

TEST_CASE("Scan in folder: Restart, Continue, or each file's own place", "[scan][folder]")
{
  for(auto mode : {scan_in::Restart, scan_in::Continue, scan_in::PerFile})
  {
    CAPTURE(int(mode));
    Rig r;
    Folder f{3, Rig::rate / 2};
    f.into(*r.g, order::Single);
    r.g->inputs.pos.value = 0.1f;
    r.g->inputs.scan.value = 1.f;
    r.g->inputs.density.value = 20.f;
    r.g->inputs.scan_folder.value = mode;
    r.g->inputs.sound_file.value = "0";
    r.run(0.2);
    const double before = r.g->scan_phase;
    REQUIRE(before > 0.3);
    r.g->inputs.sound_file.value = "2";
    r.tick();
    switch(mode)
    {
      case scan_in::Restart:
        CHECK(r.g->scan_phase < 0.05);
        break;
      case scan_in::Continue:
        CHECK(r.g->scan_phase > before);
        break;
      case scan_in::PerFile: {
        // File 0 where its grains left it, file 2 from Position
        const double at0 = r.g->file_scan[0];
        CHECK(at0 == Catch::Approx(before).margin(0.03));
        CHECK(r.g->file_scan[2] < 0.05);
        r.run(0.1);
        CHECK(r.g->file_scan[0] == at0);
        CHECK(r.g->file_scan[2] == Catch::Approx(0.2).margin(0.04));
        // File 0 again: on from where it was
        r.g->inputs.sound_file.value = "0";
        r.tick();
        CHECK(r.g->file_scan[0] == Catch::Approx(at0).margin(0.04));
        break;
      }
    }
  }
}

TEST_CASE("Scan in folder, Per file: a file's place moves only with its grains", "[scan][folder]")
{
  // 4 files in turn: each moves a quarter of the way Scan goes
  Rig r;
  Folder f{4};
  f.into(*r.g, order::Alphabetical);
  r.g->inputs.scan.value = 0.1f;
  r.g->inputs.density.value = 20.f;
  r.g->inputs.scan_folder.value = scan_in::PerFile;
  r.run(2.);
  double sum = 0.;
  for(int k = 0; k < 4; k++)
  {
    CAPTURE(k);
    CHECK(r.g->file_scan[k] == Catch::Approx(r.g->scan_travel / 4.).margin(0.01));
    sum += r.g->file_scan[k];
  }
  CHECK(sum == Catch::Approx(r.g->scan_travel_at_grain).epsilon(1e-9));

  // A file not ready: its place stays
  Rig u;
  Folder g{4};
  g.status(1, halp::sound_status::loading);
  g.into(*u.g, order::Alphabetical);
  u.g->inputs.scan.value = 0.1f;
  u.g->inputs.density.value = 20.f;
  u.g->inputs.scan_folder.value = scan_in::PerFile;
  u.run(1.);
  CHECK(u.g->file_scan[1] == 0.);
  CHECK(u.g->file_scan[0] > 0.01);

  // The folder listed again with a file before the others: the places follow
  // the files, by name
  const std::vector<double> places(r.g->file_scan.begin(), r.g->file_scan.begin() + 4);
  Folder more{5};
  for(int k = 0; k < 4; k++)
    more.names[k + 1] = f.names[k];
  more.names[0] = "a new one.wav";
  for(int k = 0; k < 5; k++)
    more.views[k].name = more.names[k];
  r.g->inputs.sounds.files = {more.views, "/folder", 1000};
  r.g->relist_file_scans();
  CHECK(r.g->file_scan[0] == 0.);
  for(int k = 0; k < 4; k++)
    CHECK(r.g->file_scan[k + 1] == places[k]);
}

TEST_CASE("Scan in folder, Per file: the play zone shows the shown file's place", "[scan][folder]")
{
  using G = Granola::Granola;
  Rig r;
  Folder f{4};
  f.into(*r.g, order::Alphabetical);
  r.g->inputs.scan.value = 0.1f;
  r.g->inputs.density.value = 20.f;
  r.g->inputs.scan_folder.value = scan_in::PerFile;
  r.g->inputs.sound_file.value = "1";
  int shown = -1;
  r.g->send_message = [&](G::processor_to_ui m) {
    if(auto* h = ossia::get_if<G::playheads>(&m.message); h && h->scanning)
      shown = h->scan;
  };
  r.run(1.);
  REQUIRE(shown >= 0);
  CHECK(
      shown / G::playheads::pos_scale
      == Catch::Approx(r.g->scanned(r.g->inputs.pos + r.g->file_scan[1])).margin(0.01));
}
