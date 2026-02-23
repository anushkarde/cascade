#include "sim/provider.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sim {

// -----------------------------------------------------------------------------
// TokenBucket
// -----------------------------------------------------------------------------

TokenBucket::TokenBucket(double rate_per_sec, double capacity)
    : rate_per_sec_(rate_per_sec), capacity_(capacity), tokens_(capacity),
      last_refill_(std::chrono::steady_clock::now()) {
  if (rate_per_sec <= 0.0 || capacity <= 0.0) {
    throw std::runtime_error("TokenBucket: rate and capacity must be positive");
  }
}

void TokenBucket::Refill() {
  const auto now = std::chrono::steady_clock::now();
  const auto elapsed = std::chrono::duration<double>(now - last_refill_).count();
  tokens_ = std::min(capacity_, tokens_ + elapsed * rate_per_sec_);
  last_refill_ = now;
}

void TokenBucket::Acquire(double tokens) {
  if (tokens <= 0.0) return;
  std::unique_lock lock(mutex_);
  while (tokens_ < tokens) {
    Refill();
    if (tokens_ >= tokens) break;
    const double wait_sec = (tokens - tokens_) / rate_per_sec_;
    cv_.wait_for(lock, std::chrono::duration<double>(wait_sec), [this, tokens] {
      Refill();
      return tokens_ >= tokens;
    });
  }
  tokens_ -= tokens;
}

// -----------------------------------------------------------------------------
// Tier
// -----------------------------------------------------------------------------

Tier::Tier(const TierConfig& config)
    : config_(config),
      token_bucket_(std::make_unique<TokenBucket>(config.rate_per_sec, config.capacity)) {}

void Tier::Enqueue(QueuedAttempt attempt) {
  {
    std::lock_guard lock(queue_mutex_);
    queue_.push(std::move(attempt));
  }
  queue_cv_.notify_one();
}

bool Tier::TryDequeue(QueuedAttempt& out) {
  std::lock_guard lock(queue_mutex_);
  if (queue_.empty() || in_flight_.load() >= config_.concurrency_cap) {
    return false;
  }
  out = std::move(queue_.front());
  queue_.pop();
  in_flight_.fetch_add(1);
  return true;
}

void Tier::BlockingDequeue(QueuedAttempt& out) {
  std::unique_lock lock(queue_mutex_);
  queue_cv_.wait(lock, [this] {
    return !queue_.empty() && in_flight_.load() < config_.concurrency_cap;
  });
  out = std::move(queue_.front());
  queue_.pop();
  in_flight_.fetch_add(1);
}

void Tier::AcquireTokens(QueuedAttempt& attempt) {
  token_bucket_->Acquire(static_cast<double>(attempt.tokens_needed));
}

void Tier::OnAttemptStart() {
  // Called when worker actually starts (after token acquire). in_flight already incremented in TryDequeue.
}

void Tier::OnAttemptFinish() {
  in_flight_.fetch_sub(1);
  queue_cv_.notify_one();
}

// -----------------------------------------------------------------------------
// LatencySampler
// -----------------------------------------------------------------------------

LatencySampler::LatencySampler(const LatencyConfig& config, SeededRng* rng)
    : config_(config), rng_(rng) {}

double LatencySampler::SampleServiceTime(const LatencyParams& params, const LatencyContext& ctx) {
  double raw = 0.0;
  switch (params.dist) {
    case LatencyParams::Dist::Lognormal: {
      double mu = params.param1;
      double sigma = params.param2;
      if (ctx.node_type == NodeType::Plan || ctx.node_type == NodeType::ExtractEvidence ||
          ctx.node_type == NodeType::DecideNext) {
        mu += 0.001 * static_cast<double>(ctx.token_length_est);
      }
      raw = rng_->Lognormal(mu, sigma);
      break;
    }
    case LatencyParams::Dist::Gamma:
      raw = rng_->Gamma(params.param1, params.param2);
      break;
    case LatencyParams::Dist::Linear: {
      double base = params.param1;
      double coeff = params.param2;
      if (ctx.node_type == NodeType::Chunk) {
        raw = base + coeff * static_cast<double>(ctx.pdf_size_est) + rng_->Uniform(-5.0, 5.0);
      } else if (ctx.node_type == NodeType::SimilaritySearch) {
        raw = base + coeff * static_cast<double>(ctx.num_chunks_est);
      } else {
        raw = base + rng_->Uniform(-2.0, 2.0);
      }
      raw = std::max(1.0, raw);
      break;
    }
  }
  if (params.tail_prob > 0.0 && rng_->Bernoulli(params.tail_prob)) {
    raw *= params.tail_multiplier;
  } else if (params.tail_prob == 0.0 && params.tail_multiplier != 1.0) {
    raw *= params.tail_multiplier;
  }
  return std::max(1.0, raw);
}

LatencySample LatencySampler::Sample(const LatencyContext& ctx, int timeout_ms, double p_fail) {
  LatencySample result;
  const LatencyParams& params = config_.Get(ctx.node_type);
  result.service_time_ms = SampleServiceTime(params, ctx);

  if (rng_->Bernoulli(p_fail)) {
    result.failed = true;
    return result;
  }

  if (timeout_ms > 0 && result.service_time_ms > static_cast<double>(timeout_ms)) {
    result.timeout = true;
    result.service_time_ms = static_cast<double>(timeout_ms);
    return result;
  }

  return result;
}

// -----------------------------------------------------------------------------
// ProviderManager
// -----------------------------------------------------------------------------

ProviderManager::ProviderManager(const ProviderConfig& config) {
  for (const auto& tc : config.tiers) {
    tier_index_[tc.provider][tc.tier_id] = tiers_.size();
    tiers_.push_back(std::make_unique<Tier>(tc));
  }
}

Tier* ProviderManager::GetTier(const std::string& provider, int tier_id) {
  auto pit = tier_index_.find(provider);
  if (pit == tier_index_.end()) return nullptr;
  auto tit = pit->second.find(tier_id);
  if (tit == pit->second.end()) return nullptr;
  return tiers_[tit->second].get();
}

const Tier* ProviderManager::GetTier(const std::string& provider, int tier_id) const {
  auto pit = tier_index_.find(provider);
  if (pit == tier_index_.end()) return nullptr;
  auto tit = pit->second.find(tier_id);
  if (tit == pit->second.end()) return nullptr;
  return tiers_[tit->second].get();
}

}  // namespace sim
