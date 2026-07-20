#include "PocketPairingActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "MappedInputManager.h"
#include "PocketPairingTransport.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

bool wifiConnected() { return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0); }

}  // namespace

const char* PocketPairingActivity::workerOperationName(const pocket::WorkerOperation operation) {
  // This helper intentionally contains state names only; pairing secrets are never passed to logging helpers.
  switch (operation) {
    case pocket::WorkerOperation::None:
      return "none";
    case pocket::WorkerOperation::Start:
      return "start";
    case pocket::WorkerOperation::Poll:
      return "poll";
    case pocket::WorkerOperation::Finalize:
      return "finalize";
    case pocket::WorkerOperation::Reject:
      return "reject";
    case pocket::WorkerOperation::Verify:
      return "verify";
    case pocket::WorkerOperation::DeleteSelf:
      return "delete_self";
  }
  return "unknown";
}

PocketPairingActivity::~PocketPairingActivity() {
  if (workerContext != nullptr) {
    workerContext->cancelled.store(true, std::memory_order_release);
    workerContext->lifecycle.requestCancel();
    workerContext->releaseReference();
    workerContext = nullptr;
  }
  clearPairingSecrets();
  clearCredentialRam();
}

void PocketPairingActivity::onEnter() {
  Activity::onEnter();
  const pocket::CredentialLoadResult loaded = credentialStore.load(credential);
  credentialPresent = loaded == pocket::CredentialLoadResult::Paired;
  if (loaded == pocket::CredentialLoadResult::Paired) {
    machine.restorePaired(false);
    if (wifiConnected()) requestOperationWithWifi(pocket::WorkerOperation::Verify);
  } else if (loaded == pocket::CredentialLoadResult::RevokedTombstone) {
    machine.restorePaired(true);
  } else {
    if (loaded == pocket::CredentialLoadResult::CorruptRemoved) {
      LOG_INF("PAIR", "Removed invalid credential blob");
    } else if (loaded == pocket::CredentialLoadResult::StorageError) {
      LOG_ERR("PAIR", "Credential storage unavailable");
      storageOperation = StorageOperation::LoadCredential;
      machine.storageFailed();
      requestUpdate();
      return;
    }
    machine.resetUnpaired();
  }
  requestUpdate();
}

void PocketPairingActivity::onExit() {
  if (workerRunning && workerContext != nullptr) {
    workerContext->cancelled.store(true, std::memory_order_release);
    workerContext->lifecycle.requestCancel();
    LOG_ERR("PAIR", "Activity exit requested before worker acknowledgement");
  }
  Activity::onExit();
}

bool PocketPairingActivity::preventAutoSleep() {
  const pocket::PairingState state = machine.state();
  return workerRunning || state == pocket::PairingState::CodeAndPolling ||
         state == pocket::PairingState::ConfirmAccount || state == pocket::PairingState::Cancelling;
}

void PocketPairingActivity::workerTrampoline(void* context) {
  runWorker(static_cast<pocket::PairingWorkerContext*>(context));
}

void PocketPairingActivity::runWorker(pocket::PairingWorkerContext* context) {
  {
    pocket::Esp32PocketGatewayTransport gateway;
    pocket::PairingClient client(gateway);
    switch (context->operation) {
      case pocket::WorkerOperation::Start:
        context->outcome =
            client.start(pocket::COMPILED_PAIRING_IDENTITY, context->cancelled, context->startResponse);
        break;
      case pocket::WorkerOperation::Poll:
        context->outcome =
            client.poll(context->startResponse.deviceCode, context->cancelled, context->pollResponse);
        break;
      case pocket::WorkerOperation::Finalize:
        context->outcome = client.finalize(context->startResponse.deviceCode, context->finalizeConfirm,
                                           context->cancelled, context->finalizeResponse);
        break;
      case pocket::WorkerOperation::Reject:
        context->outcome = client.finalize(context->startResponse.deviceCode, false, context->cancelled,
                                           context->finalizeResponse);
        break;
      case pocket::WorkerOperation::Verify:
      case pocket::WorkerOperation::DeleteSelf: {
        char bearer[pocket::DEVICE_TOKEN_TEXT_BYTES + 1]{};
        if (!pocket::encodeBase64UrlToken(context->credential.token, sizeof(context->credential.token), bearer,
                                          sizeof(bearer))) {
          context->outcome = {pocket::PocketClientResult::InvalidResponse};
        } else if (context->operation == pocket::WorkerOperation::Verify) {
          context->outcome = client.self(bearer, context->cancelled, context->selfResponse);
        } else {
          context->outcome = client.removeSelf(bearer, context->cancelled);
        }
        pocket::secureClear(bearer, sizeof(bearer));
        break;
      }
      case pocket::WorkerOperation::None:
        context->outcome = {pocket::PocketClientResult::InvalidResponse};
        break;
    }
  }
  context->stackMargin = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
  context->lifecycle.release();
  context->releaseReference();
  vTaskDelete(nullptr);
}

