#include "grain.hpp"

#include <cmath>

namespace Granola
{
namespace
{
constexpr double cubic(double a, double b, double c, double d, double x) noexcept
{
  return b
         + 0.5 * x
               * (c - a + x * (2.0 * a - 5.0 * b + 4.0 * c - d + x * (3.0 * (b - c) + d - a)));
}
}

void Grain::reset() noexcept
{
  m_active = false;
  m_source = {};
  // Releasing a pin never frees anything: the host does, later
  m_pin.reset();
  m_table = nullptr;
  m_phase_counter = 0;
  m_release = m_release_left = 0;
}

void Grain::set(Params p) noexcept
{
  // Too short to read a grain from, or nothing to read: no grain
  if(p.source.frames < 4 || p.source.channels < 1 || !p.source.data)
  {
    reset();
    return;
  }
  m_source = p.source;
  m_pin = std::move(p.pin);
  m_last = p.source.frames - 1.;

  // Its channels, from the offset; past the sound's last, the last one again
  const int sound_channels = (int)p.source.channels;
  m_channels = std::clamp(p.channels, 1, max_channels);
  const int offset = std::clamp(p.channel_offset, 0, sound_channels - 1);
  const int read = std::min(m_channels, sound_channels);
  for(int c = 0; c < m_channels; c++)
    m_chan[c] = p.source.data[std::min(std::min(c, read - 1) + offset, sound_channels - 1)];

  // The region, in the buffer: a sample at least, its end the last sample
  const double rs = std::clamp(p.region_start, 0., 1.);
  const double re = std::clamp(p.region_end, 0., 1.);
  m_region_start = std::min(rs * m_last, m_last - 1.);
  m_region_len = std::clamp((re - rs) * m_last, 1., m_last - m_region_start);

  // In the region, a sample before its end at the latest, never at 0; in a
  // region of one sample at the start, that sample's first part
  const double first = std::max(m_region_start, 0.00000001 * m_last);
  m_startpoint
      = std::clamp(p.start * m_last, first, std::max(first, m_region_start + m_region_len - 1.));

  const double length = (p.duration <= 0. || p.duration * m_last >= m_region_len)
                            ? m_region_len
                            : p.duration * m_last;

  m_backwards = p.rate < 0.;
  const double rate = std::abs(p.rate);
  m_wrap = p.wrap;
  if(m_wrap)
    m_playlen = std::round(length);
  else // up to the region's end, which is in the buffer
    m_playlen = std::min(
        std::round(length),
        std::max(m_region_start + m_region_len - m_startpoint, 1.));
  m_playlen = std::max(m_playlen, 1.);

  m_incr = rate / m_playlen;
  m_max_count = std::lround(m_playlen / rate);

  m_win = p.window;
  m_table = p.table;
  m_amp = p.amp;
  m_phase_counter = 0;
  m_release = m_release_left = 0;
  m_active = true;
}

double Grain::wrap(double idx) const noexcept
{
  const double r = std::fmod(idx - m_region_start, m_region_len);
  return m_region_start + (r < 0. ? r + m_region_len : r);
}

double Grain::read_index(double phase) const noexcept
{
  const double idx = m_startpoint + (m_backwards ? 1. - phase : phase) * m_playlen;
  return m_wrap ? wrap(idx) : idx;
}

double Grain::position() const noexcept
{
  if(m_last <= 0.)
    return 0.;
  const double phase
      = m_phase_counter >= m_max_count ? 1. : double(m_phase_counter) * m_incr;
  return std::clamp(read_index(phase) / m_last, 0., 1.);
}

template <interpolation I, bool Wrap>
int Grain::render_as(int n, double* const* out) noexcept
{
  const int C = m_channels;
  const double region_end = m_region_start + m_region_len;
  // A neighbouring sample of the region, with Wrap at end; else in the
  // sound, and silence past it
  const auto around = [&](double i) {
    if(i >= region_end)
      i -= m_region_len;
    else if(i < m_region_start)
      i += m_region_len;
    return i;
  };

  for(int s = 0; s < n; s++)
  {
    const long count = m_phase_counter;
    const double phase = count >= m_max_count ? 1. : double(count) * m_incr;
    double win = m_table ? (*m_table)(phase) : m_win(phase);
    m_last_win = win;
    win *= m_amp;
    if(m_release > 0)
      win *= double(m_release_left) / double(m_release);

    double idx = m_startpoint + (m_backwards ? 1. - phase : phase) * m_playlen;
    if constexpr(Wrap)
      idx = wrap(idx);
    // Never negative: truncation is the floor
    const long i0 = long(idx);
    const double frac = idx - double(i0);

    if constexpr(I == interpolation::none)
    {
      for(int c = 0; c < C; c++)
        out[c][s] = m_chan[c][i0] * win;
    }
    else if constexpr(I == interpolation::linear)
    {
      long i1 = i0 + 1;
      bool past = false;
      if constexpr(Wrap)
        i1 = long(around(double(i1)));
      else
        past = double(i1) > m_last;
      for(int c = 0; c < C; c++)
      {
        const double a = m_chan[c][i0];
        const double b = past ? 0. : m_chan[c][i1];
        out[c][s] = (a + (b - a) * frac) * win;
      }
    }
    else
    {
      long im1 = i0 - 1, i1 = i0 + 1, i2 = i0 + 2;
      bool before = false, past1 = false, past2 = false;
      if constexpr(Wrap)
      {
        if(m_region_len >= 3.)
        {
          im1 = long(around(double(im1)));
          i1 = long(around(double(i1)));
          i2 = long(around(double(i2)));
        }
        else
        {
          im1 = long(wrap(double(im1)));
          i1 = long(wrap(double(i1)));
          i2 = long(wrap(double(i2)));
        }
      }
      else
      {
        before = im1 < 0;
        past1 = double(i1) > m_last;
        past2 = double(i2) > m_last;
      }
      for(int c = 0; c < C; c++)
      {
        const float* d = m_chan[c];
        out[c][s] = cubic(
                        before ? 0. : d[im1], d[i0], past1 ? 0. : d[i1],
                        past2 ? 0. : d[i2], frac)
                    * win;
      }
    }

    m_phase_counter = count + 1;
    if(m_phase_counter > m_max_count || (m_release > 0 && --m_release_left <= 0))
    {
      reset();
      return s + 1;
    }
  }
  return n;
}

int Grain::render(int n, double* const* out, interpolation interp) noexcept
{
  if(!m_active)
    return 0;
  switch(interp)
  {
    case interpolation::none:
      return m_wrap ? render_as<interpolation::none, true>(n, out)
                    : render_as<interpolation::none, false>(n, out);
    case interpolation::linear:
      return m_wrap ? render_as<interpolation::linear, true>(n, out)
                    : render_as<interpolation::linear, false>(n, out);
    case interpolation::cubic:
    default:
      return m_wrap ? render_as<interpolation::cubic, true>(n, out)
                    : render_as<interpolation::cubic, false>(n, out);
  }
}
}
