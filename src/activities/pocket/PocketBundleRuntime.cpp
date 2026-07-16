#include "PocketBundleRuntime.h"

namespace pocket {

BundleLoadOutcome loadPocketBundle(PocketBundleCache& cache, const char* compiledJson, const size_t compiledJsonLength,
                                   CardBundle& destination) {
  BundleLoadOutcome outcome;
  outcome.cacheLoad = cache.loadBest(destination);
  if (outcome.cacheLoad.result == CacheResult::Success) {
    outcome.source = BundleSource::Cache;
    return outcome;
  }

  outcome.fixtureParse = parseCardBundle(compiledJson, compiledJsonLength, destination);
  if (outcome.fixtureParse == ParseResult::Success) {
    outcome.source = BundleSource::CompiledFixture;
    outcome.seed = cache.store(compiledJson, compiledJsonLength);
    return outcome;
  }

  loadFallbackCardBundle(destination);
  outcome.source = BundleSource::Fallback;
  return outcome;
}

const char* bundleSourceName(const BundleSource source) {
  if (source == BundleSource::Cache) return "cache";
  if (source == BundleSource::CompiledFixture) return "fixture";
  return "fallback";
}

}  // namespace pocket