bool PocketPairingActivity::startWorker(const pocket::WorkerOperation operation) {
  if (workerRunning || workerContext != nullptr || operation == pocket::WorkerOperation::None) return false;
  workerContext = new (std::nothrow) pocket::PairingWorkerContext();
  if (workerContext == nullptr) {
    LOG_ERR("PAIR", "Failed to allocate network worker context");
    handleWorkerLaunchFailure(operation);
    return false;
  }
  workerContext->operation = operation;
  workerContext->finalizeConfirm = finalizeConfirm;
  workerContext->credential = credential;
  workerContext->startResponse = startResponse;
  if (!workerContext->lifecycle.begin()) {
    workerContext->releaseReference();
    workerContext = nullptr;
    return false;
  }
  workerOutcome = {};
  workerOperation = operation;
  workerRunning = true;
  workerContext->addReference();
  if (xTaskCreate(workerTrampoline, "PocketPair", 8192, workerContext, 1, &workerHandle) == pdPASS) {
    requestUpdate();
    return true;
  }
  workerContext->lifecycle.launchFailed();
  workerContext->releaseReference();
  workerContext->releaseReference();
  workerContext = nullptr;
  LOG_ERR("PAIR", "Failed to create network worker");
  handleWorkerLaunchFailure(operation);
  return false;
}

void PocketPairingActivity::handleWorkerLaunchFailure(const pocket::WorkerOperation operation) {
  workerRunning = false;
  workerHandle = nullptr;
  workerOperation = pocket::WorkerOperation::None;
  if (operation == pocket::WorkerOperation::Verify) {
    machine.verifyNetworkFailed();
  } else if (operation == pocket::WorkerOperation::DeleteSelf) {
    offerLocalOnlyUnpair = true;
    retryOperation = RetryOperation::DeleteSelf;
    machine.unpairFailed();
  } else if (operation == pocket::WorkerOperation::Reject) {
    clearPairingSecrets();
    machine.rejectFinished();
  } else {
    retryOperation = operation == pocket::WorkerOperation::Poll       ? RetryOperation::Poll
                     : operation == pocket::WorkerOperation::Finalize ? RetryOperation::Finalize
                                                              : RetryOperation::Start;
    machine.startFailed();
  }
  requestUpdate();
}

