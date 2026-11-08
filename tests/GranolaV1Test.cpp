// Granola (v1): the object that documents made with the first Granola name.
// Its ports, and the samples it makes from them, are those of then.
#include <Granola/Granola.hpp>
#include <Granola/v1/Granola.hpp>

#include <avnd/introspection/input.hpp>
#include <avnd/wrappers/metadatas.hpp>

#include <catch2/catch_all.hpp>

#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <string_view>
#include <vector>

namespace
{
using V1 = GranolaV1::Granola;

template <typename T>
std::vector<std::string_view> port_names()
{
  std::vector<std::string_view> names;
  T g;
  avnd::input_introspection<T>::for_all(
      g.inputs, [&]<typename F>(F&) { names.push_back(avnd::get_name<F>()); });
  return names;
}

// A sine per channel, a second long, and two outputs, as score runs it
struct RigV1
{
  static constexpr int rate = 48000;
  static constexpr int F = 512;
  std::unique_ptr<V1> g = std::make_unique<V1>();
  std::vector<std::vector<float>> sound;
  std::vector<const float*> sound_ptrs;
  std::vector<std::vector<double>> out{2, std::vector<double>(F)};
  double* out_ptrs[2]{out[0].data(), out[1].data()};

  explicit RigV1(int channels = 1)
      : sound(channels, std::vector<float>(rate))
  {
    for(int c = 0; c < channels; c++)
      for(int i = 0; i < rate; i++)
        sound[c][i] = 0.3f * std::sin(2 * std::numbers::pi * 220. * (c + 1) * i / 48000.);
    for(auto& c : sound)
      sound_ptrs.push_back(c.data());
    g->inputs.sound.soundfile.data = sound_ptrs.data();
    g->inputs.sound.soundfile.frames = rate;
    g->inputs.sound.soundfile.channels = channels;
    g->outputs.audio.samples = out_ptrs;
    g->outputs.audio.channels = 2;
  }
  void start()
  {
    g->prepare({.rate = double(rate), .frames = F});
    g->inputs.playing.update(*g);
  }
  void tick() { (*g)({.frames = F}); }
};
}

TEST_CASE("Granola (v1): the uuid and the ports of the Granola released first", "[v1]")
{
  // The uuid documents name it by; the new Granola is another object
  CHECK(std::string_view{V1::uuid()} == "38F9684D-54A6-4F48-91E4-3B251F0956EA");
  CHECK(std::string_view{Granola::Granola::uuid()} != std::string_view{V1::uuid()});
  CHECK(std::string_view{V1::c_name()} == "granola");
  CHECK(avnd::tag_deprecated<V1>);

  // score finds a saved port by its name and type: these are the names of
  // then, Jitter and Jitter Range alike
  const std::vector<std::string_view> released{
      "Sound",
      "Position",
      "Position Jitter",
      "Position Jitter Range",
      "Duration",
      "Duration Jitter",
      "Duration Jitter Range",
      "Pitch",
      "Pitch Jitter",
      "Pitch Jitter Range",
      "Reverse",
      "Density",
      "Density Jitter",
      "Density Jitter Range",
      "Gain",
      "Gain Jitter",
      "Gain Jitter Range",
      "Window coefs",
      "Interpolation mode",
      "Window mode",
      "Loop",
      "Source Channels",
      "Channel Offset",
      "Max Voices",
      "Continuous",
      "Trigger",
      "MIDI In",
      "listening to MIDI"};
  CHECK(port_names<V1>() == released);

  // and their defaults
  V1 g;
  CHECK(g.inputs.pos_j_r.value == 1.f);
  CHECK(g.inputs.dur_j_r.value == 1.f);
  CHECK(g.inputs.rate_j_r.value == 1.f);
  CHECK(g.inputs.dens_j_r.value == 1.f);
  CHECK(g.inputs.gain_j_r.value == 1.f);
}

