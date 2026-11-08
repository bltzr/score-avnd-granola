#pragma once
// A Granola processor with a sound and output buffers, run as score would:
// for tests and benchmarks.
#include <Granola/Granola.hpp>

#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

namespace granola_test
{
struct Rig
{
  static constexpr int rate = 48000;
  static constexpr int F = 512;

  int frames;
  int sound_channels;
  std::vector<std::vector<float>> sound;
  std::vector<const float*> sound_ptrs;
  std::vector<std::vector<double>> out;
  std::vector<double*> out_ptrs;
  std::unique_ptr<Granola::Granola> g = std::make_unique<Granola::Granola>();
  int scan = -1;
  bool scanning = false;

  //! A sine per channel, \p seconds long; \p outputs buffers, which the
  //! processor's requests resize up to
  explicit Rig(double seconds = 1., int channels = 1, int outputs = 2)
      : frames{int(seconds * rate)}
      , sound_channels{channels}
  {
    sound.resize(channels, std::vector<float>(frames));
    for(int c = 0; c < channels; c++)
      for(int i = 0; i < frames; i++)
        sound[c][i] = 0.3f
                      * std::sin(
                          2. * std::numbers::pi * 220. * (c + 1) * i / double(rate));
    for(auto& c : sound)
      sound_ptrs.push_back(c.data());

    g->inputs.sound.soundfile.data = sound_ptrs.data();
    g->inputs.sound.soundfile.frames = frames;
    g->inputs.sound.soundfile.channels = channels;
    g->inputs.dur.value = 0.05f;
    g->inputs.density.value = 20.f;
    g->inputs.pos.value = 0.1f;
    set_outputs(outputs);
    g->outputs.audio.request_channels = [this](int n) { set_outputs(n); };
    g->send_message = [this](Granola::Granola::processor_to_ui m) {
      if(auto* h = ossia::get_if<Granola::Granola::playheads>(&m.message))
      {
        scan = h->scan;
        scanning = h->scanning;
      }
    };
    // As score does: the controls' update() first, then prepare()
    g->inputs.num_voices.update(*g);
    g->prepare({.rate = double(rate), .frames = F});
  }

  void set_outputs(int n)
  {
    if(n > (int)out.size())
    {
      out.resize(n, std::vector<double>(F));
      out_ptrs.clear();
      for(auto& c : out)
        out_ptrs.push_back(c.data());
    }
    g->outputs.audio.samples = out_ptrs.data();
    g->outputs.audio.channels = n;
  }

  void tick()
  {
    for(auto& c : out)
      std::fill(c.begin(), c.end(), 0.);
    (*g)({.frames = F});
  }
  void run(double seconds)
  {
    for(int t = 0; t < seconds * rate / F; t++)
      tick();
  }
  int outputs() const noexcept { return (int)g->outputs.audio.channels; }
};
}