void PocketPairingActivity::requestOperationWithWifi(const pocket::WorkerOperation operation) {
  if (wifiConnected()) {
    if (operation == pocket::WorkerOperation::Start) machine.wifiReady();
    if (operation == pocket::WorkerOperation::Verify) machine.verificationStarted();
    if (operation == pocket::WorkerOperation::DeleteSelf) machine.confirmUnpair();
    startWorker(operation);
    return;
  }
  pendingAfterWifi = operation;
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    LOG_ERR("PAIR", "OOM: WifiSelectionActivity");
    if (operation == pocket::WorkerOperation::Start) machine.resetUnpaired();
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(wifi),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void PocketPairingActivity::onWifiSelectionComplete(const bool connected) {
  const pocket::WorkerOperation operation = pendingAfterWifi;
  pendingAfterWifi = pocket::WorkerOperation::None;
  if (!connected) {
    if (operation == pocket::WorkerOperation::Start) machine.resetUnpaired();
    if (operation == pocket::WorkerOperation::Verify) machine.restorePaired(false);
    requestUpdate();
    return;
  }
  if (operation == pocket::WorkerOperation::Start) machine.wifiReady();
  if (operation == pocket::WorkerOperation::Verify) machine.verificationStarted();
  if (operation == pocket::WorkerOperation::DeleteSelf) machine.confirmUnpair();
  startWorker(operation);
}

void PocketPairingActivity::beginNewPairing() {
  unpairNotice = UnpairNotice::None;
  offerLocalOnlyUnpair = false;
  if (machine.state() == pocket::PairingState::RevokedNotice) {
    if (!credentialStore.clear()) {
      storageOperation = StorageOperation::ClearCredential;
      machine.credentialSaved(false);
      requestUpdate();
      return;
    }
    clearCredentialRam();
    credentialPresent = false;
  }
  clearPairingSecrets();
  machine.beginPairing();
  retryOperation = RetryOperation::Start;
  requestOperationWithWifi(pocket::WorkerOperation::Start);
}

void PocketPairingActivity::saveFinalizedCredential() {
  pocket::Credential pending{};
  pending.state = pocket::CredentialState::Paired;
  const bool valid =
      pocket::parseUuid(finalizeResponse.deviceId, std::strlen(finalizeResponse.deviceId), pending.deviceUuid,
                        sizeof(pending.deviceUuid)) &&
      pocket::decodeBase64UrlToken(finalizeResponse.accessToken, std::strlen(finalizeResponse.accessToken),
                                   pending.token, sizeof(pending.token));
  pocket::secureClear(finalizeResponse.accessToken, sizeof(finalizeResponse.accessToken));
  pocket::secureClear(startResponse.deviceCode, sizeof(startResponse.deviceCode));
  pocket::secureClear(startResponse.userCode, sizeof(startResponse.userCode));
  if (!valid) {
    pocket::secureClear(&pending, sizeof(pending));
    retryOperation = RetryOperation::Start;
    machine.finalizeFailed(false);
    return;
  }
  const bool saved = credentialStore.save(pending);
  clearCredentialRam();
  credential = pending;
  credentialPresent = true;
  pocket::secureClear(&pending, sizeof(pending));
  machine.credentialSaved(saved);
  storageOperation = saved ? StorageOperation::None : StorageOperation::SaveCredential;
  if (saved) requestOperationWithWifi(pocket::WorkerOperation::Verify);
}

void PocketPairingActivity::handleRevocation() {
  const bool tombstoneSaved = credentialStore.saveRevokedTombstone(credential, 1);
  bool keyCleared = false;
  if (!tombstoneSaved) {
    LOG_ERR("PAIR", "Failed to write revocation tombstone; clearing credential key");
    keyCleared = credentialStore.clear();
  }
  revocationPersistence.recordInitial(tombstoneSaved, keyCleared);

  // The remote response already invalidated this bearer. Never retain it in RAM,
  // including while persistence recovery is waiting for a retry.
  pocket::secureClear(credential.token, sizeof(credential.token));
  credential.state = pocket::CredentialState::RevokedTombstone;
  credential.revocationReason = 1;
  credentialPresent = false;
  if (revocationPersistence.resolved()) {
    storageOperation = StorageOperation::None;
    machine.verifyRevoked();
  } else {
    LOG_ERR("PAIR", "Failed to persist or clear revoked credential");
    storageOperation = StorageOperation::ClearRevokedCredential;
    machine.storageFailed();
  }
}

void PocketPairingActivity::processWorkerResult() {
  if (workerContext == nullptr || !workerContext->lifecycle.ownerMayFinish()) return;
  workerOutcome = workerContext->outcome;
  startResponse = workerContext->startResponse;
  pollResponse = workerContext->pollResponse;
  finalizeResponse = workerContext->finalizeResponse;
  selfResponse = workerContext->selfResponse;
  workerStackMargin = workerContext->stackMargin;
  workerContext->releaseReference();
  workerContext = nullptr;
  workerRunning = false;
  workerHandle = nullptr;
  const pocket::WorkerOperation completed = workerOperation;
  workerOperation = pocket::WorkerOperation::None;

  if (workerOutcome.localValidation != pocket::LocalValidationField::None) {
    LOG_INF("PAIR", "local_validation=%s",
            pocket::localValidationFieldName(workerOutcome.localValidation));
  } else {
    LOG_INF("PAIR", "op=%s result=%s http=%d transport=%s elapsed=%lu error=%ld stack_free=%lu",
            workerOperationName(completed), pocket::pocketClientResultName(workerOutcome.result),
            workerOutcome.httpStatus, pocket::gatewayTransportResultName(workerOutcome.transport),
            static_cast<unsigned long>(workerOutcome.elapsedMs), static_cast<long>(workerOutcome.transportError),
            static_cast<unsigned long>(workerStackMargin));
  }
  if (workerStackMargin < 1024) LOG_ERR("PAIR", "Network worker stack margin below protocol minimum");

  if (exitAfterCancellation) {
    exitAfterCancellation = false;
    if (completed == pocket::WorkerOperation::Start || completed == pocket::WorkerOperation::Poll ||
        completed == pocket::WorkerOperation::Finalize || completed == pocket::WorkerOperation::Reject) {
      clearPairingSecrets();
    }
    machine.cancellationFinished();
    finish();
    return;
  }

  switch (completed) {
    case pocket::WorkerOperation::Start:
      if (workerOutcome.result == pocket::PocketClientResult::Success) {
        machine.startSucceeded(startResponse, millis());
        retryOperation = RetryOperation::Poll;
      } else {
        retryOperation = RetryOperation::Start;
        machine.startFailed();
      }
      break;
    case pocket::WorkerOperation::Poll:
      if (workerOutcome.result == pocket::PocketClientResult::Pending) {
        machine.pollPending(millis());
      } else if (workerOutcome.result == pocket::PocketClientResult::Claimed) {
        machine.pollClaimed();
      } else if (workerOutcome.result == pocket::PocketClientResult::RateLimited) {
        machine.pollRateLimited(workerOutcome.retryAfter, millis());
      } else if (workerOutcome.result == pocket::PocketClientResult::ExpiredOrUnknown) {
        clearPairingSecrets();
        machine.expire();
      } else if (workerOutcome.result == pocket::PocketClientResult::TransportFailure) {
        machine.pollTransportFailed(millis());
        retryOperation = RetryOperation::Poll;
      } else {
        retryOperation = RetryOperation::Poll;
        machine.startFailed();
      }
      break;
    case pocket::WorkerOperation::Finalize:
      if (workerOutcome.result == pocket::PocketClientResult::Success) {
        machine.finalizeSucceeded();
        saveFinalizedCredential();
      } else if (workerOutcome.result == pocket::PocketClientResult::Consumed) {
        clearPairingSecrets();
        machine.finalizeConsumed();
      } else if (workerOutcome.result == pocket::PocketClientResult::ExpiredOrUnknown) {
        clearPairingSecrets();
        machine.finalizeFailed(true);
      } else {
        retryOperation = RetryOperation::Finalize;
        machine.finalizeFailed(false);
      }
      break;
    case pocket::WorkerOperation::Reject:
      clearPairingSecrets();
      machine.rejectFinished();
      break;
    case pocket::WorkerOperation::Verify:
      if (workerOutcome.result == pocket::PocketClientResult::Success) {
        uint8_t returnedUuid[pocket::DEVICE_UUID_BYTES]{};
        if (pocket::parseUuid(selfResponse.deviceId, std::strlen(selfResponse.deviceId), returnedUuid,
                              sizeof(returnedUuid)) &&
            std::memcmp(returnedUuid, credential.deviceUuid, sizeof(returnedUuid)) == 0) {
          machine.verifySucceeded();
        } else {
          machine.verifyNetworkFailed();
        }
      } else if (workerOutcome.result == pocket::PocketClientResult::Revoked) {
        handleRevocation();
      } else {
        machine.verifyNetworkFailed();
      }
      break;
    case pocket::WorkerOperation::DeleteSelf: {
      const pocket::UnpairDisposition disposition = pocket::classifyUnpairResult(workerOutcome.result);
      if (disposition != pocket::UnpairDisposition::OfferLocalOnly) {
        if (credentialStore.clear()) {
          clearCredentialRam();
          credentialPresent = false;
          unpairNotice = disposition == pocket::UnpairDisposition::RemoteConfirmed
                             ? UnpairNotice::RemoteConfirmed
                             : UnpairNotice::TokenInvalidOrRevoked;
          machine.unpairSucceeded();
        } else {
          storageOperation = StorageOperation::ClearCredential;
          machine.credentialSaved(false);
        }
      } else {
        offerLocalOnlyUnpair = true;
        retryOperation = RetryOperation::DeleteSelf;
        machine.unpairFailed();
      }
      break;
    }
    case pocket::WorkerOperation::None:
      break;
  }
  requestUpdate();
}

void PocketPairingActivity::clearPairingSecrets() {
  pocket::secureClear(&startResponse, sizeof(startResponse));
  pocket::secureClear(&pollResponse, sizeof(pollResponse));
  pocket::secureClear(&finalizeResponse, sizeof(finalizeResponse));
}

void PocketPairingActivity::clearCredentialRam() { pocket::secureClear(&credential, sizeof(credential)); }

void PocketPairingActivity::handleBack() {
  if (workerRunning) {
    exitAfterCancellation = true;
    workerContext->cancelled.store(true, std::memory_order_release);
    workerContext->lifecycle.requestCancel();
    machine.cancellationStarted();
    requestUpdate();
    return;
  }
  switch (machine.state()) {
    case pocket::PairingState::CodeAndPolling:
      clearPairingSecrets();
      machine.resetUnpaired();
      finish();
      break;
    case pocket::PairingState::ConfirmAccount:
      machine.rejectAccount();
      startWorker(pocket::WorkerOperation::Reject);
      break;
    case pocket::PairingState::UnpairConfirm:
      machine.cancelUnpair();
      requestUpdate();
      break;
    default:
      finish();
      break;
  }
}

void PocketPairingActivity::handleConfirm() {
  switch (machine.state()) {
    case pocket::PairingState::Unpaired:
    case pocket::PairingState::Expired:
    case pocket::PairingState::RestartRequired:
    case pocket::PairingState::RevokedNotice:
      beginNewPairing();
      break;
    case pocket::PairingState::ConfirmAccount:
      finalizeConfirm = true;
      machine.confirmAccount();
      startWorker(pocket::WorkerOperation::Finalize);
      break;
    case pocket::PairingState::Paired:
      machine.beginUnpair();
      requestUpdate();
      break;
    case pocket::PairingState::PairedUnverified:
      requestOperationWithWifi(pocket::WorkerOperation::Verify);
      break;
    case pocket::PairingState::UnpairConfirm:
      offerLocalOnlyUnpair = false;
      requestOperationWithWifi(pocket::WorkerOperation::DeleteSelf);
      break;
    case pocket::PairingState::ErrorRetry:
      retryLastOperation();
      break;
    case pocket::PairingState::StorageError:
      if (storageOperation == StorageOperation::LoadCredential) {
        clearCredentialRam();
        const pocket::CredentialLoadResult loaded = credentialStore.load(credential);
        credentialPresent = loaded == pocket::CredentialLoadResult::Paired;
        if (loaded == pocket::CredentialLoadResult::Paired) {
          storageOperation = StorageOperation::None;
          machine.restorePaired(false);
        } else if (loaded == pocket::CredentialLoadResult::RevokedTombstone) {
          storageOperation = StorageOperation::None;
          machine.restorePaired(true);
        } else if (loaded == pocket::CredentialLoadResult::Absent ||
                   loaded == pocket::CredentialLoadResult::CorruptRemoved) {
          storageOperation = StorageOperation::None;
          machine.resetUnpaired();
        }
      } else if (storageOperation == StorageOperation::SaveCredential && credentialPresent &&
          credentialStore.save(credential)) {
        storageOperation = StorageOperation::None;
        machine.credentialSaved(true);
        requestOperationWithWifi(pocket::WorkerOperation::Verify);
      } else if (storageOperation == StorageOperation::ClearCredential && credentialStore.clear()) {
        storageOperation = StorageOperation::None;
        clearCredentialRam();
        credentialPresent = false;
        machine.unpairSucceeded();
      } else if (storageOperation == StorageOperation::ClearRevokedCredential) {
        const bool cleared = credentialStore.clear();
        revocationPersistence.recordClearRetry(cleared);
        // Keep enforcing the no-bearer invariant even if a future refactor adds
        // fields to a retry path before this point.
        pocket::secureClear(credential.token, sizeof(credential.token));
        if (revocationPersistence.resolved()) {
          storageOperation = StorageOperation::None;
          machine.verifyRevoked();
        }
      }
      requestUpdate();
      break;
    default:
      break;
  }
}

void PocketPairingActivity::retryLastOperation() {
  switch (retryOperation) {
    case RetryOperation::Start:
      beginNewPairing();
      break;
    case RetryOperation::Poll:
      machine.retryPolling(millis());
      requestUpdate();
      break;
    case RetryOperation::Finalize:
      machine.confirmAccount();
      startWorker(pocket::WorkerOperation::Finalize);
      break;
    case RetryOperation::DeleteSelf:
      offerLocalOnlyUnpair = false;
      requestOperationWithWifi(pocket::WorkerOperation::DeleteSelf);
      break;
    case RetryOperation::None:
      beginNewPairing();
      break;
  }
}

void PocketPairingActivity::loop() {
  if (workerRunning && workerContext != nullptr && workerContext->lifecycle.ownerMayFinish()) {
    processWorkerResult();
    return;
  }

  if (machine.state() == pocket::PairingState::CodeAndPolling && !workerRunning) {
    if (machine.expired(millis())) {
      clearPairingSecrets();
      machine.expire();
      requestUpdate();
    } else if (machine.pollDue(millis())) {
      machine.pollStarted(millis());
      startWorker(pocket::WorkerOperation::Poll);
    } else {
      const uint16_t seconds = machine.secondsRemaining(millis());
      if (seconds != lastRenderedSeconds) {
        lastRenderedSeconds = seconds;
        requestUpdate();
      }
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    handleBack();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handleConfirm();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::NavNext) &&
      machine.state() == pocket::PairingState::PairedUnverified) {
    machine.beginUnpair();
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::NavNext) &&
             machine.state() == pocket::PairingState::ErrorRetry && offerLocalOnlyUnpair) {
    unpairNotice = UnpairNotice::LocalOnly;
    if (credentialStore.clear()) {
      offerLocalOnlyUnpair = false;
      clearCredentialRam();
      credentialPresent = false;
      machine.unpairSucceeded();
    } else {
      storageOperation = StorageOperation::ClearCredential;
      machine.credentialSaved(false);
    }
    requestUpdate();
  }
}

