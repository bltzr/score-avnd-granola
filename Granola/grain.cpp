
#include "grain.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

/**
 *  what's better, phase counter vs phase incr?
 *  doing phase += incr is nice for simplicy, but to really hit the last step is difficult due to rounding errors
 *
 *  src_channels:
    if no value set then the source channels are based on the amplist input
    if no value set and no amplist attached, what is default?
 *          do we need to iterated the polybuffer and find the largest number of channels?
 *          seemes better to make the user specify this, since mc. is a fixed number of output channels
 *
 *  ok so: default src_channels 1, default amps 1 channel unity
 *      if src_channels > 1, default amps is 1 for each channel
 */

void GranuGrain::reset()
{
  m_active = false;
  m_source = {};
  // Releasing a pin never frees anything: the host does, later
  m_pin.reset();
  amp_init.clear();
  amp_init.reserve(NCHAN);
  m_chan_amp.clear();
  m_chan_amp.reserve(NCHAN);
  m_chan_amp.push_back(1);
  m_buf_len = 0;
  m_incr = 0;
  m_phase_counter = 0;
}

void GranuGrain::set(
    double start, double dur_samps, double rate,
    const Granola::window_shape& window, const Granola::window_table* table,
    const boost::container::static_vector<double, NCHAN>& amps,
    const GrainSource& src, halp::sound_pin pin,
    //const halp::soundfile_port<"Window">& wind_proxy,
    //double sr,
    bool loopmode, long channel_offset, long src_channels, double region_start,
    double region_end)
{
  using namespace std;
  // Too short to read a grain from, or nothing to read: no grain
  if(src.frames < 4 || src.channels < 1 || !src.data)
  {
    reset();
    return;
  }
  m_source = src;
  m_pin = std::move(pin);
  m_buf_len = src.frames - 1;
  m_buf_chans = src.channels;
  //m_buf_sr = sr;
  //m_buf_index = buffer_index;
  m_src_channels = src_channels;

  amp_init.clear();
  amp_init.resize(m_src_channels);

  // m_src_channels = CLAMP(src_channels, 1, m_buf_chans);
  m_channel_offset = CLAMP(channel_offset, 0, m_buf_chans - 1);

  //  printf("incr_src_channels %ld buf chans %ld\n", m_src_channels, m_buf_chans);

  //  post("shape coef size %ld %f", shape_coef.size(), shape_coef[0] );

  // In the buffer, a sample at least: its end is the last sample, m_buf_len
  m_region_start = std::min(std::clamp(region_start, 0., 1.) * m_buf_len, m_buf_len - 1.);
  m_region_len = std::clamp(
      (std::clamp(region_end, 0., 1.) - std::clamp(region_start, 0., 1.)) * m_buf_len, 1.,
      m_buf_len - m_region_start);

  // Within the region, a sample before its end at the latest, never at 0
  m_startpoint = std::clamp(
      start * m_buf_len, std::max(m_region_start, 0.00000001 * m_buf_len),
      m_region_start + m_region_len - 1.);

  // duration of grain
  // or if negative use the length of the original sample scaled by ratio

  double gr_dur = (dur_samps <= 0 || dur_samps * m_buf_len >= m_region_len)
                      ? m_region_len
                      : dur_samps * m_buf_len;


  // phase goes 0-1
  // if rate is negative, then play backwards
  // to do this some decision has to be made,
  // for instance, right now we will use the same "start" and "end" points,
  // but use a negative incr and start the phase at 1 if backwards
  m_direction = rate < 0;

  rate = abs(rate);

  m_loop_mode = loopmode;
  if(loopmode)
  {
    m_playlen = round(gr_dur);
  }
  else
  {
    // Up to the region's end, which is in the buffer; a sample at least, as
    // the grain starts a sample before it at the latest
    double playmax = std::max(m_region_start + m_region_len - m_startpoint, 1.);
    m_playlen = std::min(round(gr_dur), playmax);
  }

  const auto prev_count = m_max_count;
  const auto prev_incr = m_incr;
  m_incr = rate / m_playlen;
  m_max_count = round(m_playlen / rate);

  //m_phase = rate < 0;
  //printf("direction %i playlen %f count max %ld\n", m_direction, m_playlen, m_max_count);

  /*
     poke(m_panL,     fastcos( pan * quarterPI),    i, 0, 0);
     poke(m_panR,     fastsin( pan * quarterPI),    i, 0, 0);
     poke(m_xexp,     xexp,                             i, 0, 0);
     poke(m_yexp,     yexp,                             i, 0, 0);
     _count += 1;
     */

  if(m_src_channels > 1)
  {
    if(amps.size() == m_src_channels)
      m_chan_amp = amps;
    else
    {
      m_chan_amp.clear();
      m_chan_amp.resize(m_src_channels, amps[0]);
    }
  }
  else
  {
    m_chan_amp = amps;
  }

  m_win = window;
  m_table = table;

  m_active = true;
}

