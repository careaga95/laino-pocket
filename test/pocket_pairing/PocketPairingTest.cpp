#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "PocketBundleWorker.h"
#include "PocketCredential.h"
#include "PocketCredentialStore.h"
#include "PocketPairingClient.h"
#include "PocketPairingProtocol.h"
#include "PocketPairingWorker.h"
#include "PocketPairingTransportPolicy.h"

namespace {

constexpr char DEVICE_CODE[] = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
constexpr char TOKEN[] = "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE";
constexpr char UUID[] = "00112233-4455-6677-8899-aabbccddeeff";

pocket::PairingIdentity identityForFirmware(const char* firmware) {
  return {pocket::PAIRING_PROTOCOL_VERSION, pocket::POCKET_DEVICE_MODEL, firmware};
}

pocket::PairingStartResponse validStart() {
  pocket::PairingStartResponse response{};
  std::memcpy(response.deviceCode, DEVICE_CODE, sizeof(DEVICE_CODE));
  std::memcpy(response.userCode, "ABCDEFGH", 9);
  response.expiresIn = 600;
  response.interval = 10;
  response.firstPollAfter = 5;
  return response;
}

class FakeGateway final : public pocket::PocketGatewayTransport {
 public:
  pocket::GatewayResponse next{};
  pocket::GatewayMethod method = pocket::GatewayMethod::Get;
  std::string path;
  std::string body;
  std::string bearer;
  std::string successJson;
  std::size_t successCapacity = 0;
  int calls = 0;

  void request(const pocket::GatewayRequest& request, const std::atomic<bool>&,
               pocket::GatewayResponse& response) override {
    ++calls;
    method = request.method;
    path = request.path == nullptr ? "" : request.path;
    body.assign(request.jsonBody == nullptr ? "" : request.jsonBody, request.jsonBodyLength);
    bearer = request.bearer == nullptr ? "" : request.bearer;
    successCapacity = request.successBodyCapacity;
    response = next;
    if (response.httpStatus >= 200 && response.httpStatus <= 299 && request.successBody != nullptr) {
      if (successJson.size() + 1 > request.successBodyCapacity) {
        response.transport = pocket::GatewayTransportResult::OversizedResponse;
        response.bodyLength = 0;
        return;
      }
      std::memcpy(request.successBody, successJson.data(), successJson.size());
      request.successBody[successJson.size()] = '\0';
      response.bodyLength = static_cast<uint16_t>(successJson.size());
    }
  }

  void json(int status, const std::string& value, uint8_t retryAfter = 0) {
    successJson = value;
    next = {};
    next.transport = pocket::GatewayTransportResult::Success;
    next.httpStatus = static_cast<int16_t>(status);
    next.retryAfter = retryAfter;
    if (value.size() < sizeof(next.body)) {
      next.bodyLength = static_cast<uint16_t>(value.size());
      std::memcpy(next.body, value.data(), value.size());
      next.body[value.size()] = '\0';
    }
  }
};

class FakeCredentialStorage final : public pocket::CredentialBlobStorage {
 public:
  std::vector<uint8_t> bytes;
  bool removeSucceeds = true;
  bool writeSucceeds = true;
  bool lengthSucceeds = true;
  bool readSucceeds = true;
  int writeCalls = 0;
  int removeCalls = 0;

  pocket::BlobStorageResult length(std::size_t& value) override {
    value = bytes.size();
    if (!lengthSucceeds) return pocket::BlobStorageResult::Error;
    return bytes.empty() ? pocket::BlobStorageResult::NotFound : pocket::BlobStorageResult::Ok;
  }
  pocket::BlobStorageResult read(uint8_t* output, const std::size_t outputSize) override {
    if (!readSucceeds || outputSize != bytes.size()) return pocket::BlobStorageResult::Error;
    const std::size_t count = std::min(outputSize, bytes.size());
    std::memcpy(output, bytes.data(), count);
    return count == outputSize ? pocket::BlobStorageResult::Ok : pocket::BlobStorageResult::Error;
  }
  pocket::BlobStorageResult write(const uint8_t* data, const std::size_t dataSize) override {
    ++writeCalls;
    if (!writeSucceeds) return pocket::BlobStorageResult::Error;
    bytes.assign(data, data + dataSize);
    return pocket::BlobStorageResult::Ok;
  }
  pocket::BlobStorageResult remove() override {
    ++removeCalls;
    if (!removeSucceeds) return pocket::BlobStorageResult::Error;
    bytes.clear();
    return pocket::BlobStorageResult::Ok;
  }
};

}  // namespace

TEST(PocketCredentialTest, RoundTripsExactPairedBlob) {
  pocket::Credential input{};
  input.state = pocket::CredentialState::Paired;
  for (std::size_t i = 0; i < pocket::DEVICE_UUID_BYTES; ++i) input.deviceUuid[i] = static_cast<uint8_t>(i);
  for (std::size_t i = 0; i < pocket::DEVICE_TOKEN_BYTES; ++i) input.token[i] = static_cast<uint8_t>(i + 1);

  std::array<uint8_t, pocket::CREDENTIAL_BLOB_BYTES> blob{};
  ASSERT_TRUE(pocket::encodeCredentialBlob(input, blob.data(), blob.size()));
  EXPECT_EQ(blob[0], 0x4c);
  EXPECT_EQ(blob[1], 0x50);
  EXPECT_EQ(blob[2], 1);
  EXPECT_EQ(blob[3], 1);
  EXPECT_EQ(blob[53], 0);

  pocket::Credential output{};
  ASSERT_TRUE(pocket::decodeCredentialBlob(blob.data(), blob.size(), output));
  EXPECT_EQ(output.state, pocket::CredentialState::Paired);
  EXPECT_EQ(std::memcmp(output.deviceUuid, input.deviceUuid, sizeof(input.deviceUuid)), 0);
  EXPECT_EQ(std::memcmp(output.token, input.token, sizeof(input.token)), 0);
}

TEST(PocketCredentialTest, RejectsWrongSizeSchemaReservedByteAndCrc) {
  pocket::Credential input{};
  input.token[0] = 1;
  std::array<uint8_t, pocket::CREDENTIAL_BLOB_BYTES> blob{};
  ASSERT_TRUE(pocket::encodeCredentialBlob(input, blob.data(), blob.size()));
  pocket::Credential output{};

  EXPECT_FALSE(pocket::decodeCredentialBlob(blob.data(), blob.size() - 1, output));
  auto changed = blob;
  changed[2] = 2;
  EXPECT_FALSE(pocket::decodeCredentialBlob(changed.data(), changed.size(), output));
  changed = blob;
  changed[53] = 1;
  EXPECT_FALSE(pocket::decodeCredentialBlob(changed.data(), changed.size(), output));
  changed = blob;
  changed[20] ^= 0x80;
  EXPECT_FALSE(pocket::decodeCredentialBlob(changed.data(), changed.size(), output));
}