void PocketPairingActivity::drawStateBody(const int midY) {
  char line[96];
  switch (machine.state()) {
    case pocket::PairingState::Unpaired:
      if (unpairNotice == UnpairNotice::RemoteConfirmed) {
        renderer.drawCenteredText(UI_12_FONT_ID, midY - 15, tr(STR_POCKET_UNPAIR_REMOTE_DONE), true,
                                  EpdFontFamily::BOLD);
      } else if (unpairNotice == UnpairNotice::TokenInvalidOrRevoked) {
        renderer.drawCenteredText(UI_12_FONT_ID, midY - 15, tr(STR_POCKET_UNPAIR_TOKEN_INVALID), true,
                                  EpdFontFamily::BOLD);
      } else if (unpairNotice == UnpairNotice::LocalOnly) {
        renderer.drawCenteredText(UI_12_FONT_ID, midY - 15, tr(STR_POCKET_UNPAIR_LOCAL_ONLY_DONE), true,
                                  EpdFontFamily::BOLD);
      } else {
        renderer.drawCenteredText(UI_12_FONT_ID, midY - 15, tr(STR_POCKET_NOT_PAIRED), true,
                                  EpdFontFamily::BOLD);
      }
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 20, tr(STR_POCKET_PAIR_DEVICE));
      break;
    case pocket::PairingState::WifiCheck:
    case pocket::PairingState::Starting:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_POCKET_PAIRING_STARTING));
      break;
    case pocket::PairingState::CodeAndPolling: {
      char displayCode[10];
      std::snprintf(displayCode, sizeof(displayCode), "%.4s-%.4s", startResponse.userCode, startResponse.userCode + 4);
      renderer.drawCenteredText(UI_10_FONT_ID, midY - 85, tr(STR_POCKET_ENTER_CODE));
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 45, displayCode, true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY, pocket::POCKET_CLAIM_URL, true);
      const uint16_t seconds = machine.secondsRemaining(millis());
      std::snprintf(line, sizeof(line), tr(STR_POCKET_EXPIRES_FORMAT), seconds / 60, seconds % 60);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 45, line);
      break;
    }
    case pocket::PairingState::ConfirmAccount:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 50, tr(STR_POCKET_CLAIMED), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(
          UI_10_FONT_ID, midY - 10,
          pollResponse.accountHint[0] == '\0' ? tr(STR_POCKET_YOUR_HARI_ACCOUNT) : pollResponse.accountHint, true);
      renderer.drawCenteredText(UI_12_FONT_ID, midY + 40, tr(STR_POCKET_LINK_QUESTION), true);
      break;
    case pocket::PairingState::Rejecting:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_POCKET_REJECTING));
      break;
    case pocket::PairingState::Finalizing:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_POCKET_FINALIZING));
      break;
    case pocket::PairingState::SavingNvs:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_POCKET_SAVING));
      break;
    case pocket::PairingState::Verifying:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_POCKET_VERIFYING));
      break;
    case pocket::PairingState::Paired:
    case pocket::PairingState::PairedUnverified: {
      renderer.drawCenteredText(
          UI_12_FONT_ID, midY - 35,
          machine.state() == pocket::PairingState::Paired ? tr(STR_POCKET_PAIRED) : tr(STR_POCKET_PAIRED_UNVERIFIED),
          true, EpdFontFamily::BOLD);
      char prefix[9];
      pocket::formatUuidPrefix(credential.deviceUuid, prefix, sizeof(prefix));
      std::snprintf(line, sizeof(line), "ID: %s", prefix);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, line);
      break;
    }
    case pocket::PairingState::UnpairConfirm:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 55, tr(STR_POCKET_UNPAIR_QUESTION), true, EpdFontFamily::BOLD);
      {
        std::string warning =
            wifiConnected() ? tr(STR_POCKET_UNPAIR_ONLINE_WARNING) : tr(STR_POCKET_UNPAIR_OFFLINE_WARNING);
        std::replace(warning.begin(), warning.end(), '\n', ' ');
        const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
        const auto lines = renderer.wrappedText(UI_10_FONT_ID, warning.c_str(), renderer.getScreenWidth() - 40, 3);
        int warningY = midY - (static_cast<int>(lines.size()) - 1) * lineHeight / 2;
        for (const auto& wrappedLine : lines) {
          renderer.drawCenteredText(UI_10_FONT_ID, warningY, wrappedLine.c_str(), true);
          warningY += lineHeight;
        }
      }
      break;
    case pocket::PairingState::Unpairing:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_POCKET_UNPAIRING));
      break;
    case pocket::PairingState::Cancelling:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_POCKET_CANCELLING));
      break;
    case pocket::PairingState::Cancelled:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_CANCEL));
      break;
    case pocket::PairingState::Expired:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_POCKET_EXPIRED), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 20, tr(STR_POCKET_PAIR_AGAIN));
      break;
    case pocket::PairingState::ErrorRetry:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_POCKET_SERVICE_ERROR), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 20,
                                offerLocalOnlyUnpair ? tr(STR_POCKET_UNPAIR_LOCAL_ONLY_WARNING)
                                                     : tr(STR_POCKET_RETRY_HINT));
      break;
    case pocket::PairingState::RestartRequired:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 25, tr(STR_POCKET_RESTART_REQUIRED), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 25, tr(STR_POCKET_PAIR_AGAIN));
      break;
    case pocket::PairingState::StorageError:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_POCKET_STORAGE_ERROR), true, EpdFontFamily::BOLD);
      break;
    case pocket::PairingState::RevokedNotice:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 25, tr(STR_POCKET_DEVICE_REVOKED), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 25, tr(STR_POCKET_PAIR_AGAIN));
      break;
  }
}

