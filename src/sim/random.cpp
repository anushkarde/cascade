#include "sim/random.h"

#include <cmath>
#include <limits>

namespace sim {

namespace {

// Box-Muller for normal from uniform
double NormalFromUniform(double u1, double u2) {
  if (u1 <= 0.0 || u1 >= 1.0) return 0.0;
  const double r = std::sqrt(-2.0 * std::log(u1));
  return r * std::cos(2.0 * 3.14159265358979323846 * u2);
}

}  // namespace

SeededRng::SeededRng(std::uint64_t seed) {
  std::uint64_t s = seed;
  for (int i = 0; i < 4; ++i) {
    s += 0x9e3779b97f4a7c15ULL;
    std::uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    z = z ^ (z >> 31);
    s_[i] = z;
  }
}

std::uint64_t SeededRng::Rotl(std::uint64_t x, int k) {
  return (x << k) | (x >> (64 - k));
}

std::uint64_t SeededRng::U64() {
  const std::uint64_t result = Rotl(s_[0] + s_[3], 23) + s_[0];
  const std::uint64_t t = s_[1] << 17;
  s_[2] ^= s_[0];
  s_[3] ^= s_[1];
  s_[1] ^= s_[2];
  s_[0] ^= s_[3];
  s_[2] ^= t;
  s_[3] = Rotl(s_[3], 45);
  return result;
}

double SeededRng::Uniform01() {
  return static_cast<double>(U64() >> 11) / 9007199254740992.0;  // 2^53
}

double SeededRng::Uniform(double a, double b) {
  return a + Uniform01() * (b - a);
}

bool SeededRng::Bernoulli(double p) {
  if (p <= 0.0) return false;
  if (p >= 1.0) return true;
  return Uniform01() < p;
}

double SeededRng::Lognormal(double mu, double sigma) {
  const double u1 = Uniform01();
  double u2 = Uniform01();
  while (u2 <= 0.0 || u2 >= 1.0) u2 = Uniform01();
  const double z = NormalFromUniform(u1, u2);
  const double x = std::exp(mu + sigma * z);
  return x > 0.0 ? x : std::numeric_limits<double>::min();
}

double SeededRng::Gamma(double shape, double scale) {
  // Marsaglia and Tsang's method for shape >= 1
  if (shape < 1.0) {
    return Gamma(shape + 1.0, scale) * std::pow(Uniform01(), 1.0 / shape);
  }
  const double d = shape - 1.0 / 3.0;
  const double c = 1.0 / std::sqrt(9.0 * d);
  for (;;) {
    double x, v;
    do {
      x = Normal(0.0, 1.0);
      v = 1.0 + c * x;
    } while (v <= 0.0);
    v = v * v * v;
    const double u = Uniform01();
    if (u < 1.0 - 0.0331 * (x * x) * (x * x)) return d * v * scale;
    if (std::log(u) < 0.5 * x * x + d * (1.0 - v + std::log(v))) return d * v * scale;
  }
}

double SeededRng::Normal(double mean, double stddev) {
  double u1 = Uniform01();
  double u2 = Uniform01();
  while (u1 <= 0.0 || u1 >= 1.0) u1 = Uniform01();
  while (u2 <= 0.0 || u2 >= 1.0) u2 = Uniform01();
  return mean + stddev * NormalFromUniform(u1, u2);
}

}  // namespace sim
