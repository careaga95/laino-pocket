#pragma once

#include <cstddef>
#include <cstdint>

#include "PocketBundleCache.h"
#include "PocketCardParser.h"

namespace pocket {

enum class BundleSource : uint8_t { Cache, CompiledFixture, Fallback };

struct BundleLoadOutcome {
  BundleSource source = BundleSource::Fallback;
  CacheOutcome cacheLoad;
  CacheOutcome seed;
  ParseResult fixtureParse = ParseResult::EmptyInput;
};

BundleLoadOutcome loadPocketBundle(PocketBundleCache& cache, const char* compiledJson, size_t compiledJsonLength,
                                   CardBundle& destination);
const char* bundleSourceName(BundleSource source);

}  // namespace pocket