TEST(PocketCredentialTest, TombstoneRequiresZeroToken) {
  pocket::Credential tombstone{};
  tombstone.state = pocket::CredentialState::RevokedTombstone;
  tombstone.revocationReason = 1;
  std::array<uint8_t, pocket::CREDENTIAL_BLOB_BYTES> blob{};
  EXPECT_TRUE(pocket::encodeCredentialBlob(tombstone, blob.data(), blob.size()));

  tombstone.token[0] = 7;
  EXPECT_FALSE(pocket::encodeCredentialBlob(tombstone, blob.data(), blob.size()));
}

TEST(PocketCredentialTest, UUIDAndTokenConversionsAreStrict) {
  uint8_t uuid[pocket::DEVICE_UUID_BYTES]{};
  EXPECT_TRUE(pocket::parseUuid(UUID, std::strlen(UUID), uuid, sizeof(uuid)));
  EXPECT_FALSE(pocket::parseUuid("00112233445566778899aabbccddeeff", 32, uuid, sizeof(uuid)));

  uint8_t token[pocket::DEVICE_TOKEN_BYTES]{};
  ASSERT_TRUE(pocket::decodeBase64UrlToken(TOKEN, std::strlen(TOKEN), token, sizeof(token)));
  char encoded[pocket::DEVICE_TOKEN_TEXT_BYTES + 1]{};
  ASSERT_TRUE(pocket::encodeBase64UrlToken(token, sizeof(token), encoded, sizeof(encoded)));
  EXPECT_STREQ(encoded, TOKEN);
  std::string nonCanonical(TOKEN);
  nonCanonical.back() = 'B';
  EXPECT_FALSE(pocket::decodeBase64UrlToken(nonCanonical.c_str(), nonCanonical.size(), token, sizeof(token)));
  EXPECT_TRUE(std::all_of(std::begin(token), std::end(token), [](uint8_t value) { return value == 0; }));
}

TEST(PocketCredentialTest, InvalidBlobClearsPreviousDestinationCredential) {
  pocket::Credential input{};
  input.token[0] = 9;
  std::array<uint8_t, pocket::CREDENTIAL_BLOB_BYTES> blob{};
  ASSERT_TRUE(pocket::encodeCredentialBlob(input, blob.data(), blob.size()));
  blob[54] ^= 1;

  pocket::Credential output{};
  std::memset(&output, 0xa5, sizeof(output));
  EXPECT_FALSE(pocket::decodeCredentialBlob(blob.data(), blob.size(), output));
  const auto* bytes = reinterpret_cast<const uint8_t*>(&output);
  EXPECT_TRUE(std::all_of(bytes, bytes + sizeof(output), [](uint8_t value) { return value == 0; }));
}

TEST(PocketCredentialStorageTest, HandlesAbsentValidCorruptWrongSizeAndRemovalFailure) {
  FakeCredentialStorage storage;
  pocket::Credential loaded{};
  EXPECT_EQ(pocket::loadCredentialFromStorage(storage, loaded), pocket::CredentialLoadResult::Absent);

  pocket::Credential input{};
  input.token[0] = 7;
  ASSERT_TRUE(pocket::saveCredentialToStorage(storage, input));
  EXPECT_EQ(storage.writeCalls, 1);
  EXPECT_EQ(storage.bytes.size(), pocket::CREDENTIAL_BLOB_BYTES);
  EXPECT_EQ(pocket::loadCredentialFromStorage(storage, loaded), pocket::CredentialLoadResult::Paired);

  storage.bytes[20] ^= 0x40;
  EXPECT_EQ(pocket::loadCredentialFromStorage(storage, loaded), pocket::CredentialLoadResult::CorruptRemoved);
  EXPECT_TRUE(storage.bytes.empty());

  storage.bytes.assign(55, 0);
  EXPECT_EQ(pocket::loadCredentialFromStorage(storage, loaded), pocket::CredentialLoadResult::CorruptRemoved);
  storage.bytes.assign(57, 0);
  storage.removeSucceeds = false;
  EXPECT_EQ(pocket::loadCredentialFromStorage(storage, loaded), pocket::CredentialLoadResult::StorageError);
}

TEST(PocketCredentialStorageTest, FailedWriteIsReportedAndClearIsIdempotent) {
  FakeCredentialStorage storage;
  pocket::Credential input{};
  input.token[0] = 1;
  storage.writeSucceeds = false;
  EXPECT_FALSE(pocket::saveCredentialToStorage(storage, input));
  EXPECT_EQ(storage.writeCalls, 1);
  EXPECT_TRUE(pocket::clearCredentialStorage(storage));
  EXPECT_EQ(storage.removeCalls, 0);
}

TEST(PocketCredentialStorageTest, RealLengthReadAndDeleteFailuresRemainStorageErrors) {
  FakeCredentialStorage storage;
  pocket::Credential loaded{};
  storage.lengthSucceeds = false;
  EXPECT_EQ(pocket::loadCredentialFromStorage(storage, loaded), pocket::CredentialLoadResult::StorageError);
  EXPECT_FALSE(pocket::clearCredentialStorage(storage));

  storage.lengthSucceeds = true;
  storage.bytes.assign(pocket::CREDENTIAL_BLOB_BYTES, 0);
  storage.readSucceeds = false;
  EXPECT_EQ(pocket::loadCredentialFromStorage(storage, loaded), pocket::CredentialLoadResult::StorageError);
  EXPECT_EQ(storage.removeCalls, 0);

  storage.readSucceeds = true;
  storage.bytes.assign(8, 0);
  storage.removeSucceeds = false;
  EXPECT_EQ(pocket::loadCredentialFromStorage(storage, loaded), pocket::CredentialLoadResult::StorageError);
}

TEST(PocketPairingParserTest, ParsesExactStartSchemaAndOptionalFutureScalar) {
  const std::string json = std::string("{\"protocol\":1,\"device_code\":\"") + DEVICE_CODE +
                           "\",\"user_code\":\"ABCDEFGH\",\"expires_in\":600,\"interval\":10,"
                           "\"first_poll_after\":5,\"future\":true}";
  pocket::PairingStartResponse response{};
  EXPECT_EQ(pocket::parsePairingStartResponse(json.data(), json.size(), response), pocket::JsonParseResult::Success);
  EXPECT_STREQ(response.deviceCode, DEVICE_CODE);
  EXPECT_STREQ(response.userCode, "ABCDEFGH");
}

