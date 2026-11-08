#pragma once
#include "Window.hpp"

#include <halp/sound_set.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

namespace Granola
{
//! How many of a sound's channels a grain reads
inline constexpr int max_channels = 8;

//! How a grain reads between samples: the Interpolation control's choices
enum class interpolation : uint8_t
{
  none,
  linear,
  cubic
};

//! A sound as a grain reads it: a buffer per channel
struct GrainSource
{
  const float* const* data{};
  long channels{0};
  double frames{0};

  explicit operator bool() const noexcept
  {
    return data && channels > 0 && frames > 0;
  }
};

/**
 * A grain: a part of a sound, read forwards or backwards at a speed, in a
 * region of it, under a window.
 */
class Grain
{
public:
  struct Params
  {
    double start{};    // where it starts, a part of the sound
    double duration{}; // a part of the sound; 0 or less: the region
    double rate{1.};   // the speed it reads at; below 0, backwards
    window_shape window;
    const window_table* table{}; // read rather than the window, if any
    double amp{1.};
    GrainSource source;
    halp::sound_pin pin; // keeps a sound of a folder alive
    bool wrap{};         // past the region's end, from its start
    int channel_offset{};
    int channels{1};     // read from channel_offset on
    double region_start{0.}, region_end{1.};
  };

  //! Starts: no grain if the sound is too short to read one from
  void set(Params p) noexcept;

  //! Renders up to \p n samples, out[c][0, n) for each of its channels.
  //! Returns how many: fewer when the grain ends, which it then does.
  int render(int n, double* const* out, interpolation interp) noexcept;

  //! Stops at once
  void reset() noexcept;
  //! Fades out over \p samples, then stops: a grain stopped before its end
  //! without a click
  void release(long samples) noexcept
  {
    if(m_active && m_release == 0)
      m_release = m_release_left = std::max(samples, 1L);
  }

  //! Where it reads, a part of the sound
  double position() const noexcept;
  int channels() const noexcept { return m_channels; }

  // Read by the processor, its panel and its tests
  bool m_active{false};
  // Spread: its level in every output, and on top of it in two neighbouring
  // ones, a and a + 1 (none when a < 0): as small for a hundred loudspeakers
  // as for two
  struct Pan
  {
    float all{1.f};
    int a{-1};
    float at_a{}, at_b{};
  } m_pan;
  window_shape m_win;
  const window_table* m_table{};
  double m_amp{1.};
  GrainSource m_source{};
  halp::sound_pin m_pin;
  long m_phase_counter{0};
  double m_last_win{0.}; // the window at its last sample

private:
  template <interpolation I, bool Wrap>
  int render_as(int n, double* const* out) noexcept;
  double read_index(double phase) const noexcept;
  double wrap(double idx) const noexcept;

  // The buffer of each channel it reads
  std::array<const float*, max_channels> m_chan{};
  int m_channels{1};

  double m_last{0};         // its last sample: frames - 1
  double m_region_start{0}; // in samples
  double m_region_len{0};
  double m_startpoint{0};
  double m_playlen{0}; // the samples it reads
  double m_incr{0};    // its phase, a sample further
  long m_max_count{0}; // the samples it lasts
  bool m_backwards{false};
  bool m_wrap{false};
  long m_release{0}, m_release_left{0};
};
}
