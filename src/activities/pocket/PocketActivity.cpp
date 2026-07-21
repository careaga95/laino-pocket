#include "PocketActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <new>

#include "MappedInputManager.h"
#include "PocketBundleRuntime.h"
#include "PocketCard.h"
#include "PocketCardFixture.h"
#include "PocketCardRenderer.h"
#include "PocketPairingActivity.h"
#include "PocketPairingTransport.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"

namespace {

constexpr unsigned long LONG_PRESS_MS = 1000;

bool wifiConnected() { return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0); }

}  // namespace

PocketActivity::~PocketActivity() {
  if (workerContext != nullptr) {
    workerContext->cancelled.store(true, std::memory_order_release);
    workerContext->lifecycle.requestCancel();
    workerContext->releaseReference();
    workerContext = nullptr;
  }
  pocket::secureClear(&credential, sizeof(credential));
}

void PocketActivity::onEnter() {
  Activity::onEnter();
  const pocket::BundleLoadOutcome outcome =
      pocket::loadPocketBundle(cache, pocket::COMPILED_CARD_JSON, pocket::COMPILED_CARD_JSON_LENGTH, cardBundle);
  if (outcome.source == pocket::BundleSource::Cache) {
    LOG_DBG("PKT", "source=%s slot=%s generation=%llu", pocket::bundleSourceName(outcome.source),
            pocket::cacheSlotName(outcome.cacheLoad.slot),
            static_cast<unsigned long long>(outcome.cacheLoad.generation));
  } else {
    LOG_DBG("PKT", "source=%s cache=%s seed=%s", pocket::bundleSourceName(outcome.source),
            pocket::cacheResultName(outcome.cacheLoad.result), pocket::cacheResultName(outcome.seed.result));
  }
  cardSelection.setCardCount(cardBundle.cardCount);
  reloadCredential();
  requestUpdate();
}

void PocketActivity::onExit() {
  if (workerRunning && workerContext != nullptr) {
    workerContext->cancelled.store(true, std::memory_order_release);
    workerContext->lifecycle.requestCancel();
    LOG_ERR("PKT", "Activity exit requested before sync worker acknowledgement");
  }
  Activity::onExit();
}

void PocketActivity::reloadCredential() {
  pocket::secureClear(&credential, sizeof(credential));
  credentialNeedsAttention = false;
  const pocket::CredentialLoadResult loaded = credentialStore.load(credential);
  credentialPresent = loaded == pocket::CredentialLoadResult::Paired;
  if (loaded == pocket::CredentialLoadResult::CorruptRemoved) {
    LOG_INF("PKT", "Removed invalid credential blob");
  } else if (loaded == pocket::CredentialLoadResult::StorageError) {
    LOG_ERR("PKT", "Credential storage unavailable");
    syncNotice = SyncNotice::Failed;
  } else if (loaded == pocket::CredentialLoadResult::RevokedTombstone) {
    syncNotice = SyncNotice::PairingNeedsAttention;
  }
}

void PocketActivity::openHari() {
  auto activity = makeUniqueNoThrow<PocketPairingActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("PKT", "OOM: PocketPairingActivity");
    syncNotice = SyncNotice::Failed;
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult&) {
    syncNotice = SyncNotice::None;
    reloadCredential();
    requestUpdate();
  });
}

