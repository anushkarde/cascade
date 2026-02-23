#pragma once

#include "sim/types.h"

#include <cstdint>
#include <cmath>
#include <limits>
#include <random>

namespace sim {

// Deterministic seeded RNG for reproducible simulations.
// Uses xoshiro256** for quality and speed.
class SeededRng {
 public:
  explicit SeededRng(std::uint64_t seed);

  std::uint64_t U64();
  double Uniform01();
  double Uniform(double a, double b);
  bool Bernoulli(double p);
  double Lognormal(double mu, double sigma);
  double Gamma(double shape, double scale);
  double Normal(double mean, double stddev);

 private:
  std::uint64_t s_[4];
  static std::uint64_t Rotl(std::uint64_t x, int k);
};

}  // namespace sim
