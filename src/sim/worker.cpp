#include "sim/worker.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace sim {

void ResultQueue::Push(AttemptResult r) {
  {
    std::lock_guard lock(mutex_);
    if (shutdown_.load()) return;
    queue_.push(std::move(r));
  }
  cv_.notify_one();
}

bool ResultQueue::TryPop(AttemptResult& out) {
  std::lock_guard lock(mutex_);
  if (queue_.empty() || shutdown_.load()) return false;
  out = std::move(queue_.front());
  queue_.pop();
  return true;
}

void ResultQueue::BlockingPop(AttemptResult& out) {
  std::unique_lock lock(mutex_);
  cv_.wait(lock, [this] { return !queue_.empty() || shutdown_.load(); });
  if (shutdown_.load() && queue_.empty()) return;
  out = std::move(queue_.front());
  queue_.pop();
}

void ResultQueue::Shutdown() {
  shutdown_.store(true);
  cv_.notify_all();
}

void LocalQueue::Push(LocalTask t) {
  {
    std::lock_guard lock(mutex_);
    if (shutdown_.load()) return;
    queue_.push(std::move(t));
  }
  cv_.notify_one();
}

bool LocalQueue::TryPop(LocalTask& out) {
  std::lock_guard lock(mutex_);
  if (queue_.empty() || shutdown_.load()) return false;
  out = std::move(queue_.front());
  queue_.pop();
  return true;
}

void LocalQueue::BlockingPop(LocalTask& out) {
  std::unique_lock lock(mutex_);
  cv_.wait(lock, [this] { return !queue_.empty() || shutdown_.load(); });
  if (shutdown_.load() && queue_.empty()) return;
  out = std::move(queue_.front());
  queue_.pop();
}

bool LocalQueue::TimedPop(LocalTask& out, std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  bool ok = cv_.wait_for(lock, timeout, [this] { return !queue_.empty() || shutdown_.load(); });
  if (!ok || (shutdown_.load() && queue_.empty())) return false;
  out = std::move(queue_.front());
  queue_.pop();
  return true;
}

void LocalQueue::Shutdown() {
  shutdown_.store(true);
  cv_.notify_all();
}

bool CancellableSleep(std::chrono::milliseconds total, std::atomic<bool>* cancelled,
                     int chunk_ms) {
  const auto chunk = std::chrono::milliseconds(std::max(1, chunk_ms));
  auto remaining = total;
  while (remaining.count() > 0) {
    if (cancelled && cancelled->load()) return true;
    const auto sleep_time = std::min(remaining, chunk);
    std::this_thread::sleep_for(sleep_time);
    remaining -= sleep_time;
  }
  return cancelled && cancelled->load();
}

static double SampleLocalServiceTime(NodeType type, const LatencyContext& ctx,
                                     const LatencyConfig& config, SeededRng* rng) {
  const LatencyParams& params = config.Get(type);
  double raw = 0.0;
  switch (params.dist) {
    case LatencyParams::Dist::Lognormal:
      raw = rng->Lognormal(params.param1, params.param2);
      break;
    case LatencyParams::Dist::Gamma:
      raw = rng->Gamma(params.param1, params.param2);
      break;
    case LatencyParams::Dist::Linear: {
      double base = params.param1;
      double coeff = params.param2;
      if (type == NodeType::Chunk) {
        raw = base + coeff * static_cast<double>(ctx.pdf_size_est) + rng->Uniform(-5.0, 5.0);
      } else if (type == NodeType::SimilaritySearch) {
        raw = base + coeff * static_cast<double>(ctx.num_chunks_est);
      } else {
        raw = base + rng->Uniform(-2.0, 2.0);
      }
      raw = std::max(1.0, raw);
      break;
    }
  }
  if (params.tail_prob > 0.0 && rng->Bernoulli(params.tail_prob)) {
    raw *= params.tail_multiplier;
  }
  return std::max(1.0, raw);
}

void TierWorkerLoop(Tier* tier, LatencySampler* sampler, SeededRng* rng, ResultQueue* results,
                    int time_scale, std::atomic<bool>* shutdown,
                    double heavy_tail_prob, double heavy_tail_multiplier) {
  while (!shutdown || !shutdown->load()) {
    QueuedAttempt attempt;
    if (!tier->TimedDequeue(attempt, std::chrono::milliseconds(100))) continue;

    tier->AcquireTokens(attempt);

    const auto start = std::chrono::steady_clock::now();
    const TierConfig& tc = tier->config();
    LatencySample sample = sampler->Sample(attempt.latency_ctx, attempt.timeout_ms, tc.p_fail);
    if (heavy_tail_prob > 0.0 && rng && rng->Bernoulli(heavy_tail_prob)) {
      sample.service_time_ms *= heavy_tail_multiplier;
    }

    const int scaled_ms = std::max(1, static_cast<int>(sample.service_time_ms) / time_scale);
    const bool cancelled =
        CancellableSleep(std::chrono::milliseconds(scaled_ms), attempt.cancelled, 20);

    const auto end = std::chrono::steady_clock::now();
    const double duration_ms =
        std::chrono::duration<double, std::milli>(end - start).count() * time_scale;

    AttemptResult res;
    res.node_id = attempt.node_id;
    res.workflow_id = attempt.workflow_id;
    res.attempt_id = attempt.attempt_id;
    res.provider = attempt.provider;
    res.tier_id = attempt.tier_id;
    res.duration_ms = duration_ms;
    res.cost = tc.price_per_call;

    if (cancelled) {
      res.success = false;
      res.error = "cancelled";
    } else if (sample.failed) {
      res.success = false;
      res.error = "failed";
    } else if (sample.timeout) {
      res.success = false;
      res.error = "timeout";
    } else {
      res.success = true;
    }

    tier->OnAttemptFinish();
    results->Push(std::move(res));
  }
}

void LocalWorkerLoop(LocalQueue* queue, ResourceClass resource_class, LatencySampler* sampler,
                    SeededRng* rng, ResultQueue* results, const LatencyConfig& latency_config,
                    int time_scale, std::atomic<bool>* shutdown,
                    double heavy_tail_prob, double heavy_tail_multiplier) {
  (void)sampler;
  while (!shutdown || !shutdown->load()) {
    LocalTask task;
    if (!queue->TimedPop(task, std::chrono::milliseconds(100))) continue;

    double raw_ms = SampleLocalServiceTime(task.node_type, task.latency_ctx, latency_config, rng);
    if (heavy_tail_prob > 0.0 && rng && rng->Bernoulli(heavy_tail_prob)) {
      raw_ms *= heavy_tail_multiplier;
    }
    const int scaled_ms = std::max(1, static_cast<int>(raw_ms) / time_scale);
    const bool cancelled =
        CancellableSleep(std::chrono::milliseconds(scaled_ms), task.cancelled, 20);

    const double duration_ms = raw_ms;

    AttemptResult res;
    res.node_id = task.node_id;
    res.workflow_id = task.workflow_id;
    res.attempt_id = task.attempt_id;
    res.provider = "local";
    res.tier_id = static_cast<int>(resource_class);
    res.duration_ms = duration_ms;
    res.cost = 0.0;

    if (cancelled) {
      res.success = false;
      res.error = "cancelled";
    } else {
      res.success = true;
    }

    results->Push(std::move(res));
  }
}

}  // namespace sim