double GranuGrain::window(long phase_index) const noexcept
{
  const double phase
      = phase_index >= m_max_count ? 1. : double(std::max(phase_index, 0L)) * m_incr;
  return m_table ? (*m_table)(phase) : m_win(phase);
}

double GranuGrain::position() const noexcept
{
  if(m_buf_len <= 0.)
    return 0.;
  const double phase = m_phase_counter >= m_max_count ? 1. : m_phase_counter * m_incr;
  double idx = m_startpoint + (m_direction ? 1. - phase : phase) * m_playlen;
  if(m_loop_mode)
    idx = wrapInRegion(idx);
  return std::clamp(idx / m_buf_len, 0., 1.);
}

typedef enum _granu_interp
{
  NONE,
  LINEAR,
  CUBIC
} eGInterp;

std::span<double>
GranuGrain::incr(const long interpType)
{
  // output amps is the src_channels here
  const size_t nchans = m_src_channels;

  std::span<double> amps(amp_init.data(), amp_init.size());
  //amps.reserve(nchans);
  // to do: pre-allocate everything when dsp is reset and don't use dynamic memory here

  // printf("incr_src_channels %ld\n", nchans);

  const double win = window(m_phase_counter);
  m_last_win = win;

  //qDebug() << phase << " : " << win;

  const double _sampIdx = [&] {
    const double phase = m_phase_counter == m_max_count ? 1. : m_phase_counter * m_incr;
    double _sampIdx = 0.;
    if(m_direction) // true == backwards
      _sampIdx = m_startpoint + ((1 - phase) * m_playlen);
    else
      _sampIdx = m_startpoint + (phase * m_playlen);

    if(m_loop_mode)
      _sampIdx = wrapInRegion(_sampIdx);
    return _sampIdx;
  }();

  const int N = std::min((long)nchans, (long)m_buf_chans);

  {
    const bool loop = m_loop_mode;
    for(int i = 0; i < N; i++)
    {
      int chan = (i + m_channel_offset < m_buf_chans) ? i + m_channel_offset
                                                      : m_buf_chans - 1;
      auto bufferData = m_source.data[chan];
      double _playSamp;

      switch(interpType)
      {
        case LINEAR: {
          const double lowerSamp = std::floor(_sampIdx);
          const double upperSamp = std::ceil(_sampIdx);
          const double upperVal
              = loop ? bufferData[(long)wrapInRegion(upperSamp)]
                     : ((upperSamp < m_buf_len) ? bufferData[(long)upperSamp] : 0.0);

          const long lsamp = lowerSamp;
          _playSamp = linear_interp(bufferData[lsamp], upperVal, _sampIdx - lowerSamp);
          break;
        }

        case CUBIC: {
          double a1, b, c, d;
          const double lowerSamp = std::floor(_sampIdx);
          const double frac = _sampIdx - lowerSamp;
          const long lsamp = lowerSamp;

          if(loop)
          {
            // The channels are separate buffers: no interleaving
            a1 = bufferData[(long)wrapInRegion(lowerSamp - 1)];
            b = bufferData[lsamp];
            c = bufferData[(long)wrapInRegion(lowerSamp + 1)];
            d = bufferData[(long)wrapInRegion(lowerSamp + 2)];
          }
          else
          {
            a1 = lsamp - 1 < 0 ? 0 : bufferData[(lsamp - 1)];
            b = bufferData[lsamp];
            c = (lsamp + 1) >= m_buf_len ? 0 : bufferData[(lsamp + 1)];
            d = (lsamp + 2) >= m_buf_len ? 0 : bufferData[(lsamp + 2)];
          }

          _playSamp = cubicInterpolate(a1, b, c, d, frac);
          break;
        }
        case NONE:
        default:
          _playSamp = bufferData[(long)_sampIdx];
          break;
      }
      /*
            // window
            double px = fastPrecisePow(m_phase, exp(m_shape_x));
            double ax = sin( PI * px);
            double windX = fastPrecisePow( ax, exp(m_shape_y));
        */

      amps[i] = _playSamp * win * m_chan_amp[i];
      // to do: pre-allocate everything when dsp is reset and don't use dynamic memory here
    }
  }

  for(size_t i = N; i < nchans; i++)
    amps[i] = amps[N - 1];

  //m_phase += m_incr;
  m_phase_counter++;

  if(m_phase_counter > m_max_count)
  {
    //   printf("released at phase %.17g prev %.17g incr %.17g\n", phase, phase-m_incr, m_incr );
    //   printf("counter %ld maxcount %ld \n", m_phase_counter, m_max_count );
    reset();
  }
  return amps;
}
