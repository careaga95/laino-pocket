#include "PocketBundleWorker.h"

#include <cassert>

namespace pocket {

void BundleWorkerContext::addReference() {
  const uint8_t previous = references.fetch_add(1, std::memory_order_relaxed);
  assert(previous > 0 && previous < UINT8_MAX);
  (void)previous;
}

void BundleWorkerContext::releaseReference() {
  if (references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
}

BundleWorkerContext::~BundleWorkerContext() {
  secureClear(&credential, sizeof(credential));
  secureClear(json, sizeof(json));
}

}  // namespace pocket
