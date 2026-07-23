#include "PocketActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <new>
#include <string>

#include "MappedInputManager.h"
#include "PocketPairingActivity.h"
#include "PocketPairingTransport.h"
#include "PocketSnapshotParser.h"
#include "PocketText.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

bool wifiConnected() { return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0); }

uint64_t unixEpoch(const ClockDateTime& value) {
  auto leap = [](const uint16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; };
  constexpr uint8_t monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  uint64_t days = 0;
  for (uint16_t year = 1970; year < value.year; ++year) days += leap(year) ? 366 : 365;
  for (uint8_t month = 1; month < value.month; ++month) {
    days += monthDays[month - 1] + (month == 2 && leap(value.year) ? 1 : 0);
  }
  days += value.day - 1;
  return days * 86400ULL + value.hour * 3600ULL + value.minute * 60ULL + value.second;
}

void drawFitted(const GfxRenderer& renderer, const int font, const int x, const int y, const char* value,
                const int width, const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  char buffer[160];
  const char* fitted = pocket::truncateToWidth(
      value, width, buffer, [&](const char* candidate) { return renderer.getTextWidth(font, candidate, style); });
  renderer.drawText(font, x, y, fitted, true, style);
}

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
  const pocket::CacheOutcome outcome = cache.loadBest(snapshot);
  if (outcome.result == pocket::CacheResult::Success) {
    LOG_DBG("PKT", "snapshot slot=%s generation=%llu items=%u", pocket::cacheSlotName(outcome.slot),
            static_cast<unsigned long long>(outcome.generation), static_cast<unsigned>(snapshot.itemCount));
  } else {
    pocket::loadEmptySnapshot(snapshot);
    LOG_INF("PKT", "No valid v2 snapshot cache result=%s", pocket::cacheResultName(outcome.result));
  }
  view = View::Today;
  todaySelection = 0;
  sectionIndex = 0;
  itemIndex = 0;
  initialRenderComplete = false;
  reloadCredential();
  autoSyncPending = canSync() && wifiConnected() && refreshDue();
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
    syncNotice = SyncNotice::CacheFailure;
  } else if (loaded == pocket::CredentialLoadResult::RevokedTombstone) {
    syncNotice = SyncNotice::PairingNeedsAttention;
  }
}

void PocketActivity::openHari() {
  auto activity = makeUniqueNoThrow<PocketPairingActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("PKT", "OOM: PocketPairingActivity");
    syncNotice = SyncNotice::LowMemory;
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
    syncNotice = SyncNotice::LowMemory;
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
    if (canSync()) syncNotice = SyncNotice::NoWifi;
    requestUpdate();
  }
}

void PocketActivity::workerTrampoline(void* context) { runWorker(static_cast<pocket::BundleWorkerContext*>(context)); }

void PocketActivity::runWorker(pocket::BundleWorkerContext* context) {
  context->freeHeapBefore = ESP.getFreeHeap();
  {
    pocket::Esp32PocketGatewayTransport gateway;
    pocket::PairingClient client(gateway);
    char bearer[pocket::DEVICE_TOKEN_TEXT_BYTES + 1]{};
    if (!pocket::encodeBase64UrlToken(context->credential.token, sizeof(context->credential.token), bearer,
                                      sizeof(bearer))) {
      context->outcome = {pocket::PocketClientResult::InvalidResponse};
    } else {
      std::size_t jsonLength = 0;
      context->outcome = client.snapshot(bearer, context->cancelled, context->json, sizeof(context->json), jsonLength);
      context->jsonLength = static_cast<uint16_t>(jsonLength);
    }
    pocket::secureClear(bearer, sizeof(bearer));
  }
  context->stackMargin = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
  context->freeHeapAfter = ESP.getFreeHeap();
  context->minimumFreeHeap = ESP.getMinFreeHeap();
  context->lifecycle.release();
  context->releaseReference();
  vTaskDelete(nullptr);
}