void PocketPairingActivity::drawHints() {
  const pocket::PairingState state = machine.state();
  const char* primary = "";
  const char* next = "";
  if (state == pocket::PairingState::Unpaired || state == pocket::PairingState::Expired ||
      state == pocket::PairingState::RestartRequired || state == pocket::PairingState::RevokedNotice) {
    primary = tr(STR_POCKET_PAIR_DEVICE);
  } else if (state == pocket::PairingState::ConfirmAccount || state == pocket::PairingState::UnpairConfirm) {
    primary = tr(STR_CONFIRM);
  } else if (state == pocket::PairingState::Paired) {
    primary = tr(STR_POCKET_UNPAIR);
  } else if (state == pocket::PairingState::PairedUnverified) {
    primary = tr(STR_RETRY);
    next = tr(STR_POCKET_UNPAIR);
  } else if (state == pocket::PairingState::ErrorRetry || state == pocket::PairingState::StorageError) {
    primary = tr(STR_RETRY);
    if (state == pocket::PairingState::ErrorRetry && offerLocalOnlyUnpair) next = tr(STR_POCKET_DELETE_LOCAL);
  }
  if (state != pocket::PairingState::Starting && state != pocket::PairingState::Finalizing &&
      state != pocket::PairingState::Rejecting && state != pocket::PairingState::SavingNvs &&
      state != pocket::PairingState::Verifying && state != pocket::PairingState::Unpairing &&
      state != pocket::PairingState::Cancelling) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), primary, "", next);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}

void PocketPairingActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_POCKET_HARI));
  drawStateBody(height / 2);
  drawHints();
  renderer.displayBuffer();
}
