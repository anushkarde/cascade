#include "sim/config.h"

namespace sim {

LatencyConfig::LatencyConfig() {
  // LoadPDF: lognormal + occasional cache-miss tail multiplier
  LatencyParams load_pdf;
  load_pdf.dist = LatencyParams::Dist::Lognormal;
  load_pdf.param1 = 5.0;   // log-mean
  load_pdf.param2 = 0.8;   // sigma
  load_pdf.tail_multiplier = 3.0;
  load_pdf.tail_prob = 0.1;
  by_type[NodeType::LoadPDF] = load_pdf;

  // Chunk: near-deterministic base + k*pdf_size + jitter (linear)
  LatencyParams chunk;
  chunk.dist = LatencyParams::Dist::Linear;
  chunk.param1 = 50.0;   // base_ms
  chunk.param2 = 0.5;    // coeff per pdf_size unit
  chunk.tail_multiplier = 1.0;
  chunk.tail_prob = 0.0;
  by_type[NodeType::Chunk] = chunk;

  // Embed: lognormal/gamma heavier tail
  LatencyParams embed;
  embed.dist = LatencyParams::Dist::Gamma;
  embed.param1 = 4.0;    // shape
  embed.param2 = 25.0;   // scale (mean = shape*scale)
  embed.tail_multiplier = 2.0;
  embed.tail_prob = 0.05;
  by_type[NodeType::Embed] = embed;

  // SimilaritySearch: base + k*num_chunks
  LatencyParams ss;
  ss.dist = LatencyParams::Dist::Linear;
  ss.param1 = 20.0;
  ss.param2 = 2.0;
  ss.tail_multiplier = 1.0;
  ss.tail_prob = 0.0;
  by_type[NodeType::SimilaritySearch] = ss;

  // Plan, ExtractEvidence, Aggregate, DecideNext: lognormal token-length dependent
  LatencyParams llm;
  llm.dist = LatencyParams::Dist::Lognormal;
  llm.param1 = 6.0;
  llm.param2 = 0.8;
  llm.tail_multiplier = 1.0;
  llm.tail_prob = 0.0;
  by_type[NodeType::Plan] = llm;
  by_type[NodeType::ExtractEvidence] = llm;
  by_type[NodeType::Aggregate] = llm;
  by_type[NodeType::DecideNext] = llm;
}

const LatencyParams& LatencyConfig::Get(NodeType t) const {
  auto it = by_type.find(t);
  if (it != by_type.end()) return it->second;
  static LatencyParams default_params = []() {
    LatencyParams p;
    p.dist = LatencyParams::Dist::Lognormal;
    p.param1 = 5.0;
    p.param2 = 0.8;
    return p;
  }();
  return default_params;
}

ProviderConfig::ProviderConfig() {
  // Embed tier (cheaper, slower)
  TierConfig embed_slow;
  embed_slow.provider = "embed_provider";
  embed_slow.tier_id = 0;
  embed_slow.rate_per_sec = 20.0;
  embed_slow.capacity = 50.0;
  embed_slow.concurrency_cap = 4;
  embed_slow.price_per_call = 0.0001;
  embed_slow.p_fail = 0.02;
  embed_slow.default_timeout_ms = 10'000;
  embed_slow.default_max_retries = 3;
  tiers.push_back(embed_slow);

  // Embed tier (faster, more expensive)
  TierConfig embed_fast;
  embed_fast.provider = "embed_provider";
  embed_fast.tier_id = 1;
  embed_fast.rate_per_sec = 100.0;
  embed_fast.capacity = 200.0;
  embed_fast.concurrency_cap = 8;
  embed_fast.price_per_call = 0.0005;
  embed_fast.p_fail = 0.01;
  embed_fast.default_timeout_ms = 5'000;
  embed_fast.default_max_retries = 3;
  tiers.push_back(embed_fast);

  // LLM tier (cheaper)
  TierConfig llm_slow;
  llm_slow.provider = "llm_provider";
  llm_slow.tier_id = 0;
  llm_slow.rate_per_sec = 5.0;
  llm_slow.capacity = 20.0;
  llm_slow.concurrency_cap = 2;
  llm_slow.price_per_call = 0.01;
  llm_slow.p_fail = 0.03;
  llm_slow.default_timeout_ms = 30'000;
  llm_slow.default_max_retries = 3;
  tiers.push_back(llm_slow);

  // LLM tier (faster)
  TierConfig llm_fast;
  llm_fast.provider = "llm_provider";
  llm_fast.tier_id = 1;
  llm_fast.rate_per_sec = 20.0;
  llm_fast.capacity = 50.0;
  llm_fast.concurrency_cap = 4;
  llm_fast.price_per_call = 0.05;
  llm_fast.p_fail = 0.02;
  llm_fast.default_timeout_ms = 15'000;
  llm_fast.default_max_retries = 3;
  tiers.push_back(llm_fast);
}

}  // namespace sim
