#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace Granola
{
/**
 * The window of a grain: its level over its phase, in [0, 1], peaking at 1.
 * The processor plays it and the panel draws it, from the same values.
 *
 * Its two coefficients come from the "Window coefs" pad: a = b = 1 at the
 * bottom (flat), growing with y, x tilting the peak from the start (x = 0)
 * to the end (x = 1). Both are at least 1, where the shapes below are
 * defined, bounded and smooth.
 */
struct window_shape
{
  enum kind : uint8_t
  {
    beta = 0,       // x^(a-1) (1-x)^(b-1)
    cosine = 1,     // sin(pi x^ea)^eb
    kumaraswamy = 2 // x^(a-1) (1-x^a)^(b-1)
  };

  //! How far y on the pad takes the coefficients
  static constexpr double radius = 64.;

  kind type{beta};
  double a{1.}, b{1.};
  // The logarithm of the peak (beta), or what scales to 1 (kumaraswamy);
  // the two exponents (cosine)
  double norm{0.};
  double ex{1.}, ey{1.};

  //! The coefficients of a point of the pad, in [0, 1]^2
  static window_shape from_pad(int type, double x, double y) noexcept
  {
    x = std::clamp(x, 0., 1.);
    y = std::clamp(y, 0., 1.);
    const double angle = (1. - x) * std::numbers::pi / 2.;
    return make(
        kind(std::clamp(type, 0, 2)), 1. + y * radius * std::cos(angle),
        1. + y * radius * std::sin(angle));
  }

  static window_shape make(kind type, double a, double b) noexcept
  {
    window_shape w;
    w.type = type;
    // 1 within rounding, e.g. 1 + 64 cos(pi / 2): exactly, a flat side, else
    // a zero of no width at its end
    w.a = a < 1. + 1e-9 ? 1. : a;
    w.b = b < 1. + 1e-9 ? 1. : b;
    switch(type)
    {
      case beta: {
        // The mode, where a shape with a and b above 1 peaks; one at 1
        // peaks at its end
        const double s = w.a + w.b - 2.;
        const double mode = s > 0. ? (w.a - 1.) / s : 0.5;
        w.norm = w.log_beta(mode);
        break;
      }
      case kumaraswamy: {
        const double ab = w.a * w.b;
        const double mode
            = ab > 1. ? std::pow((w.a - 1.) / (ab - 1.), 1. / w.a) : 0.5;
        const double peak = w.raw_kumaraswamy(mode);
        w.norm = peak > 0. ? 1. / peak : 1.;
        break;
      }
      case cosine:
        w.ex = std::exp(w.a / 10.);
        w.ey = std::exp(w.b / 10.);
        break;
    }
    return w;
  }

  //! The level at \p phase, in [0, 1]
  double operator()(double phase) const noexcept
  {
    phase = std::clamp(phase, 0., 1.);
    double v = 0.;
    switch(type)
    {
      case beta:
        v = std::exp(log_beta(phase) - norm);
        break;
      case kumaraswamy:
        v = raw_kumaraswamy(phase) * norm;
        break;
      case cosine:
        v = std::pow(std::sin(std::numbers::pi * std::pow(phase, ex)), ey);
        break;
    }
    return std::clamp(v, 0., 1.);
  }

  friend bool operator==(const window_shape&, const window_shape&) = default;

private:
  // (e - 1) log(x), 0 for e = 1 whatever x: a flat side, even at x = 0
  static double term(double e, double x) noexcept
  {
    return e == 1. ? 0. : (e - 1.) * std::log(x);
  }
  double log_beta(double x) const noexcept
  {
    return term(a, x) + (b == 1. ? 0. : (b - 1.) * std::log1p(-x));
  }
  double raw_kumaraswamy(double x) const noexcept
  {
    const double xa = std::pow(x, a);
    return std::exp(term(a, x) + (b == 1. ? 0. : (b - 1.) * std::log1p(-xa)));
  }
};

/**
 * A window's levels at evenly spaced phases, read with linear interpolation:
 * what grains read rather than the window's math, which takes logarithms and
 * exponentials. Every grain of a window reads the same table.
 */
struct window_table
{
  static constexpr int size = 2048;

  //! Off the window by at most this much, else the window is computed
  static constexpr double tolerance = 1e-3;

  window_shape shape;
  bool built{};
  // Whether it is within tolerance of the window: not a spike narrower than
  // its steps, e.g. the cosine window at the top of the pad
  bool accurate{};
  std::array<float, size + 1> level{};

  void build(const window_shape& w) noexcept
  {
    shape = w;
    for(int i = 0; i <= size; i++)
      level[i] = float(w(double(i) / size));
    // Checked between its points, where it is furthest from the window
    accurate = true;
    for(int i = 0; i < size && accurate; i++)
      accurate = std::abs((level[i] + level[i + 1]) / 2. - w((i + 0.5) / size)) <= tolerance;
    built = true;
  }

  //! The level at \p phase, in [0, 1]
  float operator()(double phase) const noexcept
  {
    const double p = std::clamp(phase, 0., 1.) * size;
    const int i = std::min(int(p), size - 1);
    const float t = float(p - i);
    return level[i] + t * (level[i + 1] - level[i]);
  }
};
}
