// CPU time of the processor, as a part of real time, for some patches.
//   granola_bench [filter]   runs the scenarios whose name contains filter
#include "GranolaRig.hpp"

#include <chrono>
#include <cstdio>
#include <functional>
#include <string_view>

using granola_test::Rig;

namespace
{
struct Scenario
{
  const char* name;
  double sound_seconds{10.};
  int sound_channels{2};
  int outputs{2};
  std::function<void(Granola::Granola&)> setup;
};

// Best of a few runs: the least disturbed by the rest of the machine
double cpu_percent(const Scenario& s, double audio_seconds = 4.)
{
  double best = 1e300;
  for(int run = 0; run < 5; run++)
  {
    Rig r{s.sound_seconds, s.sound_channels, s.outputs};
    s.setup(*r.g);
    r.run(0.5); // warm up: the voices fill
    const int ticks = int(audio_seconds * Rig::rate / Rig::F);
    const auto t0 = std::chrono::steady_clock::now();
    for(int t = 0; t < ticks; t++)
      (*r.g)({.frames = Rig::F});
    const std::chrono::duration<double> dt = std::chrono::steady_clock::now() - t0;
    best = std::min(best, dt.count());
  }
  return 100. * best / audio_seconds;
}

void dense(Granola::Granola& g)
{
  g.inputs.num_voices.value = 256;
  g.inputs.num_voices.update(g);
  g.inputs.density.value = 256.f;
  g.inputs.dur.value = 0.2f;
}
}

int main(int argc, char** argv)
{
  const std::string_view filter = argc > 1 ? argv[1] : "";
  using mode = decltype(Granola::Granola{}.inputs.window_mode.value);
  using interp = decltype(Granola::Granola{}.inputs.interp_type.value);

  const Scenario scenarios[] = {
      {"idle: not playing", 10., 2, 2,
       [](auto& g) {
    dense(g);
    g.inputs.playing.value = false;
  }},
      {"default: density 20", 10., 2, 2, [](auto& g) { g.inputs.density.value = 20.f; }},
      {"dense: 256 voices, beta, cubic", 10., 2, 2, dense},
      {"dense: kumaraswamy", 10., 2, 2,
       [](auto& g) {
    dense(g);
    g.inputs.window_mode.value = mode::Kuma;
  }},
      {"dense: cos", 10., 2, 2,
       [](auto& g) {
    dense(g);
    g.inputs.window_mode.value = mode::Cos;
  }},
      {"dense: linear", 10., 2, 2,
       [](auto& g) {
    dense(g);
    g.inputs.interp_type.value = interp::Linear;
  }},
      {"dense: wrap at end, scanning", 10., 2, 2,
       [](auto& g) {
    dense(g);
    g.inputs.loopmode.value = true;
    g.inputs.scan.value = 1.f;
    g.inputs.pos.value = 0.9f;
  }},
      {"dense: every deviation", 10., 2, 2,
       [](auto& g) {
    dense(g);
    g.inputs.pos_j.value = 0.5f;
    g.inputs.dur_j.value = 0.5f;
    g.inputs.rate_j.value = 0.3f;
    g.inputs.gain_j.value = 0.5f;
    g.inputs.reverse_chance.value = 0.3f;
  }},
      {"dense: 8 channels", 10., 8, 8, dense},
      {"dense: 100 outputs, spread 1", 10., 1, 100,
       [](auto& g) {
    dense(g);
    g.inputs.out_channels.value = 100;
    g.inputs.spread.value = 1.f;
  }},
      {"short grains: 5 ms, density 256", 10., 2, 2,
       [](auto& g) {
    dense(g);
    g.inputs.dur.value = 0.0005f;
  }},
  };

  std::printf("%-36s %10s\n", "scenario", "% of real time");
  for(const auto& s : scenarios)
  {
    if(!std::string_view{s.name}.contains(filter))
      continue;
    std::printf("%-36s %10.2f\n", s.name, cpu_percent(s));
    std::fflush(stdout);
  }
}