TEST(PocketPairingParserTest, RejectsWrongTypesDuplicatesLengthsAndOversize) {
  const std::string wrongType = std::string("{\"protocol\":\"1\",\"device_code\":\"") + DEVICE_CODE +
                                "\",\"user_code\":\"ABCDEFGH\",\"expires_in\":600,\"interval\":10,"
                                "\"first_poll_after\":5}";
  pocket::PairingStartResponse response{};
  EXPECT_EQ(pocket::parsePairingStartResponse(wrongType.data(), wrongType.size(), response),
            pocket::JsonParseResult::InvalidSchema);
  const std::string duplicate = "{\"protocol\":1,\"protocol\":1}";
  EXPECT_EQ(pocket::parsePairingStartResponse(duplicate.data(), duplicate.size(), response),
            pocket::JsonParseResult::InvalidSchema);
  std::string oversized(385, ' ');
  EXPECT_EQ(pocket::parsePairingStartResponse(oversized.data(), oversized.size(), response),
            pocket::JsonParseResult::TooLarge);
}

TEST(PocketPairingParserTest, SensitiveStartScratchNeverPublishesPartialOrInvalidData) {
  pocket::PairingStartResponse destination{};
  std::memset(&destination, 0x5a, sizeof(destination));
  const pocket::PairingStartResponse original = destination;
  const std::string partial = std::string("{\"protocol\":1,\"device_code\":\"") + DEVICE_CODE +
                              "\",\"user_code\":\"ABCD";
  EXPECT_EQ(pocket::parsePairingStartResponse(partial.data(), partial.size(), destination),
            pocket::JsonParseResult::InvalidSchema);
  EXPECT_EQ(std::memcmp(&destination, &original, sizeof(destination)), 0);

  const std::string duplicate = std::string("{\"protocol\":1,\"device_code\":\"") + DEVICE_CODE +
                                "\",\"device_code\":\"" + DEVICE_CODE + "\"}";
  EXPECT_EQ(pocket::parsePairingStartResponse(duplicate.data(), duplicate.size(), destination),
            pocket::JsonParseResult::InvalidSchema);
  EXPECT_EQ(std::memcmp(&destination, &original, sizeof(destination)), 0);

  const std::string oversized(385, 'x');
  EXPECT_EQ(pocket::parsePairingStartResponse(oversized.data(), oversized.size(), destination),
            pocket::JsonParseResult::TooLarge);
  EXPECT_EQ(std::memcmp(&destination, &original, sizeof(destination)), 0);
}

TEST(PocketPairingParserTest, ParsesPendingAndClaimedHintWithUnicodeEscapes) {
  pocket::PairingPollResponse pending{};
  constexpr char PENDING[] = "{\"protocol\":1,\"status\":\"pending\"}";
  EXPECT_EQ(pocket::parsePairingPollResponse(PENDING, sizeof(PENDING) - 1, pending), pocket::JsonParseResult::Success);
  EXPECT_EQ(pending.status, pocket::PollStatus::Pending);

  pocket::PairingPollResponse claimed{};
  constexpr char CLAIMED[] =
      "{\"protocol\":1,\"status\":\"claimed\",\"account_hint\":\"a\\u2022\\u2022\\u2022@g\\u2026\"}";
  EXPECT_EQ(pocket::parsePairingPollResponse(CLAIMED, sizeof(CLAIMED) - 1, claimed), pocket::JsonParseResult::Success);
  EXPECT_EQ(claimed.status, pocket::PollStatus::Claimed);
  EXPECT_NE(std::strstr(claimed.accountHint, "@g"), nullptr);
}

TEST(PocketPairingParserTest, RejectsHintOver24Utf8BytesAndHintOnPending) {
  pocket::PairingPollResponse response{};
  constexpr char TOO_LONG[] = "{\"protocol\":1,\"status\":\"claimed\",\"account_hint\":\"1234567890123456789012345\"}";
  EXPECT_EQ(pocket::parsePairingPollResponse(TOO_LONG, sizeof(TOO_LONG) - 1, response),
            pocket::JsonParseResult::InvalidSchema);
  constexpr char WRONG_SHAPE[] = "{\"protocol\":1,\"status\":\"pending\",\"account_hint\":\"x\"}";
  EXPECT_EQ(pocket::parsePairingPollResponse(WRONG_SHAPE, sizeof(WRONG_SHAPE) - 1, response),
            pocket::JsonParseResult::InvalidSchema);
  const char invalidUtf8[] = "{\"protocol\":1,\"status\":\"claimed\",\"account_hint\":\"\xc0\xaf\"}";
  EXPECT_EQ(pocket::parsePairingPollResponse(invalidUtf8, sizeof(invalidUtf8) - 1, response),
            pocket::JsonParseResult::InvalidSchema);
}

TEST(PocketPairingParserTest, FinalizeAndSelfRequireScopeTokenTypeUuidAndActiveStatus) {
  const std::string finalized = std::string("{\"protocol\":1,\"access_token\":\"") + TOKEN +
                                "\",\"token_type\":\"Bearer\",\"device_id\":\"" + UUID +
                                "\",\"scope\":\"pocket.device.read\"}";
  pocket::PairingFinalizeResponse finalizeResponse{};
  EXPECT_EQ(pocket::parsePairingFinalizeResponse(finalized.data(), finalized.size(), finalizeResponse),
            pocket::JsonParseResult::Success);

  const std::string self = std::string("{\"protocol\":1,\"device_id\":\"") + UUID +
                           "\",\"name\":\"Laino Pocket (X3)\",\"scope\":\"pocket.device.read\","
                           "\"status\":\"active\"}";
  pocket::PocketSelfResponse selfResponse{};
  EXPECT_EQ(pocket::parsePocketSelfResponse(self.data(), self.size(), selfResponse), pocket::JsonParseResult::Success);

  std::string wrongScope = finalized;
  wrongScope.replace(wrongScope.find("pocket.device.read"), 18, "mail.read         ");
  EXPECT_EQ(pocket::parsePairingFinalizeResponse(wrongScope.data(), wrongScope.size(), finalizeResponse),
            pocket::JsonParseResult::InvalidSchema);
}

TEST(PocketPairingParserTest, SensitiveFinalizeScratchNeverPublishesPartialBearer) {
  pocket::PairingFinalizeResponse destination{};
  std::memset(&destination, 0x3c, sizeof(destination));
  const pocket::PairingFinalizeResponse original = destination;
  const std::string partialToken = "{\"protocol\":1,\"access_token\":\"AQEBAQEBAQEBAQE";
  EXPECT_EQ(pocket::parsePairingFinalizeResponse(partialToken.data(), partialToken.size(), destination),
            pocket::JsonParseResult::InvalidSchema);
  EXPECT_EQ(std::memcmp(&destination, &original, sizeof(destination)), 0);

  const std::string wrongType = std::string("{\"protocol\":1,\"access_token\":7,\"token_type\":\"Bearer\","
                                            "\"device_id\":\"") +
                                UUID + "\",\"scope\":\"pocket.device.read\"}";
  EXPECT_EQ(pocket::parsePairingFinalizeResponse(wrongType.data(), wrongType.size(), destination),
            pocket::JsonParseResult::InvalidSchema);
  EXPECT_EQ(std::memcmp(&destination, &original, sizeof(destination)), 0);

  const std::string oversized(513, 'x');
  EXPECT_EQ(pocket::parsePairingFinalizeResponse(oversized.data(), oversized.size(), destination),
            pocket::JsonParseResult::TooLarge);
  EXPECT_EQ(std::memcmp(&destination, &original, sizeof(destination)), 0);
}

