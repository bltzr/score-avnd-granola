
#include "grain.hpp"

#include <cmath>
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
  m_chan_amp = {1};
  m_buf_len = 0;
  m_incr = 0;
  m_shape_x = 1.;
  m_shape_y = 1.;
  m_phase_counter = 0;
}

void GranuGrain::set(
    double start, double dur_samps, double rate,
    //long buffer_index,
    const boost::container::static_vector<double, 2>& shape_coef,
    const boost::container::static_vector<double, NCHAN>& amps,
    const halp::soundfile_port<"Sound">& buf_proxy,
    //const halp::soundfile_port<"Window">& wind_proxy,
    //double sr,
    bool loopmode, long windowType, long channel_offset, long src_channels)
{
  using namespace std;
  m_buf_len = buf_proxy.frames() - 1;
  m_buf_chans = buf_proxy.channels();
  //m_buf_sr = sr;
  //m_buf_index = buffer_index;
  m_src_channels = src_channels;

  amp_init.clear();
  amp_init.resize(m_src_channels);

  // m_src_channels = CLAMP(src_channels, 1, m_buf_chans);
  m_channel_offset = CLAMP(channel_offset, 0, m_buf_chans - 1);

  //  printf("incr_src_channels %ld buf chans %ld\n", m_src_channels, m_buf_chans);

  //  post("shape coef size %ld %f", shape_coef.size(), shape_coef[0] );

  m_startpoint = CLAMP(start, 0, 1) * m_buf_len;

  // duration of grain
  // or if negative use the length of the original sample scaled by ratio

  double gr_dur = (dur_samps <= 0 || dur_samps >= 1) ? m_buf_len : dur_samps * m_buf_len;

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
    double playlen = round(rate * gr_dur);
    m_playlen = playlen;
  }
  else
  {
    gr_dur = (gr_dur >= m_buf_len) ? m_buf_len : gr_dur;
    double playlen = round(rate * gr_dur);
    double playmax = m_buf_len - m_startpoint;
    m_playlen = (playlen <= playmax) ? playlen : playmax;
  }

  const auto prev_count = m_max_count;
  m_incr = rate / m_playlen; // or m_playlen - 1?
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

  if(prev_count != m_max_count || m_wind_type != windowType || m_shape != shape_coef)
  {
    m_shape = shape_coef;
    m_window_update.clear();
    m_window_update.resize(m_max_count + 1);
    m_window.resize(m_max_count + 1);
  }

  m_wind_type = windowType;

  // to do soon: deal with how to switch window types, buffers?
  if(shape_coef.size() == 2)
  {
    m_shape_x = shape_coef[0];
    m_shape_y = shape_coef[1];
  }

  if(m_wind_type == 0)
  {

    //    printf("coef %f %f \n", m_shape_x, m_shape_y);

    setBeta_ab(m_shape_x, m_shape_y);
    m_wind_norm_coef = getBetaScalar(m_shape_x, m_shape_y, m_incr);
    // printf("coef %f\n", m_wind_norm_coef);
  }
  else if(m_wind_type == 2)
  {
    m_shape_x = m_shape_x < 1 ? 2. : m_shape_x;
    m_shape_y = m_shape_y < 1 ? 2. : m_shape_y;
    m_wind_norm_coef = 1. / kumaraswamy_peak(m_shape_x, m_shape_y);
  }
  else
    m_wind_norm_coef = 1;

  // type 1 cos has no precalc coeffs

  //post("%f", m_wind_max);

  m_active = true;
}

double GranuGrain::window(long phase_index)
{
  phase_index = std::clamp(phase_index, (long)0, (long)m_max_count);
  if(m_window_update.test(phase_index))
    return m_window[phase_index];

  auto win = [this] {
    const double phase = m_phase_counter == m_max_count ? 1. : m_phase_counter * m_incr;

    switch(m_wind_type)
    {
      case 0:

        return CLAMP(
            betaNumerator(phase, m_shape_x, m_shape_y) * m_wind_norm_coef, 0., 1.);
      case 1: {
        double px = pow_fast(phase, exp(m_shape_x)); //fastPrecisePow
        double ax = sin(PI * px);
        return pow_fast(ax, exp(m_shape_y));
      }
      case 2:
        return kumaraswamy(phase, m_shape_x, m_shape_y) * m_wind_norm_coef;
      default:
        return 0.;
    }
  }();

  m_window[phase_index] = win;
  m_window_update.set(phase_index);
  return win;
}

typedef enum _granu_interp
{
  NONE,
  LINEAR,
  CUBIC
} eGInterp;

std::span<double>
GranuGrain::incr(halp::soundfile_port<"Sound">& snd, const long interpType)
{
  // output amps is the src_channels here
  const size_t nchans = m_src_channels;

  std::span<double> amps(amp_init.data(), amp_init.size());
  //amps.reserve(nchans);
  // to do: pre-allocate everything when dsp is reset and don't use dynamic memory here

  // printf("incr_src_channels %ld\n", nchans);

  const double win = window(m_phase_counter);

  //qDebug() << phase << " : " << win;

  const double _sampIdx = [&] {
    const double phase = m_phase_counter == m_max_count ? 1. : m_phase_counter * m_incr;
    double _sampIdx = 0.;
    if(m_direction) // true == backwards
      _sampIdx = m_startpoint + ((1 - phase) * m_playlen);
    else
      _sampIdx = m_startpoint + (phase * m_playlen);

    if(m_loop_mode)
      _sampIdx = wrapDouble(_sampIdx, m_buf_len);
    return _sampIdx;
  }();

  const int N = std::min((long)nchans, (long)m_buf_chans);

  {
    const bool loop = m_loop_mode;
    for(int i = 0; i < N; i++)
    {
      int chan = (i + m_channel_offset < m_buf_chans) ? i + m_channel_offset
                                                      : m_buf_chans - 1;
      auto bufferData = snd.soundfile.data[chan];
      double _playSamp;

      switch(interpType)
      {
        case LINEAR: {
          const double lowerSamp = std::floor(_sampIdx);
          const double upperSamp = std::ceil(_sampIdx);
          const double upperVal
              = loop ? bufferData[(long)wrapDouble(upperSamp, m_buf_len)]
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
            a1 = lsamp - 1 < 0
                     ? 0
                     : bufferData
                         [i + m_buf_chans * (long)wrapDouble(lowerSamp - 1, m_buf_len)];
            b = bufferData[lsamp];
            c = bufferData[(long)wrapDouble(lowerSamp + 1, m_buf_len)];
            d = bufferData[(long)wrapDouble(lowerSamp + 2, m_buf_len)];
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

  if(long missingCh = nchans - m_buf_chans; missingCh > 0)
  {
    while(missingCh--)
    {
      amps[missingCh] = amps.back();
    }
  }

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
