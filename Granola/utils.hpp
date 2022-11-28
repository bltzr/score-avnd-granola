#pragma once

#include <cmath>

#include <cfloat>
#include <cstdint>
#include <limits>

#ifndef PI
/** The pi constant.				*/
#define PI 3.14159265358979323846
#endif
#ifndef TWOPI
/** Twice the pi constant.			*/
#define TWOPI 6.28318530717958647692
#endif // TWOPI
#ifndef PIOVERTWO
/** Half of the pi constant.		*/
#define PIOVERTWO 1.57079632679489661923
#endif // PIOVERTWO

#define CLAMP(a, lo, hi) ((a) > (lo) ? ((a) < (hi) ? (a) : (hi)) : (lo))

static constexpr inline double linear_interp(double v0, double v1, double t)
{
  return v0 + ((v1 - v0) * t);
}

static constexpr inline double
cubicInterpolate(double a, double b, double c, double d, double x)
{
  return b
         + 0.5 * x
               * (c - a
                  + x * (2.0 * a - 5.0 * b + 4.0 * c - d + x * (3.0 * (b - c) + d - a)));
}

static inline double wrapDouble(double p_current, double tablesize)
{
  if(p_current < 0)
    return fmod(p_current, tablesize) + tablesize;
  else
    return fmod(p_current, tablesize);
}

// should be much more precise with large b
inline double fastPrecisePow(double a, const double b)
{
  // calculate approximation with just the fraction of the exponent
  int32_t exp = (int32_t)b;
  union
  {
    double d;
    int32_t x[2];
  } u = {a};
  u.x[1] = (int32_t)((b - exp) * (u.x[1] - 1072632447) + 1072632447);
  u.x[0] = 0;

  // exponentiation by squaring with the exponent's integer part
  // double r = u.d makes everything much slower, not sure why
  double r = 1.0;
  while(exp)
  {
    if(exp & 1)
    {
      r *= a;
    }
    a *= a;
    exp >>= 1;
  }

  return r * u.d;
}

static inline double
expA(double x0) // approx exp(x) (more accurate than 'fastexp()') from sm_filter_pack
{
  double x = 0.999996 + (0.031261316 + (0.00048274797 + 0.000006 * x0) * x0) * x0;
  x *= x;
  x *= x;
  x *= x;
  x *= x;
  x *= x;
  return x;
}

double bicubicInterp_acc(double p[4][4], double x, double y);

float FTAcos(float angle);
float FTAsin(float angle);
float cos_52s(float x);