void PocketActivity::requestSync() {
  if (!canSync() || workerRunning || pendingAfterWifi) {
    if (!canSync() && !workerRunning) openHari();
    return;
  }
  if (wifiConnected()) {
    startWorker();
    return;
  }
  pendingAfterWifi = true;
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    pendingAfterWifi = false;
    syncNotice = SyncNotice::Failed;
    LOG_ERR("PKT", "OOM: WifiSelectionActivity");
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(wifi),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void PocketActivity::onWifiSelectionComplete(const bool connected) {
  pendingAfterWifi = false;
  if (connected && canSync()) {
    startWorker();
  } else {
    requestUpdate();
  }
}

void PocketActivity::workerTrampoline(void* context) { runWorker(static_cast<pocket::BundleWorkerContext*>(context)); }

void PocketActivity::runWorker(pocket::BundleWorkerContext* context) {
  {
    pocket::Esp32PocketGatewayTransport gateway;
    pocket::PairingClient client(gateway);
    char bearer[pocket::DEVICE_TOKEN_TEXT_BYTES + 1]{};
    if (!pocket::encodeBase64UrlToken(context->credential.token, sizeof(context->credential.token), bearer,
                                      sizeof(bearer))) {
      context->outcome = {pocket::PocketClientResult::InvalidResponse};
    } else {
      std::size_t jsonLength = 0;
      context->outcome = client.bundle(bearer, context->cancelled, context->json, sizeof(context->json), jsonLength);
      context->jsonLength = static_cast<uint16_t>(jsonLength);
    }
    pocket::secureClear(bearer, sizeof(bearer));
  }
  context->stackMargin = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
  context->lifecycle.release();
  context->releaseReference();
  vTaskDelete(nullptr);
}

bool PocketActivity::startWorker() {
  if (workerRunning || workerContext != nullptr || !canSync()) return false;
  // This bounded 4 KiB response belongs on transient heap: the ESP32-C3 stack is too small and steady-state
  // activity RAM should not permanently duplicate PocketBundleCache's buffer.
  workerContext = new (std::nothrow) pocket::BundleWorkerContext();
  if (workerContext == nullptr) {
    LOG_ERR("PKT", "Failed to allocate bundle sync context");
    syncNotice = SyncNotice::Failed;
    requestUpdate();
    return false;
  }
  workerContext->credential = credential;
  if (!workerContext->lifecycle.begin()) {
    workerContext->releaseReference();
    workerContext = nullptr;
    return false;
  }
  workerRunning = true;
  syncNotice = SyncNotice::Updating;
  workerContext->addReference();
  if (xTaskCreate(workerTrampoline, "PocketSync", 8192, workerContext, 1, &workerHandle) == pdPASS) {
    requestUpdate();
    return true;
  }
  workerContext->lifecycle.launchFailed();
  workerContext->releaseReference();
  workerContext->releaseReference();
  workerContext = nullptr;
  workerRunning = false;
  syncNotice = SyncNotice::Failed;
  LOG_ERR("PKT", "Failed to create bundle sync worker");
  requestUpdate();
  return false;
}

void PocketActivity::processWorkerResult() {
  if (!workerRunning || workerContext == nullptr || !workerContext->lifecycle.ownerMayFinish()) return;

  const pocket::PocketClientOutcome outcome = workerContext->outcome;
  workerStackMargin = workerContext->stackMargin;
  if (exitAfterCancellation) {
    // The activity owner has explicitly cancelled; a racing late success must not replace the visible cache.
  } else if (outcome.result == pocket::PocketClientResult::Success) {
    const pocket::CacheOutcome stored = cache.store(workerContext->json, workerContext->jsonLength);
    const pocket::CacheOutcome loaded =
        stored.result == pocket::CacheResult::Success ? cache.loadBest(cardBundle) : stored;
    if (stored.result == pocket::CacheResult::Success && loaded.result == pocket::CacheResult::Success) {
      cardSelection.setCardCount(cardBundle.cardCount);
      syncNotice = SyncNotice::Updated;
      LOG_INF("PKT", "Bundle sync stored slot=%s generation=%llu cards=%u stack_margin=%lu",
              pocket::cacheSlotName(stored.slot), static_cast<unsigned long long>(stored.generation),
              static_cast<unsigned>(cardBundle.cardCount), static_cast<unsigned long>(workerStackMargin));
    } else {
      syncNotice = SyncNotice::Failed;
      LOG_ERR("PKT", "Bundle cache update failed store=%s load=%s", pocket::cacheResultName(stored.result),
              pocket::cacheResultName(loaded.result));
    }
  } else if (outcome.result == pocket::PocketClientResult::Unauthorized ||
             outcome.result == pocket::PocketClientResult::Revoked) {
    credentialNeedsAttention = true;
    syncNotice = SyncNotice::PairingNeedsAttention;
    LOG_INF("PKT", "Bundle sync rejected result=%s http=%d", pocket::pocketClientResultName(outcome.result),
            static_cast<int>(outcome.httpStatus));
  } else if (outcome.result != pocket::PocketClientResult::Cancelled) {
    syncNotice = SyncNotice::Failed;
    LOG_ERR("PKT", "Bundle sync failed result=%s transport=%s http=%d elapsed=%lu stack_margin=%lu",
            pocket::pocketClientResultName(outcome.result), pocket::gatewayTransportResultName(outcome.transport),
            static_cast<int>(outcome.httpStatus), static_cast<unsigned long>(outcome.elapsedMs),
            static_cast<unsigned long>(workerStackMargin));
  }

  workerContext->releaseReference();
  workerContext = nullptr;
  workerHandle = nullptr;
  workerRunning = false;
  if (exitAfterCancellation) {
    exitAfterCancellation = false;
    onGoHome(HomeMenuItem::POCKET);
    return;
  }
  requestUpdate();
}

void PocketActivity::cancelWorkerAndExit() {
  if (workerContext == nullptr) return;
  exitAfterCancellation = true;
  workerContext->cancelled.store(true, std::memory_order_release);
  workerContext->lifecycle.requestCancel();
  requestUpdate();
}

void PocketActivity::loop() {
  processWorkerResult();
  if (workerRunning) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelWorkerAndExit();
    return;
  }

  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) longPressFired = false;
    return;
  }

  if (canSync() && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressFired = true;
    openHari();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::POCKET);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (canSync())
      requestSync();
    else
      openHari();
    return;
  }

  bool changed = false;
  if (mappedInput.wasReleased(MappedInputManager::Button::NavPrevious)) {
    RenderLock lock;
    changed = cardSelection.selectPrevious();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::NavNext)) {
    RenderLock lock;
    changed = cardSelection.selectNext();
  }

  if (changed) requestUpdate();
}