bool PocketActivity::startWorker() {
  if (workerRunning || workerContext != nullptr || !canSync()) return false;
  // The verified snapshot remains durable on SD. Release its expanded in-memory representation before
  // allocating the network response and TLS task so the ESP32-C3 keeps the TLS safety margin.
  pocket::loadEmptySnapshot(snapshot);
  workerContext = new (std::nothrow) pocket::BundleWorkerContext();
  if (workerContext == nullptr) {
    LOG_ERR("PKT", "Failed to allocate bundle sync context");
    restoreCachedSnapshot();
    syncNotice = SyncNotice::LowMemory;
    requestUpdate();
    return false;
  }
  workerContext->credential = credential;
  if (!workerContext->lifecycle.begin()) {
    workerContext->releaseReference();
    workerContext = nullptr;
    restoreCachedSnapshot();
    syncNotice = SyncNotice::LowMemory;
    requestUpdate();
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
  restoreCachedSnapshot();
  syncNotice = SyncNotice::LowMemory;
  LOG_ERR("PKT", "Failed to create bundle sync worker");
  requestUpdate();
  return false;
}

void PocketActivity::processWorkerResult() {
  if (!workerRunning || workerContext == nullptr || !workerContext->lifecycle.ownerMayFinish()) return;

  const pocket::PocketClientOutcome outcome = workerContext->outcome;
  workerStackMargin = workerContext->stackMargin;
  const uint32_t freeHeapBefore = workerContext->freeHeapBefore;
  const uint32_t freeHeapAfter = workerContext->freeHeapAfter;
  const uint32_t minimumFreeHeap = workerContext->minimumFreeHeap;
  if (exitAfterCancellation) {
    workerContext->releaseReference();
    workerContext = nullptr;
    workerHandle = nullptr;
    workerRunning = false;
    exitAfterCancellation = false;
    onGoHome(HomeMenuItem::POCKET);
    return;
  } else if (outcome.result == pocket::PocketClientResult::Success) {
    const pocket::CacheOutcome stored = cache.store(workerContext->json, workerContext->jsonLength);
    const uint16_t jsonLength = workerContext->jsonLength;
    workerContext->releaseReference();
    workerContext = nullptr;
    workerHandle = nullptr;
    workerRunning = false;
    const pocket::CacheOutcome loaded = stored.result == pocket::CacheResult::Success
                                            ? cache.loadBest(snapshot)
                                            : pocket::CacheOutcome{stored.result, stored.slot, stored.generation};
    if (stored.result == pocket::CacheResult::Success && loaded.result == pocket::CacheResult::Success) {
      view = View::Today;
      todaySelection = 0;
      sectionIndex = 0;
      itemIndex = 0;
      syncNotice = SyncNotice::Updated;
      LOG_INF("PKT",
              "Snapshot sync stored slot=%s generation=%llu items=%u bytes=%u stack_margin=%lu heap=%lu/%lu min=%lu",
              pocket::cacheSlotName(stored.slot), static_cast<unsigned long long>(stored.generation),
              static_cast<unsigned>(snapshot.itemCount), static_cast<unsigned>(jsonLength),
              static_cast<unsigned long>(workerStackMargin), static_cast<unsigned long>(freeHeapBefore),
              static_cast<unsigned long>(freeHeapAfter), static_cast<unsigned long>(minimumFreeHeap));
    } else {
      restoreCachedSnapshot();
      syncNotice = SyncNotice::CacheFailure;
      LOG_ERR("PKT", "Bundle cache update failed store=%s load=%s", pocket::cacheResultName(stored.result),
              pocket::cacheResultName(loaded.result));
    }
  } else {
    workerContext->releaseReference();
    workerContext = nullptr;
    workerHandle = nullptr;
    workerRunning = false;
    restoreCachedSnapshot();
    if (outcome.result == pocket::PocketClientResult::Unauthorized ||
        outcome.result == pocket::PocketClientResult::Revoked) {
      credentialNeedsAttention = true;
      syncNotice = SyncNotice::PairingNeedsAttention;
      LOG_INF("PKT", "Bundle sync rejected result=%s http=%d", pocket::pocketClientResultName(outcome.result),
              static_cast<int>(outcome.httpStatus));
    } else if (outcome.result == pocket::PocketClientResult::Cancelled) {
      syncNotice = SyncNotice::None;
    } else {
      if (outcome.transport == pocket::GatewayTransportResult::LowMemory)
        syncNotice = SyncNotice::LowMemory;
      else if (outcome.transport == pocket::GatewayTransportResult::NoWifi)
        syncNotice = SyncNotice::NoWifi;
      else if (outcome.result == pocket::PocketClientResult::InvalidResponse)
        syncNotice = SyncNotice::InvalidData;
      else
        syncNotice = SyncNotice::ServiceUnavailable;
      LOG_ERR("PKT",
              "Bundle sync failed result=%s transport=%s http=%d elapsed=%lu stack_margin=%lu heap=%lu/%lu min=%lu",
              pocket::pocketClientResultName(outcome.result), pocket::gatewayTransportResultName(outcome.transport),
              static_cast<int>(outcome.httpStatus), static_cast<unsigned long>(outcome.elapsedMs),
              static_cast<unsigned long>(workerStackMargin), static_cast<unsigned long>(freeHeapBefore),
              static_cast<unsigned long>(freeHeapAfter), static_cast<unsigned long>(minimumFreeHeap));
    }
  }
  requestUpdate();
}

void PocketActivity::restoreCachedSnapshot() {
  const pocket::CacheOutcome restored = cache.loadBest(snapshot);
  if (restored.result != pocket::CacheResult::Success) pocket::loadEmptySnapshot(snapshot);
}

bool PocketActivity::refreshDue() const {
  if (snapshot.fixture || snapshot.refreshAfterEpoch == 0) return true;
  ClockDateTime current;
  const auto result = halClock.getDateTimeUtc(current);
  return (result == HalClock::ReadResult::Fresh || result == HalClock::ReadResult::Cached) &&
         unixEpoch(current) >= snapshot.refreshAfterEpoch;
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

  if (autoSyncPending && initialRenderComplete) {
    autoSyncPending = false;
    startWorker();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    goBack();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!canSync())
      openHari();
    else
      openSelection();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    goToday();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (canSync())
      requestSync();
    else
      openHari();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    RenderLock lock;
    moveSelection(-1);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    RenderLock lock;
    moveSelection(1);
  } else {
    return;
  }
  requestUpdate();
}

