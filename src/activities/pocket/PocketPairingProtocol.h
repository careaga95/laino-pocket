#pragma once

#include <cstddef>
#include <cstdint>

namespace pocket {

inline constexpr uint8_t PAIRING_PROTOCOL_VERSION = 1;
inline constexpr std::size_t DEVICE_CODE_BYTES = 43;
inline constexpr std::size_t USER_CODE_BYTES = 8;
inline constexpr std::size_t ACCOUNT_HINT_BYTES = 24;
inline constexpr std::size_t DEVICE_ID_TEXT_BYTES = 36;
inline constexpr const char* POCKET_SCOPE = "pocket.device.read";
inline constexpr const char* POCKET_CLAIM_URL = "https://hari.laino.app/pocket/pair";

enum class JsonParseResult : uint8_t { Success, TooLarge, Malformed, InvalidSchema };

struct PairingStartResponse {
  char deviceCode[DEVICE_CODE_BYTES + 1]{};
  char userCode[USER_CODE_BYTES + 1]{};
  uint16_t expiresIn = 0;
  uint8_t interval = 0;
  uint8_t firstPollAfter = 0;
};

enum class PollStatus : uint8_t { Pending, Claimed };

struct PairingPollResponse {
  PollStatus status = PollStatus::Pending;
  char accountHint[ACCOUNT_HINT_BYTES + 1]{};
};

struct PairingFinalizeResponse {
  char accessToken[DEVICE_CODE_BYTES + 1]{};
  char deviceId[DEVICE_ID_TEXT_BYTES + 1]{};
};

struct PocketSelfResponse {
  char deviceId[DEVICE_ID_TEXT_BYTES + 1]{};
  char name[65]{};
};

JsonParseResult parsePairingStartResponse(const char* json, std::size_t length, PairingStartResponse& response);
JsonParseResult parsePairingPollResponse(const char* json, std::size_t length, PairingPollResponse& response);
JsonParseResult parsePairingFinalizeResponse(const char* json, std::size_t length, PairingFinalizeResponse& response);
JsonParseResult parsePocketSelfResponse(const char* json, std::size_t length, PocketSelfResponse& response);
JsonParseResult parsePocketStatusResponse(const char* json, std::size_t length, const char* expectedStatus);
JsonParseResult parsePocketError(const char* json, std::size_t length, char* error, std::size_t errorSize);
bool isBase64Url256(const char* value, std::size_t length);
bool isCanonicalUserCode(const char* value, std::size_t length);

enum class PairingState : uint8_t {
  Unpaired,
  WifiCheck,
  Starting,
  CodeAndPolling,
  ConfirmAccount,
  Rejecting,
  Finalizing,
  SavingNvs,
  Verifying,
  PairedUnverified,
  Paired,
  UnpairConfirm,
  Unpairing,
  Cancelling,
  Cancelled,
  Expired,
  ErrorRetry,
  RestartRequired,
  StorageError,
  RevokedNotice,
};

enum class RevocationPersistenceState : uint8_t { None, TombstoneSaved, KeyCleared, ClearPending };

class RevocationPersistenceCoordinator {
 public:
  RevocationPersistenceState recordInitial(bool tombstoneSaved, bool clearSucceeded);
  RevocationPersistenceState recordClearRetry(bool clearSucceeded);
  [[nodiscard]] RevocationPersistenceState state() const { return current; }
  [[nodiscard]] bool resolved() const {
    return current == RevocationPersistenceState::TombstoneSaved ||
           current == RevocationPersistenceState::KeyCleared;
  }

 private:
  RevocationPersistenceState current = RevocationPersistenceState::None;
};

class PairingMachine {
 public:
  void resetUnpaired();
  void restorePaired(bool revokedTombstone);
  void beginPairing();
  void wifiReady();
  void startSucceeded(const PairingStartResponse& response, uint32_t now);
  void startFailed();
  [[nodiscard]] bool pollDue(uint32_t now) const;
  [[nodiscard]] bool expired(uint32_t now) const;
  [[nodiscard]] uint16_t secondsRemaining(uint32_t now) const;
  void pollStarted(uint32_t now);
  void pollPending(uint32_t now);
  void pollClaimed();
  void pollRateLimited(uint8_t retryAfterSeconds, uint32_t now);
  void pollTransportFailed(uint32_t now);
  void retryPolling(uint32_t now);
  void expire();
  void confirmAccount();
  void rejectAccount();
  void rejectFinished();
  void finalizeSucceeded();
  void finalizeConsumed();
  void finalizeFailed(bool isExpired);
  void credentialSaved(bool success);
  void storageFailed();
  void verifySucceeded();
  void verificationStarted();
  void verifyNetworkFailed();
  void verifyRevoked();
  void beginUnpair();
  void confirmUnpair();
  void cancelUnpair();
  void unpairSucceeded();
  void unpairFailed();
  void cancellationStarted();
  void cancellationFinished();

  [[nodiscard]] PairingState state() const { return currentState; }
  [[nodiscard]] uint8_t consecutiveFailures() const { return failures; }

 private:
  PairingState currentState = PairingState::Unpaired;
  uint32_t startedAt = 0;
  uint32_t lastPollAt = 0;
  uint32_t retryDelayStartedAt = 0;
  uint32_t ttlMs = 600000;
  uint32_t pollIntervalMs = 10000;
  uint32_t firstPollDelayMs = 5000;
  uint32_t retryDelayMs = 0;
  uint8_t failures = 0;
  bool firstPollPending = true;
  bool retryDelayActive = false;
  PairingState unpairReturnState = PairingState::Paired;
};

const char* pairingStateName(PairingState state);

}  // namespace pocket