TEST(PocketPairingMachineTest, FollowsSuccessPathAndFirstPollSchedule) {
  pocket::PairingMachine machine;
  machine.beginPairing();
  EXPECT_EQ(machine.state(), pocket::PairingState::WifiCheck);
  machine.wifiReady();
  machine.startSucceeded(validStart(), 1000);
  EXPECT_FALSE(machine.pollDue(5999));
  EXPECT_TRUE(machine.pollDue(6000));
  machine.pollStarted(6000);
  EXPECT_FALSE(machine.pollDue(15999));
  EXPECT_TRUE(machine.pollDue(16000));
  machine.pollClaimed();
  machine.confirmAccount();
  machine.finalizeSucceeded();
  machine.credentialSaved(true);
  machine.verifySucceeded();
  EXPECT_EQ(machine.state(), pocket::PairingState::Paired);
}

TEST(PocketPairingMachineTest, PollingAndExpirySurviveUnsignedMillisWrap) {
  pocket::PairingMachine machine;
  machine.startSucceeded(validStart(), 0xfffffff0U);
  EXPECT_FALSE(machine.pollDue(0x00001377U));
  EXPECT_TRUE(machine.pollDue(0x00001378U));
  EXPECT_FALSE(machine.expired(0x000927afU));
  EXPECT_TRUE(machine.expired(0x000927b0U));
  EXPECT_EQ(machine.secondsRemaining(0x000927b0U), 0);
}

TEST(PocketPairingMachineTest, ThreeTransportFailuresStopAndRetryKeepsOriginalTtl) {
  pocket::PairingMachine machine;
  machine.startSucceeded(validStart(), 100);
  machine.pollTransportFailed(5100);
  machine.pollTransportFailed(15100);
  machine.pollTransportFailed(25100);
  EXPECT_EQ(machine.state(), pocket::PairingState::ErrorRetry);
  EXPECT_EQ(machine.consecutiveFailures(), 3);
  machine.retryPolling(30000);
  EXPECT_EQ(machine.state(), pocket::PairingState::CodeAndPolling);
  EXPECT_TRUE(machine.pollDue(30000));
  machine.pollTransportFailed(600100);
  machine.retryPolling(600100);
  EXPECT_EQ(machine.state(), pocket::PairingState::Expired);
}

TEST(PocketPairingMachineTest, RetryAfterIsClampedToFiveAndThirtySeconds) {
  pocket::PairingMachine machine;
  machine.startSucceeded(validStart(), 0);
  machine.pollRateLimited(1, 5000);
  EXPECT_FALSE(machine.pollDue(9999));
  EXPECT_TRUE(machine.pollDue(10000));
  machine.pollRateLimited(99, 10000);
  EXPECT_FALSE(machine.pollDue(39999));
  EXPECT_TRUE(machine.pollDue(40000));
}

TEST(PocketPairingMachineTest, PendingPollWaitsFullIntervalAfterResponseCompletes) {
  pocket::PairingMachine machine;
  machine.startSucceeded(validStart(), 1000);
  ASSERT_TRUE(machine.pollDue(6000));

  machine.pollStarted(6000);
  machine.pollPending(9000);

  EXPECT_FALSE(machine.pollDue(18999));
  EXPECT_TRUE(machine.pollDue(19000));
}

TEST(PocketPairingMachineTest, ConsumedAndRevokedHaveDedicatedRecoveryStates) {
  pocket::PairingMachine machine;
  machine.finalizeConsumed();
  EXPECT_EQ(machine.state(), pocket::PairingState::RestartRequired);
  machine.restorePaired(false);
  machine.verifyRevoked();
  EXPECT_EQ(machine.state(), pocket::PairingState::RevokedNotice);
  machine.restorePaired(true);
  EXPECT_EQ(machine.state(), pocket::PairingState::RevokedNotice);
}

TEST(PocketPairingMachineTest, StorageFailureCannotBecomeUnpairedImplicitly) {
  pocket::PairingMachine machine;
  machine.storageFailed();
  EXPECT_EQ(machine.state(), pocket::PairingState::StorageError);
}

TEST(PocketPairingMachineTest, CancelUnpairRestoresExactVerifiedState) {
  pocket::PairingMachine machine;
  machine.restorePaired(false);
  machine.verifySucceeded();
  ASSERT_EQ(machine.state(), pocket::PairingState::Paired);
  machine.beginUnpair();
  ASSERT_EQ(machine.state(), pocket::PairingState::UnpairConfirm);
  machine.cancelUnpair();
  EXPECT_EQ(machine.state(), pocket::PairingState::Paired);
}

TEST(PocketPairingMachineTest, CancelUnpairRestoresExactUnverifiedState) {
  pocket::PairingMachine machine;
  machine.restorePaired(false);
  ASSERT_EQ(machine.state(), pocket::PairingState::PairedUnverified);
  machine.beginUnpair();
  ASSERT_EQ(machine.state(), pocket::PairingState::UnpairConfirm);
  machine.cancelUnpair();
  EXPECT_EQ(machine.state(), pocket::PairingState::PairedUnverified);

  machine.beginUnpair();
  machine.confirmUnpair();
  machine.cancelUnpair();
  EXPECT_EQ(machine.state(), pocket::PairingState::Paired);
  machine.unpairSucceeded();
  EXPECT_EQ(machine.state(), pocket::PairingState::Unpaired);
}

TEST(PocketRevocationPersistenceTest, CoversInitialAndRetryStorageDecisions) {
  pocket::RevocationPersistenceCoordinator saved;
  EXPECT_EQ(saved.recordInitial(true, false), pocket::RevocationPersistenceState::TombstoneSaved);
  EXPECT_TRUE(saved.resolved());

  pocket::RevocationPersistenceCoordinator cleared;
  EXPECT_EQ(cleared.recordInitial(false, true), pocket::RevocationPersistenceState::KeyCleared);
  EXPECT_TRUE(cleared.resolved());

  pocket::RevocationPersistenceCoordinator pending;
  EXPECT_EQ(pending.recordInitial(false, false), pocket::RevocationPersistenceState::ClearPending);
  EXPECT_FALSE(pending.resolved());
  EXPECT_EQ(pending.recordClearRetry(false), pocket::RevocationPersistenceState::ClearPending);
  EXPECT_FALSE(pending.resolved());
  EXPECT_EQ(pending.recordClearRetry(true), pocket::RevocationPersistenceState::KeyCleared);
  EXPECT_TRUE(pending.resolved());
}

