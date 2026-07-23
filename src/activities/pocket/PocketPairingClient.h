#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "PocketFirmwareIdentity.h"
#include "PocketPairingProtocol.h"
#include "PocketReading.h"
#include "PocketSnapshot.h"

namespace pocket {

enum class GatewayMethod : uint8_t { Get, Post, Delete };
enum class GatewaySuccessMediaType : uint8_t { Json, Epub };

class GatewayBodySink {
 public:
  virtual ~GatewayBodySink() = default;
  virtual bool begin(uint32_t contentLength) = 0;
  virtual bool write(const uint8_t* data, size_t length) = 0;
  virtual bool finish() = 0;
  virtual void abort() = 0;
};

enum class GatewayTransportResult : uint8_t {
  Success,
  NoWifi,
  Cancelled,
  LowMemory,
  DnsOrConnectFailure,
  TlsFailure,
  Timeout,
  WriteFailure,
  MalformedHttp,
  RedirectRejected,
  UnsupportedContentType,
  OversizedResponse,
  ReadFailure,
  StorageFailure,
};

struct GatewayRequest {
  GatewayMethod method = GatewayMethod::Get;
  const char* path = nullptr;
  const char* jsonBody = nullptr;
  std::size_t jsonBodyLength = 0;
  const char* bearer = nullptr;
  // Successful large responses may use caller-owned storage. Non-2xx JSON errors always stay in GatewayResponse.
  char* successBody = nullptr;
  std::size_t successBodyCapacity = 0;
  // Large binary success bodies stream directly to caller-owned storage.
  // Non-2xx JSON errors always remain bounded by GatewayResponse::body.
  GatewayBodySink* successSink = nullptr;
  uint32_t successSinkMaximumBytes = 0;
  GatewaySuccessMediaType successMediaType = GatewaySuccessMediaType::Json;
  uint32_t timeoutMs = 13000;
};

struct GatewayResponse {
  GatewayTransportResult transport = GatewayTransportResult::ReadFailure;
  int16_t httpStatus = 0;
  uint8_t retryAfter = 0;
  char body[1025]{};
  uint16_t bodyLength = 0;
  uint32_t elapsedMs = 0;
  int32_t transportError = 0;
};

class PocketGatewayTransport {
 public:
  virtual ~PocketGatewayTransport() = default;
  virtual void request(const GatewayRequest& request, const std::atomic<bool>& cancelled,
                       GatewayResponse& response) = 0;
};

enum class PocketClientResult : uint8_t {
  Success,
  Pending,
  Claimed,
  RateLimited,
  ExpiredOrUnknown,
  NotClaimed,
  Consumed,
  Unauthorized,
  Revoked,
  ScopeDenied,
  PrincipalNotAllowed,
  BodyTooLarge,
  UnsupportedMediaType,
  ServerBusy,
  Cancelled,
  TransportFailure,
  InvalidResponse,
  HttpFailure,
};

enum class LocalValidationField : uint8_t { None, Protocol, Model, Firmware };

struct PocketClientOutcome {
  PocketClientResult result = PocketClientResult::TransportFailure;
  int16_t httpStatus = 0;
  uint8_t retryAfter = 0;
  GatewayTransportResult transport = GatewayTransportResult::ReadFailure;
  uint32_t elapsedMs = 0;
  int32_t transportError = 0;
  char error[33]{};
  LocalValidationField localValidation = LocalValidationField::None;
};

class PairingClient {
 public:
  explicit PairingClient(PocketGatewayTransport& transport) : transport(transport) {}

  PocketClientOutcome start(const PairingIdentity& identity, const std::atomic<bool>& cancelled,
                            PairingStartResponse& response);
  PocketClientOutcome poll(const char* deviceCode, const std::atomic<bool>& cancelled, PairingPollResponse& response);
  PocketClientOutcome finalize(const char* deviceCode, bool confirm, const std::atomic<bool>& cancelled,
                               PairingFinalizeResponse& response);
  PocketClientOutcome self(const char* bearer, const std::atomic<bool>& cancelled, PocketSelfResponse& response);
  PocketClientOutcome removeSelf(const char* bearer, const std::atomic<bool>& cancelled);
  PocketClientOutcome bundle(const char* bearer, const std::atomic<bool>& cancelled, char* json,
                             std::size_t jsonCapacity, std::size_t& jsonLength);
  PocketClientOutcome snapshot(const char* bearer, const std::atomic<bool>& cancelled, char* json,
                               std::size_t jsonCapacity, std::size_t& jsonLength);
  PocketClientOutcome readingManifest(const char* bearer, const std::atomic<bool>& cancelled, char* json,
                                      std::size_t jsonCapacity, std::size_t& jsonLength);
  PocketClientOutcome readingContent(const char* bearer, const ReadingItem& item, GatewayBodySink& sink,
                                     const std::atomic<bool>& cancelled);

 private:
  PocketGatewayTransport& transport;
  PocketClientOutcome execute(const GatewayRequest& request, const std::atomic<bool>& cancelled,
                              GatewayResponse& response);
  static PocketClientOutcome classifyError(const GatewayResponse& response);
};

const char* pocketClientResultName(PocketClientResult result);
const char* gatewayTransportResultName(GatewayTransportResult result);
const char* localValidationFieldName(LocalValidationField field);

}  // namespace pocket