const pocket::SnapshotSection* PocketActivity::selectedSection() const {
  return snapshot.sectionAt(view == View::Today ? todaySelection : sectionIndex);
}

const pocket::SnapshotItem* PocketActivity::selectedItem() const {
  const auto* section = selectedSection();
  return section == nullptr ? nullptr : snapshot.itemAt(*section, itemIndex);
}

void PocketActivity::moveSelection(const int delta) {
  if (view == View::Detail) return;
  syncNotice = SyncNotice::None;
  size_t& selection = view == View::Today ? todaySelection : itemIndex;
  const auto* section = selectedSection();
  const size_t count = view == View::Today ? snapshot.sectionCount : section == nullptr ? 0 : section->itemCount;
  if (delta < 0 && selection > 0)
    --selection;
  else if (delta > 0 && selection + 1 < count)
    ++selection;
}

void PocketActivity::openSelection() {
  syncNotice = SyncNotice::None;
  if (view == View::Today) {
    const auto* section = selectedSection();
    if (section == nullptr) return;
    sectionIndex = todaySelection;
    itemIndex = 0;
    view = View::Section;
  } else if (view == View::Section && selectedItem() != nullptr) {
    view = View::Detail;
  } else {
    return;
  }
  requestUpdate();
}

void PocketActivity::goBack() {
  syncNotice = SyncNotice::None;
  if (view == View::Detail) {
    view = View::Section;
    requestUpdate();
  } else if (view == View::Section) {
    todaySelection = sectionIndex;
    view = View::Today;
    requestUpdate();
  } else {
    onGoHome(HomeMenuItem::POCKET);
  }
}

