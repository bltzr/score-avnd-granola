// The processor: what each control does to the grains and the outputs.
#include "GranolaRig.hpp"

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
    lo = std::min(lo, gr.m_chan_amp[0]);
    hi = std::max(hi, gr.m_chan_amp[0]);
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
