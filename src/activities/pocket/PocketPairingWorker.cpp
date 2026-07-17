#include "PocketPairingWorker.h"

#include <cassert>

namespace pocket {

bool WorkerLifecycle::begin() {
  WorkerPhase expected = WorkerPhase::Idle;
  return current.compare_exchange_strong(expected, WorkerPhase::Running, std::memory_order_acq_rel);
}

void WorkerLifecycle::launchFailed() { current.store(WorkerPhase::StartFailed, std::memory_order_release); }

void WorkerLifecycle::requestCancel() {
  WorkerPhase expected = WorkerPhase::Running;
  current.compare_exchange_strong(expected, WorkerPhase::Cancelling, std::memory_order_acq_rel);
}

void WorkerLifecycle::release() { current.store(WorkerPhase::Released, std::memory_order_release); }

bool WorkerLifecycle::ownerMayFinish() const {
  const WorkerPhase value = phase();
  return value == WorkerPhase::Idle || value == WorkerPhase::Released || value == WorkerPhase::StartFailed;
}

UnpairDisposition classifyUnpairResult(const PocketClientResult result) {
  if (result == PocketClientResult::Success) return UnpairDisposition::RemoteConfirmed;
  if (result == PocketClientResult::Unauthorized || result == PocketClientResult::Revoked) {
    return UnpairDisposition::TokenInvalidOrRevoked;
  }
  return UnpairDisposition::OfferLocalOnly;
}

void PairingWorkerContext::addReference() {
  const uint8_t previous = references.fetch_add(1, std::memory_order_relaxed);
  assert(previous > 0 && previous < UINT8_MAX);
  (void)previous;
}

void PairingWorkerContext::releaseReference() {
  if (references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
}

void PairingWorkerContext::clearSecrets() {
  secureClear(&credential, sizeof(credential));
  secureClear(&startResponse, sizeof(startResponse));
  secureClear(&pollResponse, sizeof(pollResponse));
  secureClear(&finalizeResponse, sizeof(finalizeResponse));
  secureClear(&selfResponse, sizeof(selfResponse));
}

PairingWorkerContext::~PairingWorkerContext() { clearSecrets(); }

}  // namespace pocket
