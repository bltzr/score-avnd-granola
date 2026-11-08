#pragma once

#include <array>
#include <bit>
#include <vector>
//#include "buffer_proxy.hpp"
#include "utils.hpp"
#include "Window.hpp"

#include <ossia/math/safe_math.hpp>

#include <boost/container/static_vector.hpp>

#include <halp/sound_set.hpp>
#include <halp/soundfile_port.hpp>

#include <cmath>

#include <limits>

#define NCHAN 8

// Lightweight view over any per-channel sample storage (soundfile port, file
// of a sound set). Grains capture one at spawn time; those of a sound set also
// pin the file, so a change of the folder never pulls memory out from under a
// playing grain.
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

struct GranuGrain
{
  bool m_active = false;

  double m_startpoint; // in samples (floats for interpolation)
  long m_buf_index;

  long m_wind_index;
  // The grain's window, from the processor, and the table of it that it
  // reads, if any: the processor keeps it while a grain reads it
  Granola::window_shape m_win;
  const Granola::window_table* m_table{};
  boost::container::static_vector<double, NCHAN> m_chan_amp;
  // Spread: the grain's level in every output, and on top of it in two
  // neighbouring ones, `a` and `a + 1` (none when a < 0). As small for a
  // hundred loudspeakers as for two.
  struct Pan
  {
    float all{1.f};
    int a{-1};
    float at_a{}, at_b{};
  } m_pan;


  long m_buf_chans = 1;
  double m_buf_len = 0;
  // The part of the buffer the grain reads, in samples: Loop wraps in it,
  // else the grain stops at its end
  double m_region_start = 0;
  double m_region_len = 0;
  double wrapInRegion(double idx) const noexcept
  {
    return m_region_start + wrapDouble(idx - m_region_start, m_region_len);
  }
  double m_buf_sr = 0;

  bool m_loop_mode = false;

  long m_phase_counter = 0;
  long m_max_count = 0;

  long m_channel_offset = 0;
  long m_src_channels = 1;

  boost::container::static_vector<double, NCHAN> amp_init;

  // src channel count
  //  0 = amps used as weights for 1 channel with offset option
  //  >0 = amps used as weights for src channels, if set to 2 works for stereo files,

  void
  set(double start, double dur_samps, double rate,
      const Granola::window_shape& window, const Granola::window_table* table,
      const boost::container::static_vector<double, NCHAN>& amps,
      const GrainSource& src, halp::sound_pin pin,
      //const halp::soundfile_port<"Window">& wind_proxy, // future holder of optional window buffer
      //double sr,
      bool loopmode, long channel_offset, long src_channels,
      double region_start = 0., double region_end = 1.);

  //std::vector<double> incr( float *bufferData, long interpType );
  std::span<double> incr(long interpType);

  GrainSource m_source{};
  halp::sound_pin m_pin;

  void reset();
  //! Fades out over \p samples, then stops: a grain stopped before its end
  //! without a click
  void release(long samples) noexcept
  {
    if(m_active && m_release == 0)
      m_release = m_release_left = std::max(samples, 1L);
  }
  long m_release{}, m_release_left{};

  // Current read position, normalized to [0, 1] over the buffer
  double position() const noexcept;

  // Window value computed by the last call to incr()
  double m_last_win = 0.;

  double window(long phase) const noexcept;

private:
  double m_playlen;
  bool m_direction;
  //double      m_phase;
  double m_incr;

};
