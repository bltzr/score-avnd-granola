// The processor through what a host may do to it, in any order: controls set
// to anything and their update() called, prepare() again with other sizes,
// blocks of any length, sounds replaced by others or removed, MIDI. Nothing
// may crash, and the outputs stay finite. Run it under ASan and UBSan.
#include <ossia/math/safe_math.hpp>

#include <Granola/Granola.hpp>

#include <catch2/catch_all.hpp>

#include <cmath>
#include <limits>
#include <memory>
#include <random>
#include <vector>

namespace
{
struct Sound
{
  std::vector<std::vector<float>> channels;
  std::vector<const float*> ptrs;
  Sound(int frames, int count, std::mt19937& rng)
  {
    std::uniform_real_distribution<float> d{-1.f, 1.f};
    channels.resize(count, std::vector<float>(frames));
    for(auto& c : channels)
      for(auto& s : c)
        s = d(rng);
    for(auto& c : channels)
      ptrs.push_back(c.data());
  }
};

struct Host
{
  static constexpr int max_frames = 8192;
  static constexpr int max_outs = 128;
  std::vector<std::vector<double>> out
      = std::vector<std::vector<double>>(max_outs, std::vector<double>(max_frames));
  std::vector<double*> out_ptrs;
  std::unique_ptr<Granola::Granola> g = std::make_unique<Granola::Granola>();
  int channels = 2;
  // The transport, as a host's musical tick says it: any tempo, stopped,
  // rewinding, jumping
  double tempo = 120.;
  double quarters = 0.;
  double quarters_per_frame = 120. / 60. / 48000.;
  int sig_num = 4, sig_denom = 4;

  Host()
  {
    for(auto& c : out)
      out_ptrs.push_back(c.data());
    g->outputs.audio.samples = out_ptrs.data();
    g->outputs.audio.channels = channels;
    // The host gives the channels asked for, up to what it has
    g->outputs.audio.request_channels
        = [this](int n) { g->outputs.audio.channels = std::clamp(n, 0, max_outs); };
  }

  void tick(int frames)
  {
    halp::tick_musical t{.frames = frames};
    t.tempo = tempo;
    t.signature = {sig_num, sig_denom};
    t.start_position_in_quarters = quarters;
    quarters += quarters_per_frame * frames;
    t.end_position_in_quarters = quarters;
    t.bar_at_start = t.bar_at_end = std::floor(t.start_position_in_quarters / 4.) * 4.;
    (*g)(t);
    bool finite = true;
    for(int c = 0; c < (int)g->outputs.audio.channels; c++)
      for(int s = 0; s < frames; s++)
        finite &= ossia::safe_isfinite(out[c][s]);
    REQUIRE(finite);
  }
};

float any_value(std::mt19937& rng)
{
  static const float specials[]
      = {0.f,
         1.f,
         -1.f,
         0.5f,
         1e-9f,
         1e9f,
         -1e9f,
         std::numeric_limits<float>::quiet_NaN(),
         std::numeric_limits<float>::infinity(),
         -std::numeric_limits<float>::infinity(),
         std::numeric_limits<float>::denorm_min()};
  if(rng() % 3 == 0)
    return specials[rng() % std::size(specials)];
  return std::uniform_real_distribution<float>{-0.5f, 1.5f}(rng);
}
}