TEST(PocketPairingClientTest, StartUsesExactGatewayPathAndBoundedBody) {
  FakeGateway gateway;
  gateway.json(200, std::string("{\"protocol\":1,\"device_code\":\"") + DEVICE_CODE +
                        "\",\"user_code\":\"ABCDEFGH\",\"expires_in\":600,\"interval\":10,"
                        "\"first_poll_after\":5}");
  pocket::PairingClient client(gateway);
  pocket::PairingStartResponse response{};
  std::atomic<bool> cancelled{false};

  const auto outcome = client.start(identityForFirmware("1.4.1+2f6b79f"), cancelled, response);

  EXPECT_EQ(outcome.result, pocket::PocketClientResult::Success);
  EXPECT_EQ(gateway.method, pocket::GatewayMethod::Post);
  EXPECT_EQ(gateway.path, "/api/device/pocket/v1/pairing/start");
  EXPECT_EQ(gateway.path.find('?'), std::string::npos);
  EXPECT_LT(gateway.body.size(), 256U);
}

TEST(PocketPairingClientTest, AcceptsThirtyTwoCharactersAndRejectsThirtyThreeBeforeTransport) {
  FakeGateway gateway;
  gateway.json(200, std::string("{\"protocol\":1,\"device_code\":\"") + DEVICE_CODE +
                        "\",\"user_code\":\"ABCDEFGH\",\"expires_in\":600,\"interval\":10,"
                        "\"first_poll_after\":5}");
  pocket::PairingClient client(gateway);
  pocket::PairingStartResponse response{};
  std::atomic<bool> cancelled{false};
  constexpr char VALID_32[] = "12345678901234567890123456789012";
  constexpr char INVALID_33[] = "123456789012345678901234567890123";

  const auto accepted = client.start(identityForFirmware(VALID_32), cancelled, response);
  EXPECT_EQ(accepted.result, pocket::PocketClientResult::Success);
  EXPECT_EQ(gateway.calls, 1);

  const auto rejected = client.start(identityForFirmware(INVALID_33), cancelled, response);
  EXPECT_EQ(rejected.result, pocket::PocketClientResult::InvalidResponse);
  EXPECT_EQ(rejected.localValidation, pocket::LocalValidationField::Firmware);
  EXPECT_STREQ(pocket::localValidationFieldName(rejected.localValidation), "firmware");
  EXPECT_EQ(gateway.calls, 1);
}

TEST(PocketPairingClientTest, CompiledIdentityIsBoundedAndReachesTransport) {
  FakeGateway gateway;
  gateway.json(200, std::string("{\"protocol\":1,\"device_code\":\"") + DEVICE_CODE +
                        "\",\"user_code\":\"ABCDEFGH\",\"expires_in\":600,\"interval\":10,"
                        "\"first_poll_after\":5}");
  pocket::PairingClient client(gateway);
  pocket::PairingStartResponse response{};
  std::atomic<bool> cancelled{false};

  static_assert(sizeof(pocket::POCKET_FIRMWARE_BUILD_ID) - 1 <= 32);
  const auto outcome = client.start(pocket::COMPILED_PAIRING_IDENTITY, cancelled, response);

  EXPECT_EQ(outcome.result, pocket::PocketClientResult::Success);
  EXPECT_EQ(gateway.calls, 1);
  EXPECT_NE(gateway.body.find(std::string("\"firmware\":\"") + pocket::POCKET_FIRMWARE_BUILD_ID + "\""),
            std::string::npos);
}

TEST(PocketPairingClientTest, RejectsInvalidProtocolAndModelBeforeTransport) {
  FakeGateway gateway;
  pocket::PairingClient client(gateway);
  pocket::PairingStartResponse response{};
  std::atomic<bool> cancelled{false};

  const auto protocol = client.start({2, pocket::POCKET_DEVICE_MODEL, "1.4.1+406b0d6"}, cancelled, response);
  EXPECT_EQ(protocol.localValidation, pocket::LocalValidationField::Protocol);
  const auto model = client.start({1, "other-model", "1.4.1+406b0d6"}, cancelled, response);
  EXPECT_EQ(model.localValidation, pocket::LocalValidationField::Model);
  EXPECT_EQ(gateway.calls, 0);
}

TEST(PocketPairingClientTest, DeviceCodeAndBearerNeverAppearInUrls) {
  FakeGateway gateway;
  gateway.json(200, "{\"protocol\":1,\"status\":\"pending\"}");
  pocket::PairingClient client(gateway);
  pocket::PairingPollResponse poll{};
  std::atomic<bool> cancelled{false};
  EXPECT_EQ(client.poll(DEVICE_CODE, cancelled, poll).result, pocket::PocketClientResult::Pending);
  EXPECT_EQ(gateway.path.find(DEVICE_CODE), std::string::npos);
  EXPECT_NE(gateway.body.find(DEVICE_CODE), std::string::npos);

  gateway.json(200, std::string("{\"protocol\":1,\"device_id\":\"") + UUID +
                        "\",\"name\":\"Laino Pocket (X3)\",\"scope\":\"pocket.device.read\","
                        "\"status\":\"active\"}");
  pocket::PocketSelfResponse self{};
  EXPECT_EQ(client.self(TOKEN, cancelled, self).result, pocket::PocketClientResult::Success);
  EXPECT_EQ(gateway.path.find(TOKEN), std::string::npos);
  EXPECT_EQ(gateway.bearer, TOKEN);
}

TEST(PocketBundleClientTest, UsesExactAuthenticatedPathAndAcceptsNormativeBundle) {
  FakeGateway gateway;
  gateway.json(200,
               R"({"protocolVersion":1,"cards":[{"label":"Laino","title":"Today","subtitle":"Ready","lines":["One"]}]})");
  pocket::PairingClient client(gateway);
  std::atomic<bool> cancelled{false};
  char json[pocket::MAX_JSON_DOCUMENT_BYTES + 1]{};
  std::size_t jsonLength = 0;

  const auto outcome = client.bundle(TOKEN, cancelled, json, sizeof(json), jsonLength);

  EXPECT_EQ(outcome.result, pocket::PocketClientResult::Success);
  EXPECT_EQ(gateway.method, pocket::GatewayMethod::Get);
  EXPECT_EQ(gateway.path, "/api/device/pocket/v1/bundle");
  EXPECT_EQ(gateway.path.find(TOKEN), std::string::npos);
  EXPECT_EQ(gateway.bearer, TOKEN);
  EXPECT_EQ(gateway.successCapacity, sizeof(json));
  EXPECT_EQ(jsonLength, gateway.successJson.size());
}