void PocketActivity::goToday() {
  if (view == View::Today) return;
  view = View::Today;
  todaySelection = sectionIndex;
  syncNotice = SyncNotice::None;
  requestUpdate();
}

const char* PocketActivity::headerLabel() const {
  switch (syncNotice) {
    case SyncNotice::Updating:
      return tr(STR_POCKET_SYNCING);
    case SyncNotice::Updated:
      return tr(STR_POCKET_SYNCED);
    case SyncNotice::NoWifi:
      return tr(STR_POCKET_SYNC_NO_WIFI);
    case SyncNotice::LowMemory:
      return tr(STR_POCKET_SYNC_LOW_MEMORY);
    case SyncNotice::ServiceUnavailable:
      return tr(STR_POCKET_SYNC_UNAVAILABLE);
    case SyncNotice::InvalidData:
      return tr(STR_POCKET_SYNC_INVALID_DATA);
    case SyncNotice::CacheFailure:
      return tr(STR_POCKET_SYNC_CACHE_FAILED);
    case SyncNotice::PairingNeedsAttention:
      return tr(STR_POCKET_SYNC_NEEDS_PAIRING);
    case SyncNotice::None:
      return tr(STR_LAINO_POCKET);
  }
  return tr(STR_LAINO_POCKET);
}

void PocketActivity::formatFreshness(char* buffer, const size_t capacity) const {
  if (capacity == 0) return;
  if (snapshot.fixture || snapshot.generatedAtEpoch == 0) {
    std::snprintf(buffer, capacity, "%s", tr(STR_POCKET_NOT_SYNCED_HINT));
    return;
  }
  ClockDateTime current;
  const auto result = halClock.getDateTimeUtc(current);
  if (result != HalClock::ReadResult::Fresh && result != HalClock::ReadResult::Cached) {
    std::snprintf(buffer, capacity, tr(STR_POCKET_TIME_UNAVAILABLE_FORMAT),
                  pocket::snapshotHasPartialSource(snapshot) ? tr(STR_POCKET_PARTIAL) : tr(STR_POCKET_SAVED_OFFLINE));
    return;
  }
  const uint64_t now = unixEpoch(current);
  const uint64_t ageMinutes = now > snapshot.generatedAtEpoch ? (now - snapshot.generatedAtEpoch) / 60ULL : 0;
  const char* state = pocket::snapshotHasPartialSource(snapshot) ? tr(STR_POCKET_PARTIAL)
                      : ageMinutes >= 120                        ? tr(STR_POCKET_STALE)
                                                                 : tr(STR_POCKET_UPDATED);
  if (ageMinutes < 60)
    std::snprintf(buffer, capacity, tr(STR_POCKET_FRESH_MINUTES_FORMAT), state,
                  static_cast<unsigned long long>(ageMinutes));
  else
    std::snprintf(buffer, capacity, tr(STR_POCKET_FRESH_HOURS_FORMAT), state,
                  static_cast<unsigned long long>(ageMinutes / 60ULL));
}

void PocketActivity::renderToday(const Rect& content) {
  GUI.drawList(
      renderer, content, snapshot.sectionCount, static_cast<int>(todaySelection),
      [this](const int index) -> std::string {
        const auto* section = snapshot.sectionAt(index);
        return section == nullptr ? "" : section->label;
      },
      [this](const int index) -> std::string {
        const auto* section = snapshot.sectionAt(index);
        const auto* first = section == nullptr ? nullptr : snapshot.itemAt(*section, 0);
        return first == nullptr ? tr(STR_POCKET_NOTHING_OPEN) : first->title;
      },
      nullptr,
      [this](const int index) -> std::string {
        const auto* section = snapshot.sectionAt(index);
        return section == nullptr ? "" : std::to_string(section->total);
      },
      false);
}