const char* PocketActivity::headerLabel() const {
  switch (syncNotice) {
    case SyncNotice::Updating:
      return tr(STR_POCKET_SYNCING);
    case SyncNotice::Updated:
      return tr(STR_POCKET_SYNCED);
    case SyncNotice::Failed:
      return tr(STR_POCKET_SYNC_FAILED);
    case SyncNotice::PairingNeedsAttention:
      return tr(STR_POCKET_SYNC_NEEDS_PAIRING);
    case SyncNotice::None:
      return tr(STR_LAINO_POCKET);
  }
  return tr(STR_LAINO_POCKET);
}

void PocketActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  int viewableTop = 0;
  int viewableRight = 0;
  int viewableBottom = 0;
  int viewableLeft = 0;
  renderer.getOrientedViewableTRBL(&viewableTop, &viewableRight, &viewableBottom, &viewableLeft);

  const Rect hintSafeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int safeLeft = std::max(viewableLeft, hintSafeArea.x);
  const int safeTop = std::max(viewableTop, hintSafeArea.y);
  const int safeRight = std::min(screenWidth - viewableRight, hintSafeArea.x + hintSafeArea.width);
  const int safeBottom = std::min(screenHeight - viewableBottom, hintSafeArea.y + hintSafeArea.height);

  const int headerY = std::max(safeTop, metrics.topPadding);
  const int headerWidth = std::max(0, safeRight - safeLeft);
  if (headerWidth > 0 && metrics.headerHeight > 0) {
    GUI.drawHeader(renderer, Rect{safeLeft, headerY, headerWidth, metrics.headerHeight}, headerLabel());
  }

  // getScreenSafeArea() currently reserves front hints only, so Pocket keeps side-hint clearance local.
  const int sideInset = std::max(metrics.contentSidePadding, metrics.sideButtonHintsWidth + metrics.verticalSpacing);
  const int cardX = safeLeft + sideInset;
  const int cardY = headerY + metrics.headerHeight + metrics.verticalSpacing;
  const int cardWidth = std::max(0, safeRight - safeLeft - sideInset * 2);
  const int availableCardHeight = std::max(0, safeBottom - cardY - metrics.verticalSpacing);
  const auto& card = cardBundle.cardAt(cardSelection.index());
  const int cardHeight = std::min(availableCardHeight, pocket::preferredCardHeight(renderer, card));
  pocket::drawCard(renderer, Rect{cardX, cardY, cardWidth, cardHeight}, card, cardSelection.index(),
                   cardBundle.cardCount);

  const char* previousLabel = cardSelection.canSelectPrevious() ? tr(STR_POCKET_PREVIOUS) : "";
  const char* nextLabel = cardSelection.canSelectNext() ? tr(STR_POCKET_NEXT) : "";
  const char* confirmLabel = workerRunning ? "" : canSync() ? tr(STR_POCKET_SYNC) : tr(STR_POCKET_HARI);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, previousLabel, nextLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, previousLabel, nextLabel);

  renderer.displayBuffer();
}