TEST(PocketBundleClientTest, RejectsMoreThanFourCardsAndInvalidOutputCapacity) {
  FakeGateway gateway;
  const std::string card = R"({"label":"L","title":"T","subtitle":"S","lines":[]})";
  gateway.json(200, std::string("{\"protocolVersion\":1,\"cards\":[") + card + "," + card + "," + card +
                        "," + card + "," + card + "]}");
  pocket::PairingClient client(gateway);
  std::atomic<bool> cancelled{false};
  char json[pocket::MAX_JSON_DOCUMENT_BYTES + 1]{};
  std::size_t jsonLength = 999;

  EXPECT_EQ(client.bundle(TOKEN, cancelled, json, sizeof(json), jsonLength).result,
            pocket::PocketClientResult::InvalidResponse);
  EXPECT_EQ(jsonLength, 0U);
  EXPECT_EQ(client.bundle(TOKEN, cancelled, json, sizeof(json) - 1, jsonLength).result,
            pocket::PocketClientResult::InvalidResponse);
}

TEST(PocketBundleClientTest, PreservesNormativeRevocationClassification) {
  FakeGateway gateway;
  gateway.json(401, R"({"error":"device_revoked"})");
  pocket::PairingClient client(gateway);
  std::atomic<bool> cancelled{false};
  char json[pocket::MAX_JSON_DOCUMENT_BYTES + 1]{};
  std::size_t jsonLength = 0;

  EXPECT_EQ(client.bundle(TOKEN, cancelled, json, sizeof(json), jsonLength).result,
            pocket::PocketClientResult::Revoked);
  EXPECT_EQ(jsonLength, 0U);
}

TEST(PocketPairingClientTest, ClassifiesRateLimitConsumedRevokedAndMalformedErrors) {
  FakeGateway gateway;
  pocket::PairingClient client(gateway);
  std::atomic<bool> cancelled{false};
  pocket::PairingPollResponse poll{};
  gateway.json(429, "{\"error\":\"rate_limited\"}", 44);
  auto result = client.poll(DEVICE_CODE, cancelled, poll);
  EXPECT_EQ(result.result, pocket::PocketClientResult::RateLimited);
  EXPECT_EQ(result.retryAfter, 44);
  gateway.json(429, "{\"error\":\"rate_limited\"}");
  EXPECT_EQ(client.poll(DEVICE_CODE, cancelled, poll).result, pocket::PocketClientResult::InvalidResponse);

  pocket::PairingFinalizeResponse finalized{};
  gateway.json(410, "{\"error\":\"consumed\"}");
  EXPECT_EQ(client.finalize(DEVICE_CODE, true, cancelled, finalized).result, pocket::PocketClientResult::Consumed);

  pocket::PocketSelfResponse self{};
  gateway.json(401, "{\"error\":\"device_revoked\"}");
  EXPECT_EQ(client.self(TOKEN, cancelled, self).result, pocket::PocketClientResult::Revoked);
  gateway.json(502, "<html>challenge</html>");
  EXPECT_EQ(client.self(TOKEN, cancelled, self).result, pocket::PocketClientResult::InvalidResponse);
}

TEST(PocketPairingClientTest, RejectFinalizeRequiresEmpty204AndDeleteRequiresRevokedJson) {
  FakeGateway gateway;
  pocket::PairingClient client(gateway);
  std::atomic<bool> cancelled{false};
  pocket::PairingFinalizeResponse finalized{};
  gateway.json(204, "");
  EXPECT_EQ(client.finalize(DEVICE_CODE, false, cancelled, finalized).result, pocket::PocketClientResult::Success);

  gateway.json(200, "{\"protocol\":1,\"status\":\"revoked\"}");
  EXPECT_EQ(client.removeSelf(TOKEN, cancelled).result, pocket::PocketClientResult::Success);
  gateway.json(200, "{\"protocol\":1,\"status\":\"active\"}");
  EXPECT_EQ(client.removeSelf(TOKEN, cancelled).result, pocket::PocketClientResult::InvalidResponse);
}

TEST(PocketPairingClientTest, PropagatesCancellationWithoutParsingResponse) {
  FakeGateway gateway;
  gateway.next.transport = pocket::GatewayTransportResult::Cancelled;
  pocket::PairingClient client(gateway);
  pocket::PairingPollResponse response{};
  std::atomic<bool> cancelled{true};
  EXPECT_EQ(client.poll(DEVICE_CODE, cancelled, response).result, pocket::PocketClientResult::Cancelled);
}

TEST(PocketPairingClientTest, AcceptsOnlyExactStatusAndNormativeErrorCodePairs) {
  struct Case {
    int status;
    const char* error;
    pocket::PocketClientResult expected;
    uint8_t retryAfter;
  };
  constexpr Case CASES[] = {
      {400, "expired_or_unknown", pocket::PocketClientResult::ExpiredOrUnknown, 0},
      {400, "invalid_request", pocket::PocketClientResult::HttpFailure, 0},
      {400, "unsupported_protocol", pocket::PocketClientResult::HttpFailure, 0},
      {401, "unauthorized", pocket::PocketClientResult::Unauthorized, 0},
      {401, "device_revoked", pocket::PocketClientResult::Revoked, 0},
      {403, "scope_denied", pocket::PocketClientResult::ScopeDenied, 0},
      {403, "principal_not_allowed", pocket::PocketClientResult::PrincipalNotAllowed, 0},
      {409, "not_claimed", pocket::PocketClientResult::NotClaimed, 0},
      {410, "consumed", pocket::PocketClientResult::Consumed, 0},
      {413, "body_too_large", pocket::PocketClientResult::BodyTooLarge, 0},
      {415, "unsupported_media_type", pocket::PocketClientResult::UnsupportedMediaType, 0},
      {429, "rate_limited", pocket::PocketClientResult::RateLimited, 9},
      {503, "server_busy", pocket::PocketClientResult::ServerBusy, 0},
  };
  FakeGateway gateway;
  pocket::PairingClient client(gateway);
  pocket::PairingPollResponse response{};
  std::atomic<bool> cancelled{false};
  for (const Case& item : CASES) {
    gateway.json(item.status, std::string("{\"error\":\"") + item.error + "\"}", item.retryAfter);
    EXPECT_EQ(client.poll(DEVICE_CODE, cancelled, response).result, item.expected)
        << item.status << " " << item.error;
  }

  gateway.json(500, "{\"error\":\"device_revoked\"}");
  EXPECT_EQ(client.poll(DEVICE_CODE, cancelled, response).result, pocket::PocketClientResult::InvalidResponse);
  gateway.json(401, "{\"error\":\"scope_denied\"}");
  EXPECT_EQ(client.poll(DEVICE_CODE, cancelled, response).result, pocket::PocketClientResult::InvalidResponse);
  gateway.json(403, "{\"error\":\"device_revoked\"}");
  EXPECT_EQ(client.poll(DEVICE_CODE, cancelled, response).result, pocket::PocketClientResult::InvalidResponse);
  gateway.json(200, "{\"error\":\"unauthorized\"}");
  EXPECT_EQ(client.poll(DEVICE_CODE, cancelled, response).result, pocket::PocketClientResult::InvalidResponse);
}