void PocketActivity::renderSection(const Rect& content) {
  const auto* section = selectedSection();
  if (section == nullptr || section->itemCount == 0) {
    UITheme::drawCenteredText(renderer, content, UI_10_FONT_ID, content.y + content.height / 2,
                              tr(STR_POCKET_NOTHING_OPEN), true);
    return;
  }
  GUI.drawList(
      renderer, content, section->itemCount, static_cast<int>(itemIndex),
      [this, section](const int index) -> std::string {
        const auto* item = snapshot.itemAt(*section, index);
        return item == nullptr ? "" : item->title;
      },
      [this, section](const int index) -> std::string {
        const auto* item = snapshot.itemAt(*section, index);
        return item == nullptr ? "" : item->subtitle;
      });
}

void PocketActivity::renderDetail(const Rect& content) {
  const auto* item = selectedItem();
  if (item == nullptr) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = content.x + metrics.contentSidePadding;
  const int width = content.width - metrics.contentSidePadding * 2;
  int y = content.y + metrics.verticalSpacing;
  drawFitted(renderer, UI_12_FONT_ID, x, y, item->title, width, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;
  drawFitted(renderer, UI_10_FONT_ID, x, y, item->subtitle, width);
  y += renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing * 2;
  for (size_t index = 0; index < item->detailCount; ++index) {
    drawFitted(renderer, UI_10_FONT_ID, x, y, item->detail[index], width);
    y += renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;
  }
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
  char freshness[80];
  formatFreshness(freshness, sizeof(freshness));
  const char* title = headerLabel();
  if (syncNotice == SyncNotice::None && view != View::Today) {
    const auto* section = selectedSection();
    if (section != nullptr) title = section->label;
  }
  if (headerWidth > 0 && metrics.headerHeight > 0) {
    GUI.drawHeader(renderer, Rect{safeLeft, headerY, headerWidth, metrics.headerHeight}, title,
                   view == View::Today ? freshness : nullptr);
  }

  const int sideInset = std::max(metrics.contentSidePadding, metrics.sideButtonHintsWidth + metrics.verticalSpacing);
  const int contentY = headerY + metrics.headerHeight + metrics.verticalSpacing;
  const Rect content{safeLeft + sideInset, contentY, std::max(0, safeRight - safeLeft - sideInset * 2),
                     std::max(0, safeBottom - contentY - metrics.verticalSpacing)};
  if (syncNotice == SyncNotice::Updating) {
    UITheme::drawCenteredText(renderer, content, UI_10_FONT_ID, content.y + content.height / 2, tr(STR_POCKET_SYNCING),
                              true);
  } else if (view == View::Today)
    renderToday(content);
  else if (view == View::Section)
    renderSection(content);
  else
    renderDetail(content);

  const auto* section = selectedSection();
  const size_t selection = view == View::Today ? todaySelection : itemIndex;
  const size_t selectionCount = view == View::Today  ? snapshot.sectionCount
                                : section == nullptr ? 0
                                                     : section->itemCount;
  const bool canOpen =
      view == View::Today ? selectedSection() != nullptr : view == View::Section && selectedItem() != nullptr;
  const char* previousLabel = !workerRunning && view != View::Detail && selection > 0 ? tr(STR_POCKET_PREVIOUS) : "";
  const char* nextLabel =
      !workerRunning && view != View::Detail && selection + 1 < selectionCount ? tr(STR_POCKET_NEXT) : "";
  const char* confirmLabel = workerRunning ? "" : !canSync() ? tr(STR_POCKET_HARI) : canOpen ? tr(STR_OPEN) : "";
  const char* todayLabel = !workerRunning && view != View::Today ? tr(STR_POCKET_TODAY) : "";
  const char* syncLabel = workerRunning ? "" : canSync() ? tr(STR_POCKET_SYNC) : tr(STR_POCKET_HARI);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, todayLabel, syncLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, previousLabel, nextLabel);

  renderer.displayBuffer();
  initialRenderComplete = true;
}