TEST_CASE("Granola (v1): a Jitter spreads its control by N(0, range / 4)", "[v1]")
{
  for(float range : {0.f, 0.2f, 0.8f})
  {
    CAPTURE(range);
    RigV1 r;
    auto& in = r.g->inputs;
    in.pos.value = 0.5f;
    in.pos_j.value = 1.f;
    in.pos_j_r.value = range;
    in.dur.value = 0.01f;
    in.density.value = 4.f;
    r.start();
    std::vector<double> starts;
    std::vector<bool> seen(r.g->grains.size());
    for(int t = 0; t < 400; t++)
    {
      r.tick();
      for(std::size_t i = 0; i < r.g->grains.size(); i++)
      {
        auto& gr = r.g->grains[i];
        // Each grain once, when it starts
        if(gr.m_active && !seen[i])
          starts.push_back(gr.m_startpoint / gr.m_buf_len);
        seen[i] = gr.m_active;
      }
    }
    REQUIRE(starts.size() > 100);
    double mean = 0, var = 0;
    for(double s : starts)
      mean += s;
    mean /= starts.size();
    for(double s : starts)
      var += (s - mean) * (s - mean);
    const double sd = std::sqrt(var / starts.size());
    CHECK(mean == Catch::Approx(0.5).margin(0.03));
    CHECK(sd == Catch::Approx(range / 4.).margin(0.015));
  }
}

TEST_CASE("Granola (v1): NaN or infinite controls, as cables give them: no crash", "[v1]")
{
  const float bad[]{
      std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity()};
  for(int which = 0; which < 8; which++)
    for(float b : bad)
    {
      CAPTURE(which, b);
      RigV1 r;
      auto& in = r.g->inputs;
      in.pos_j.value = 1.f;
      in.dur_j.value = 1.f;
      in.rate_j.value = 1.f;
      float* f[]{&in.pos.value,     &in.dur.value,     &in.rate.value,
                 &in.density.value, &in.pos_j_r.value, &in.dur_j_r.value,
                 &in.rate_j_r.value, &in.dur_j.value};
      *f[which] = b;
      r.start();
      for(int t = 0; t < 40; t++)
        r.tick();
      // The grains it made read the sound, whatever their level
      for(auto& g : r.g->grains)
        if(g.m_active)
          CHECK(g.m_max_count < 10 * RigV1::rate);
    }
}

#if defined(__GLIBCXX__)
// The samples of the Granola released first, made by its own code (origin/main
// at a45445a) on the same patch: the normal distributions are those of
// libstdc++, whence the condition
TEST_CASE("Granola (v1): the samples of the Granola released first", "[v1]")
{
  struct expected
  {
    int mode, interp, channels;
    double energy, weighted;
  };
  const expected golden[]{
      {0, 0, 1, 8729.1459389686443, 159.93146109902466},
      {0, 1, 2, 21079.775522736683, 302.72930791032559},
      {0, 2, 1, 8729.2493408678238, 164.21253478428869},
      {1, 0, 2, 10166.35200180133, -73.029950609600419},
      {1, 2, 1, 5765.9068284599116, -19.91843918004372},
      {2, 1, 1, 3892.8199868778306, -25.514043184260593},
      {2, 2, 2, 8745.3180751447871, -134.7049527767272},
  };
  for(const auto& e : golden)
  {
    CAPTURE(e.mode, e.interp, e.channels);
    RigV1 r{e.channels};
    auto& in = r.g->inputs;
    in.pos.value = 0.3f;
    in.pos_j.value = 0.5f;
    in.pos_j_r.value = 0.7f;
    in.dur.value = 0.05f;
    in.dur_j.value = 0.3f;
    in.dur_j_r.value = 0.5f;
    in.rate.value = 1.2f;
    in.rate_j.value = 0.2f;
    in.rate_j_r.value = 0.6f;
    in.density.value = 8.f;
    in.dens_j.value = 0.5f;
    in.dens_j_r.value = 10.f;
    in.gain.value = 0.8f;
    in.gain_j.value = 0.4f;
    in.win_coefs.value = {0.3f, 0.2f};
    in.window_mode.value = decltype(in.window_mode.value)(e.mode);
    in.interp_type.value = decltype(in.interp_type.value)(e.interp);
    in.src_channels.value = e.channels;
    in.loopmode.value = e.interp == 1;
    in.reverse.value = e.mode == 2;
    r.start();
    double energy = 0, weighted = 0;
    for(int t = 0; t < 100; t++)
    {
      r.tick();
      for(int c = 0; c < e.channels; c++)
        for(int i = 0; i < RigV1::F; i++)
        {
          energy += r.out[c][i] * r.out[c][i];
          weighted += r.out[c][i] * (1 + (i % 7));
        }
    }
    CHECK(energy == Catch::Approx(e.energy).epsilon(1e-9));
    CHECK(weighted == Catch::Approx(e.weighted).epsilon(1e-7));
  }
}
#endif