TEST(PocketPairingClientTest, RequiresEndpointSpecificSuccessStatuses) {
  FakeGateway gateway;
  pocket::PairingClient client(gateway);
  std::atomic<bool> cancelled{false};
  pocket::PairingStartResponse start{};
  gateway.json(201, std::string("{\"protocol\":1,\"device_code\":\"") + DEVICE_CODE +
                        "\",\"user_code\":\"ABCDEFGH\",\"expires_in\":600,\"interval\":10,"
                        "\"first_poll_after\":5}");
  EXPECT_EQ(client.start(identityForFirmware("1.4.1"), cancelled, start).result,
            pocket::PocketClientResult::InvalidResponse);

  pocket::PairingPollResponse poll{};
  gateway.json(202, "{\"protocol\":1,\"status\":\"pending\"}");
  EXPECT_EQ(client.poll(DEVICE_CODE, cancelled, poll).result, pocket::PocketClientResult::InvalidResponse);

  pocket::PocketSelfResponse self{};
  gateway.json(201, std::string("{\"protocol\":1,\"device_id\":\"") + UUID +
                        "\",\"name\":\"Laino Pocket (X3)\",\"scope\":\"pocket.device.read\","
                        "\"status\":\"active\"}");
  EXPECT_EQ(client.self(TOKEN, cancelled, self).result, pocket::PocketClientResult::InvalidResponse);
  gateway.json(204, "");
  EXPECT_EQ(client.removeSelf(TOKEN, cancelled).result, pocket::PocketClientResult::InvalidResponse);
}

TEST(PocketPairingWorkerTest, LifecycleWaitsForReleaseAndHandlesLaunchFailure) {
  pocket::WorkerLifecycle lifecycle;
  EXPECT_TRUE(lifecycle.ownerMayFinish());
  ASSERT_TRUE(lifecycle.begin());
  EXPECT_FALSE(lifecycle.ownerMayFinish());
  lifecycle.requestCancel();
  EXPECT_EQ(lifecycle.phase(), pocket::WorkerPhase::Cancelling);
  EXPECT_FALSE(lifecycle.ownerMayFinish());
  lifecycle.release();
  EXPECT_TRUE(lifecycle.ownerMayFinish());

  pocket::WorkerLifecycle failed;
  ASSERT_TRUE(failed.begin());
  failed.launchFailed();
  EXPECT_EQ(failed.phase(), pocket::WorkerPhase::StartFailed);
  EXPECT_TRUE(failed.ownerMayFinish());
}

TEST(PocketPairingWorkerTest, UnpairOnlyTreatsThreeDefinedOutcomesAsRemoteResolution) {
  EXPECT_EQ(pocket::classifyUnpairResult(pocket::PocketClientResult::Success),
            pocket::UnpairDisposition::RemoteConfirmed);
  EXPECT_EQ(pocket::classifyUnpairResult(pocket::PocketClientResult::Unauthorized),
            pocket::UnpairDisposition::TokenInvalidOrRevoked);
  EXPECT_EQ(pocket::classifyUnpairResult(pocket::PocketClientResult::Revoked),
            pocket::UnpairDisposition::TokenInvalidOrRevoked);
  EXPECT_EQ(pocket::classifyUnpairResult(pocket::PocketClientResult::TransportFailure),
            pocket::UnpairDisposition::OfferLocalOnly);
  EXPECT_EQ(pocket::classifyUnpairResult(pocket::PocketClientResult::ServerBusy),
            pocket::UnpairDisposition::OfferLocalOnly);
  EXPECT_EQ(pocket::classifyUnpairResult(pocket::PocketClientResult::InvalidResponse),
            pocket::UnpairDisposition::OfferLocalOnly);
}

TEST(PocketPairingWorkerTest, WorkerContextOutlivesOwnerUntilOrderedTeardown) {
  auto* context = new pocket::PairingWorkerContext();
  ASSERT_TRUE(context->lifecycle.begin());
  context->addReference();  // The real task acquires this before xTaskCreate.
  context->releaseReference();  // Simulate an externally destroyed Activity owner.
  context->cancelled.store(true, std::memory_order_release);
  context->lifecycle.requestCancel();
  context->outcome.result = pocket::PocketClientResult::Cancelled;
  context->lifecycle.release();
  EXPECT_TRUE(context->lifecycle.ownerMayFinish());
  context->releaseReference();  // Worker is the last owner and securely destroys the context.
}

TEST(PocketBundleWorkerTest, LargeResponseContextOutlivesActivityOwnerUntilWorkerRelease) {
  auto* context = new pocket::BundleWorkerContext();
  ASSERT_TRUE(context->lifecycle.begin());
  context->addReference();
  context->releaseReference();
  context->cancelled.store(true, std::memory_order_release);
  context->lifecycle.requestCancel();
  context->outcome.result = pocket::PocketClientResult::Cancelled;
  context->lifecycle.release();
  EXPECT_TRUE(context->lifecycle.ownerMayFinish());
  context->releaseReference();
}

TEST(PocketPairingTransportPolicyTest, CancellationAndTotalDeadlineUseProductionCheckpoints) {
  constexpr pocket::TransportCheckpoint CHECKPOINTS[] = {
      pocket::TransportCheckpoint::BeforeRequest, pocket::TransportCheckpoint::DnsWait,
      pocket::TransportCheckpoint::ResponseRead, pocket::TransportCheckpoint::BetweenPolls};
  for (const auto checkpoint : CHECKPOINTS) {
    EXPECT_EQ(pocket::checkTransportControl(checkpoint, true, 0),
              pocket::TransportControlDecision::Cancelled);
  }
  EXPECT_EQ(pocket::checkTransportControl(pocket::TransportCheckpoint::ResponseRead, false, 12999),
            pocket::TransportControlDecision::Continue);
  EXPECT_EQ(pocket::checkTransportControl(pocket::TransportCheckpoint::ResponseRead, false, 13000),
            pocket::TransportControlDecision::TimedOut);
}

TEST(PocketPairingTransportPolicyTest, RejectsRedirectAmbiguousFramingOversizeAndBadType) {
  pocket::ResponseEnvelope envelope{};
  envelope.status = 302;
  EXPECT_EQ(pocket::validateResponseEnvelope(envelope), pocket::GatewayTransportResult::RedirectRejected);
  envelope = {200, true, true, true, true, 20};
  EXPECT_EQ(pocket::validateResponseEnvelope(envelope), pocket::GatewayTransportResult::MalformedHttp);
  envelope = {200, true, false, true, true, 1025};
  EXPECT_EQ(pocket::validateResponseEnvelope(envelope), pocket::GatewayTransportResult::OversizedResponse);
  envelope = {200, true, false, true, false, 20};
  EXPECT_EQ(pocket::validateResponseEnvelope(envelope), pocket::GatewayTransportResult::UnsupportedContentType);
  envelope = {204, false, true, false, false, -1};
  EXPECT_EQ(pocket::validateResponseEnvelope(envelope), pocket::GatewayTransportResult::MalformedHttp);
  envelope = {200, true, false, true, true, 1024};
  EXPECT_EQ(pocket::validateResponseEnvelope(envelope), pocket::GatewayTransportResult::Success);
}