TEST_CASE("The processor through anything a host may do", "[fuzz]")
{
  for(unsigned seed = 1; seed <= 60; seed++)
  {
    CAPTURE(seed);
    std::mt19937 rng{seed};
    Host h;
    auto& in = h.g->inputs;
    std::vector<std::unique_ptr<Sound>> sounds;
    // A folder: files of a pool that outlives the grains, as the host's pins
    // would keep them, in any state
    std::vector<std::unique_ptr<Sound>> pool;
    for(int k = 0; k < 8; k++)
      pool.push_back(std::make_unique<Sound>(std::array{5, 100, 4800}[k % 3], 1 + k % 3, rng));
    std::vector<halp::sound_view> views;
    uint64_t generation = 0;

    // Sometimes the controls' update() before prepare(), as score does
    if(rng() % 2)
    {
      in.num_voices.value = int(rng() % 300);
      in.num_voices.update(*h.g);
      h.tick(64); // a tick before prepare(): silent
    }
    h.g->prepare({.rate = 48000., .frames = int(64 << (rng() % 5))});

    for(int step = 0; step < 200; step++)
    {
      switch(rng() % 19)
      {
        case 16: // Duration's mode and time, anything, synced or not
          in.dur_mode.value = decltype(in.dur_mode.value)(rng() % 3);
          in.dur_time.value = any_value(rng) * float(1 + rng() % 100);
          in.dur_time.sync = rng() % 2;
          break;
        case 17: { // Quantize: any unit, past the ones it offers too, any value
          in.quantize.value.unit = halp::grid_unit(int(rng() % 6) - 1);
          in.quantize.value.value = any_value(rng) * float(1 + rng() % 40);
          in.quantize.value.offset = any_value(rng);
          break;
        }
        case 18: { // the transport
          h.tempo = std::array{120., 60., 1., 999., 0., -10., 1e9}[rng() % 7];
          if(rng() % 8 == 0)
            h.tempo = std::numeric_limits<double>::quiet_NaN();
          h.quarters_per_frame
              = std::array{2. / 48000., 0., -1. / 48000., 1e-9, 50. / 48000.}[rng() % 5];
          if(rng() % 4 == 0)
            h.quarters = any_value(rng) * 1e4;
          h.sig_num = int(rng() % 9) - 1;
          h.sig_denom = std::array{4, 8, 0, 2}[rng() % 4];
          break;
        }
        case 0: { // another sound: any length, any channels, sometimes none
          const int frames = std::array{1, 2, 3, 5, 100, 4800, 48000}[rng() % 7];
          const int count = frames > 4800 ? 1 + int(rng() % 2) : 1 + int(rng() % 12);
          sounds.push_back(std::make_unique<Sound>(frames, count, rng));
          auto& s = *sounds.back();
          in.sound.soundfile.data = rng() % 8 ? s.ptrs.data() : nullptr;
          in.sound.soundfile.frames = frames;
          in.sound.soundfile.channels = count;
          in.sound.update(*h.g);
          // The old ones are freed by the host once replaced
          if(sounds.size() > 2)
            sounds.erase(sounds.begin());
          break;
        }
        case 10: { // the folder listed again, its files in any state, any order
          views.clear();
          const int count = int(rng() % 7);
          for(int k = 0; k < count; k++)
          {
            auto& snd = *pool[rng() % pool.size()];
            views.push_back(
                {.name = "f.wav",
                 .data = snd.ptrs.data(),
                 .frames = int64_t(snd.channels[0].size()),
                 .channels = int32_t(snd.channels.size()),
                 .rate = 48000,
                 .status = halp::sound_status(rng() % 4)});
          }
          in.sounds.files = {views, "/f", ++generation};
          in.sounds.value = rng() % 4 ? "/f" : "";
          in.source.value = decltype(in.source.value)(rng() % 2);
          // Past the last entry too, as a cable may send
          in.order.value = decltype(in.order.value)(rng() % 7);
          in.sound_file.value = std::to_string(int(rng() % 9) - 1);
          break;
        }
        case 1:
          in.num_voices.value = int(rng() % 600) - 50;
          in.num_voices.update(*h.g);
          break;
        case 2:
          in.out_channels.value = int(rng() % 200) - 10;
          break;
        case 3:
          in.src_channels.value = int(rng() % 20) - 2;
          in.channel_offset.value = int(rng() % 20) - 2;
          break;
        case 4:
          h.g->prepare({.rate = std::array{44100., 48000., 96000.}[rng() % 3],
                        .frames = int(1 + rng() % 4096)});
          break;
        case 5: { // MIDI
          for(int k = 0, n = int(rng() % 8); k < n; k++)
          {
            const uint8_t status = std::array<uint8_t, 4>{0x90, 0x80, 0xB0, 0x90}[rng() % 4];
            in.midi.midi_messages.push_back(
                libremidi::message{{status, uint8_t(rng() % 128), uint8_t(rng() % 128)}, 0});
          }
          in.on_midi.value = decltype(in.on_midi.value)(rng() % 7);
          in.velocity.value = any_value(rng);
          in.keep_time.value = rng() % 2;
          break;
        }
        case 6:
          in.win_coefs.value = {any_value(rng), any_value(rng)};
          in.window_mode.value = decltype(in.window_mode.value)(rng() % 4);
          in.interp_type.value = decltype(in.interp_type.value)(rng() % 4);
          break;
        case 7:
          in.scan.value = any_value(rng) * 4.f;
          in.scan_mode.value = decltype(in.scan_mode.value)(rng() % 4);
          in.scan_folder.value = decltype(in.scan_folder.value)(rng() % 4);
          break;
        case 8:
          in.sample_start.value = any_value(rng);
          in.sample_end.value = any_value(rng);
          in.loopmode.value = rng() % 2;
          break;
        case 9:
          in.playing.value = rng() % 3 != 0;
          in.playing.update(*h.g);
          if(rng() % 4 == 0)
            in.trig.value.emplace();
          break;
        default: { // the continuous controls, anything
          float* f[] = {&in.pos.value,        &in.pos_j.value,   &in.dur.value,
                        &in.dur_j.value,      &in.rate.value,    &in.rate_j.value,
                        &in.density.value,    &in.dens_j.value,  &in.gain.value,
                        &in.gain_j.value,     &in.spread.value,  &in.reverse_chance.value};
          *f[rng() % std::size(f)] = any_value(rng) * float(1 + rng() % 300);
          in.rate_j_semitones.value = rng() % 2;
          break;
        }
      }
      // Density's values within the block, as automation gives them
      if(rng() % 5 == 0)
        for(int k = int(rng() % 4); k > 0; k--)
          in.density.values[int(rng() % 1100)] = any_value(rng) * float(1 + rng() % 300);
      // A block of any length, up to the host's largest
      h.tick(int(rng() % 3 == 0 ? rng() % 4 : rng() % 8 == 0 ? rng() % Host::max_frames : rng() % 1024));
      in.midi.midi_messages.clear();
      in.density.values.clear();
      in.trig.value.reset();
    }
  }
}
