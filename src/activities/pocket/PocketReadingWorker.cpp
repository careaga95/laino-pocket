#include "PocketReadingWorker.h"

#include <cassert>

namespace pocket {

void ReadingWorkerContext::addReference() {
  const uint8_t previous = references.fetch_add(1, std::memory_order_relaxed);
  assert(previous > 0 && previous < UINT8_MAX);
  (void)previous;
}

void ReadingWorkerContext::releaseReference() {
  if (references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
}

ReadingWorkerContext::~ReadingWorkerContext() {
  secureClear(&credential, sizeof(credential));
  secureClear(json, sizeof(json));
}

}  // namespace pocket
