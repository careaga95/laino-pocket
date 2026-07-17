#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "PocketPairingProtocol.h"

namespace pocket {

enum class GatewayMethod : uint8_t { Get, Post, Delete };
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
};

struct GatewayRequest {
  GatewayMethod method = GatewayMethod::Get;
  const char* path = nullptr;
  const char* jsonBody = nullptr;
  std::size_t jsonBodyLength = 0;
  const char* bearer = nullptr;
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

struct PocketClientOutcome {
  PocketClientResult result = PocketClientResult::TransportFailure;
  int16_t httpStatus = 0;
  uint8_t retryAfter = 0;
  GatewayTransportResult transport = GatewayTransportResult::ReadFailure;
  uint32_t elapsedMs = 0;
  int32_t transportError = 0;
  char error[33]{};
};

class PairingClient {
 public:
  explicit PairingClient(PocketGatewayTransport& transport) : transport(transport) {}

  PocketClientOutcome start(const char* firmware, const std::atomic<bool>& cancelled, PairingStartResponse& response);
  PocketClientOutcome poll(const char* deviceCode, const std::atomic<bool>& cancelled, PairingPollResponse& response);
  PocketClientOutcome finalize(const char* deviceCode, bool confirm, const std::atomic<bool>& cancelled,
                               PairingFinalizeResponse& response);
  PocketClientOutcome self(const char* bearer, const std::atomic<bool>& cancelled, PocketSelfResponse& response);
  PocketClientOutcome removeSelf(const char* bearer, const std::atomic<bool>& cancelled);

 private:
  PocketGatewayTransport& transport;
  PocketClientOutcome execute(const GatewayRequest& request, const std::atomic<bool>& cancelled,
                              GatewayResponse& response);
  static PocketClientOutcome classifyError(const GatewayResponse& response);
};

const char* pocketClientResultName(PocketClientResult result);
const char* gatewayTransportResultName(GatewayTransportResult result);

}  // namespace pocket
