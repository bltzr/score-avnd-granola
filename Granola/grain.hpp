#pragma once

#include <vector>
//#include "buffer_proxy.hpp"
#include "utils.hpp"

#include <boost/container/static_vector.hpp>
#include <boost/dynamic_bitset.hpp>

#include <halp/controls.hpp>

#include <cmath>

#include <limits>

#define NCHAN 1

struct GranuGrain
{
  bool m_active = false;

  double m_startpoint; // in samples (floats for interpolation)
  long m_buf_index;

  long m_wind_index;
  long m_wind_type = 0;

  double m_shape_x = 1.;
  double m_shape_y = 1.;
  double m_wind_norm_coef = 1.0;

  boost::container::static_vector<double, 2> m_shape;
  boost::container::static_vector<double, NCHAN> m_chan_amp;

  boost::dynamic_bitset<> m_window_update;
  std::vector<double> m_window;

  long m_buf_chans = 1;
  double m_buf_len = 0;
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
      //long buffer_index, // add this when we have several buffers
      const boost::container::static_vector<double, 2>&
          shape_coef, // if one number then look for window, if two do shaping?, or add another inlet for
      const boost::container::static_vector<double, NCHAN>& amps,
      const halp::soundfile_port<"Sound">& buf_proxy,
      //const halp::soundfile_port<"Window">& wind_proxy, // future holder of optional window buffer
      //double sr,
      bool loopmode, long windowType, long channel_offset, long src_channels);

  //std::vector<double> incr( float *bufferData, long interpType );
  std::span<double> incr(halp::soundfile_port<"Sound">& snd, long interpType);

  void reset();

  double window(long phase);

private:
  double m_playlen;
  bool m_direction;
  //double      m_phase;
  double m_incr;

  inline void setBeta_ab(double a, double b)
  {
    constexpr double minx = (1 / DBL_MAX) + 0.00001;
    const double sum = a + b;
    if(sum > 170.0)
    {
      if(a > b)
      {
        m_shape_x = a - (sum - 170);
        m_shape_y = (b < minx ? minx : b);
      }
      else
      {
        m_shape_x = (a < minx ? minx : a);
        m_shape_y = b - (sum - 170);
      }
    }
    else
    {
      m_shape_x = CLAMP(m_shape_x, minx, 170);
      m_shape_y = CLAMP(m_shape_y, minx, 170);
    }
  }
};

static inline double pow_fast(double a, double b)
{
  union
  {
    double d;
    long long x;
  } u = {a};
  u.x = (long long)(b * (u.x - 4606921278410026770LL) + 4606921278410026770LL);
  return u.d;
}

static inline double kumaraswamy(double x, double a, double b)
{ // a and b >= 1
  return a * b * fastPrecisePow(x, a - 1.)
         * fastPrecisePow(1. - fastPrecisePow(x, a), b - 1.);
}

static inline double kumaraswamy_peak(double a, double b)
{
  if((a <= 1) && (b <= 1))
    return 1;

  double mode = fastPrecisePow((a - 1) / (a * b - 1), 1. / a);
  return kumaraswamy(mode, a, b);
}

static inline double fixDenorm(double x)
{
  return ((fabs(x) < 1e-20f) ? 0.0 : x);
}

// phase input should be 0 < x < 1
// currently I'm not seeing the full range of the curve for a,b < 1, so maybe the phase needs to be scaled somehow
static inline double betaNumerator(double x, double a, double b)
{
  const double num = pow_fast(x, a - 1) * pow_fast(1 - x, b - 1);

  //   if( std::isinf(num) ) printf("inf! x %f\n", x);

  return std::isinf(num) ? 1 : num;
}

static inline double clampGammaDouble(double x)
{
  constexpr double minx = (1 / DBL_MAX) + 0.00001;
  return CLAMP(x, minx, 170.0);
}

static inline double betaFN(double a, double b)
{
  return tgamma(a) * tgamma(b) / tgamma(a + b);
}

static inline double lbetaFn(double a, double b)
{
  return exp(lgamma(a) + lgamma(b) - lgamma(a + b));
}

static inline double betaPDF(double x, double a, double b)
{
  return pow_fast(x, a - 1) * pow_fast(1 - x, b - 1)
         / betaFN(a, b); // could cache gammas here
}

static inline double betaMode(double a, double b)
{
  if(a > 1 && b > 1)
    return (a - 1) / (a + b - 2);
  else if(a == 1 && b == 1) // all 1s
    return 0.5;
  else if(a < 1 && b < 1) // bi modal
    return 0.0001;
  else if(a < 1 && b >= 1)
    return 0.0001;
  else if(a >= 1 && b < 1)
    return 0.9999;
  else if(a == 1 && b > 1)
    return 0.0001;
  else if(a > 1 && b == 1)
    return 0.9999;
  else
  {
    printf("unmatched beta mode %f %f returning 0\n", a, b);
    return 0;
  }
}

static inline double getBetaScalar(double a, double b, double stepsize)
{

  // reusing denominator: (betaNum / betaDen) == betaPDF
  const double betaDenominator = lbetaFn(a, b);

  if(a > 1 && b > 1)
    return 1.
           / (betaDenominator
              * (betaNumerator((a - 1) / (a + b - 2), a, b) / betaDenominator));
  else if(a > 1 && b == 1)
    return 1. / (betaDenominator * (betaNumerator(1., a, b) / betaDenominator));
  else if(a == 1 && b > 1)
    return 1. / (betaDenominator * (betaNumerator(0., a, b) / betaDenominator));
  else if(a >= 1 && b < 1) // in this case x(1) = inf
    return 1.
           / (betaDenominator * (betaNumerator(1 - stepsize, a, b) / betaDenominator));
  else if(a < 1 && b >= 1) // in this case x(0) = inf
    return 1. / (betaDenominator * (betaNumerator(stepsize, a, b) / betaDenominator));
  else if(a < 1 && b < 1)
  {
    if(a > b) // if a > b, then use x(1 - stepsize)
      return 1.
             / (betaDenominator * (betaNumerator(1 - stepsize, a, b) / betaDenominator));
    else
      return 1. / (betaDenominator * (betaNumerator(stepsize, a, b) / betaDenominator));
  }
  else if(a == 1 && b == 1)
    return 1.;
  else
    printf("unknown situation a %f b %f\n", a, b);

  return 0;
}

static inline double betaMax(double a, double b)
{
  return betaPDF(betaMode(a, b), a, b);
}
