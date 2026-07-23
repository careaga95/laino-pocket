#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "PocketBundleWorker.h"
#include "PocketCredentialStore.h"
#include "PocketReadingCache.h"
#include "PocketReadingWorker.h"
#include "PocketSdCacheStorage.h"
#include "PocketSnapshot.h"
#include "PocketSnapshotCache.h"
#include "activities/Activity.h"

struct Rect;

class PocketActivity final : public Activity {
  enum class SyncNotice : uint8_t {
    None,
    Updating,
    Updated,
    NoWifi,
    LowMemory,
    ServiceUnavailable,
    InvalidData,
    CacheFailure,
    PairingNeedsAttention,
    Downloading,
    DownloadFailed,
    IntegrityFailure
  };
  enum class View : uint8_t { Today, Sections, Section, Detail, ReadingList };
  enum class TodaySlot : uint8_t { NextMeeting, TopPriority, Waiting, Owe, Count };
  enum class PendingNetworkAction : uint8_t { None, Snapshot, ReadingManifest, ReadingDownload };
  enum class WorkerKind : uint8_t { None, Snapshot, Reading };

  pocket::PocketSdCacheStorage cacheStorage;
  pocket::PocketSnapshotCache cache{cacheStorage};
  pocket::PocketReadingCache readingCache{cacheStorage};
  pocket::PocketSnapshot snapshot;
  pocket::ReadingManifest readings;
  pocket::CredentialStore credentialStore;
  pocket::Credential credential{};
  pocket::BundleWorkerContext* workerContext = nullptr;
  pocket::ReadingWorkerContext* readingWorkerContext = nullptr;
  TaskHandle_t workerHandle = nullptr;
  SyncNotice syncNotice = SyncNotice::None;
  bool credentialPresent = false;
  bool credentialNeedsAttention = false;
  bool workerRunning = false;
  bool exitAfterCancellation = false;
  bool autoSyncPending = false;
  bool initialRenderComplete = false;
  bool detailFromToday = false;
  bool sectionFromToday = false;
  WorkerKind workerKind = WorkerKind::None;
  PendingNetworkAction pendingNetworkAction = PendingNetworkAction::None;
  uint32_t workerStackMargin = 0;
  View view = View::Today;
  size_t todaySelection = 0;
  size_t sectionIndex = 0;
  size_t itemIndex = 0;
  size_t readingIndex = 0;
  bool readingLocal[pocket::MAX_READING_ITEMS]{};

 public:
  explicit PocketActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Pocket", renderer, mappedInput) {}
  ~PocketActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return workerRunning; }
  bool preventAutoSleep() override { return workerRunning; }

 private:
  static void workerTrampoline(void* context);
  static void runWorker(pocket::BundleWorkerContext* context);
  static void readingWorkerTrampoline(void* context);
  static void runReadingWorker(pocket::ReadingWorkerContext* context);
  void reloadCredential();
  void openHari();
  void requestSync();
  void requestReadingManifestSync();
  void requestReadingDownload();
  void requestNetworkAction(PendingNetworkAction action);
  void onWifiSelectionComplete(bool connected);
  bool startWorker();
  bool startReadingWorker(pocket::ReadingWorkerOperation operation);
  void processWorkerResult();
  void processReadingWorkerResult();
  void cancelWorkerAndExit();
  void restoreCachedSnapshot();
  void restoreCachedReadings();
  void refreshReadingLocalState();
  void openReadings();
  void openSelectedReading();
  [[nodiscard]] bool selectedReadingDownloaded() const;
  [[nodiscard]] bool canSync() const { return credentialPresent && !credentialNeedsAttention; }
  [[nodiscard]] bool refreshDue() const;
  [[nodiscard]] const char* headerLabel() const;
  [[nodiscard]] const char* sectionLabel(const pocket::SnapshotSection& section) const;
  [[nodiscard]] const pocket::SnapshotSection* selectedSection() const;
  [[nodiscard]] const pocket::SnapshotItem* selectedItem() const;
  [[nodiscard]] pocket::SnapshotSectionId todaySectionIdAt(size_t index) const;
  [[nodiscard]] const pocket::SnapshotSection* sectionForId(pocket::SnapshotSectionId id) const;
  [[nodiscard]] const pocket::SnapshotItem* todayItemAt(size_t index) const;
  [[nodiscard]] const pocket::SnapshotItem* selectedTodayItem() const;
  [[nodiscard]] size_t todayItemCount() const;
  [[nodiscard]] const char* todaySlotLabel(size_t index) const;
  [[nodiscard]] const char* todayEmptyLabel(size_t index) const;
  [[nodiscard]] bool canOpenTodaySelection() const;
  bool selectSection(pocket::SnapshotSectionId id);
  bool selectItem(const pocket::SnapshotItem* item);
  void moveSelection(int delta);
  void openSelection();
  void openSections();
  void goBack();
  void goToday();
  void renderToday(const Rect& content);
  void renderSections(const Rect& content);
  void renderSection(const Rect& content);
  void renderDetail(const Rect& content);
  void renderReadings(const Rect& content);
  void formatFreshness(char* buffer, size_t capacity) const;
  void formatReadingFreshness(char* buffer, size_t capacity) const;
};