TEST(PocketPairingTransportPolicyTest, LargeSuccessBodyIsExplicitlyBoundedToSnapshotContract) {
  const pocket::ResponseEnvelope exact{
      200, true, false, true, true, static_cast<int32_t>(pocket::POCKET_MAX_LARGE_RESPONSE_BODY_BYTES)};
  const pocket::ResponseEnvelope oversized{
      200, true, false, true, true, static_cast<int32_t>(pocket::POCKET_MAX_LARGE_RESPONSE_BODY_BYTES + 1)};
  EXPECT_EQ(pocket::validateResponseEnvelope(exact, pocket::POCKET_MAX_LARGE_RESPONSE_BODY_BYTES),
            pocket::GatewayTransportResult::Success);
  EXPECT_EQ(pocket::validateResponseEnvelope(oversized, pocket::POCKET_MAX_LARGE_RESPONSE_BODY_BYTES),
            pocket::GatewayTransportResult::OversizedResponse);
  EXPECT_EQ(pocket::validateResponseEnvelope(exact, pocket::POCKET_MAX_LARGE_RESPONSE_BODY_BYTES + 1),
            pocket::GatewayTransportResult::OversizedResponse);
}

TEST(PocketPairingTransportPolicyTest, AllowsBoundedCloudflareResponseMetadataLines) {
  // Cloudflare Report-To lines observed in production reached 260 bytes. The
  // transport still applies the independent 8 KiB aggregate metadata limit.
  EXPECT_GE(pocket::POCKET_MAX_RESPONSE_LINE_BYTES, 512U);
  EXPECT_LT(pocket::POCKET_MAX_RESPONSE_LINE_BYTES, 8192U);
}

TEST(PocketPairingLoggingTest, LogStatementsNeverInterpolatePairingSecrets) {
  std::ifstream input(PAIRING_ACTIVITY_SOURCE);
  ASSERT_TRUE(input.is_open());
  const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  constexpr const char* FORBIDDEN[] = {"deviceCode", "userCode",         "accessToken",
                                       "bearer",     "credential.token", "Authorization"};
  std::size_t position = 0;
  while ((position = source.find("LOG_", position)) != std::string::npos) {
    const std::size_t end = source.find(");", position);
    ASSERT_NE(end, std::string::npos);
    const std::string statement = source.substr(position, end + 2 - position);
    for (const char* forbidden : FORBIDDEN) EXPECT_EQ(statement.find(forbidden), std::string::npos);
    position = end + 2;
  }
}

TEST(PocketBundleLoggingTest, SyncLogStatementsNeverInterpolateCredentialOrResponseData) {
  std::ifstream input(POCKET_ACTIVITY_SOURCE);
  ASSERT_TRUE(input.is_open());
  const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  constexpr const char* FORBIDDEN[] = {"bearer", "credential.token", "context->json", "context->credential"};
  std::size_t position = 0;
  while ((position = source.find("LOG_", position)) != std::string::npos) {
    const std::size_t end = source.find(");", position);
    ASSERT_NE(end, std::string::npos);
    const std::string statement = source.substr(position, end + 2 - position);
    for (const char* forbidden : FORBIDDEN) EXPECT_EQ(statement.find(forbidden), std::string::npos);
    position = end + 2;
  }
}

TEST(PocketInteractionTest, SyncIsAVisibleFrontButtonAndSideButtonsOwnListNavigation) {
  std::ifstream input(POCKET_ACTIVITY_SOURCE);
  ASSERT_TRUE(input.is_open());
  const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

  EXPECT_EQ(source.find("getHeldTime"), std::string::npos);
  EXPECT_EQ(source.find("LONG_PRESS"), std::string::npos);
  EXPECT_NE(source.find("mappedInput.wasReleased(MappedInputManager::Button::Right)"), std::string::npos);
  EXPECT_NE(source.find("mappedInput.wasReleased(MappedInputManager::Button::Up)"), std::string::npos);
  EXPECT_NE(source.find("mappedInput.wasReleased(MappedInputManager::Button::Down)"), std::string::npos);
  EXPECT_NE(source.find("const char* syncLabel"), std::string::npos);
  EXPECT_NE(source.find("tr(STR_POCKET_SYNC)"), std::string::npos);
  EXPECT_NE(source.find("GUI.drawSideButtonHints"), std::string::npos);
}

TEST(PocketPairingRenderingTest, UnpairWarningIsWrappedInsideTheDisplayInsteadOfDrawnAsOneLine) {
  std::ifstream input(PAIRING_ACTIVITY_SOURCE);
  ASSERT_TRUE(input.is_open());
  const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  const std::size_t drawStateBody = source.find("void PocketPairingActivity::drawStateBody");
  ASSERT_NE(drawStateBody, std::string::npos);
  const std::size_t state = source.find("case pocket::PairingState::UnpairConfirm:", drawStateBody);
  ASSERT_NE(state, std::string::npos);
  const std::size_t stateEnd = source.find("break;", state);
  ASSERT_NE(stateEnd, std::string::npos);
  const std::string renderCase = source.substr(state, stateEnd - state);

  EXPECT_NE(renderCase.find("renderer.wrappedText"), std::string::npos);
  EXPECT_NE(renderCase.find("renderer.getScreenWidth() - 40"), std::string::npos);
  EXPECT_NE(renderCase.find("std::replace(warning.begin(), warning.end(), '\\n', ' ')"), std::string::npos);
  EXPECT_NE(renderCase.find("for (const auto& wrappedLine : lines)"), std::string::npos);
}

TEST(PocketPairingInteractionTest, UnpairConfirmationConnectsBeforeRemoteDelete) {
  std::ifstream input(PAIRING_ACTIVITY_SOURCE);
  ASSERT_TRUE(input.is_open());
  const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  const std::size_t handler = source.find("void PocketPairingActivity::handleConfirm");
  ASSERT_NE(handler, std::string::npos);
  const std::size_t state = source.find("case pocket::PairingState::UnpairConfirm:", handler);
  ASSERT_NE(state, std::string::npos);
  const std::size_t stateEnd = source.find("break;", state);
  ASSERT_NE(stateEnd, std::string::npos);
  const std::string confirmCase = source.substr(state, stateEnd - state);

  EXPECT_NE(confirmCase.find("requestOperationWithWifi(pocket::WorkerOperation::DeleteSelf)"),
            std::string::npos);
  EXPECT_EQ(confirmCase.find("credentialStore.clear()"), std::string::npos);
}
