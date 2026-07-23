#include "PocketPairingClient.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "PocketCardParser.h"
#include "PocketCredential.h"
#include "PocketReadingParser.h"
#include "PocketSnapshotParser.h"

namespace pocket {
namespace {

constexpr char START_PATH[] = "/api/device/pocket/v1/pairing/start";
constexpr char POLL_PATH[] = "/api/device/pocket/v1/pairing/poll";
constexpr char FINALIZE_PATH[] = "/api/device/pocket/v1/pairing/finalize";
constexpr char SELF_PATH[] = "/api/device/pocket/v1/self";
constexpr char BUNDLE_PATH[] = "/api/device/pocket/v1/bundle";
constexpr char SNAPSHOT_PATH[] = "/api/device/pocket/v1/snapshot";
constexpr char READING_PATH[] = "/api/device/pocket/v1/reading";
constexpr uint32_t READING_DOWNLOAD_TIMEOUT_MS = 120000;

PocketClientOutcome baseOutcome(const GatewayResponse& response) {
  PocketClientOutcome outcome{};
  outcome.httpStatus = response.httpStatus;
  outcome.retryAfter = response.retryAfter;
  outcome.transport = response.transport;
  outcome.elapsedMs = response.elapsedMs;
  outcome.transportError = response.transportError;
  return outcome;
}

PocketClientResult resultForError(const int status, const char* error) {
  if (status == 400 && std::strcmp(error, "expired_or_unknown") == 0) return PocketClientResult::ExpiredOrUnknown;
  if (status == 409 && std::strcmp(error, "not_claimed") == 0) return PocketClientResult::NotClaimed;
  if (status == 410 && std::strcmp(error, "consumed") == 0) return PocketClientResult::Consumed;
  if (status == 401 && std::strcmp(error, "unauthorized") == 0) return PocketClientResult::Unauthorized;
  if (status == 401 && std::strcmp(error, "device_revoked") == 0) return PocketClientResult::Revoked;
  if (status == 403 && std::strcmp(error, "scope_denied") == 0) return PocketClientResult::ScopeDenied;
  if (status == 403 && std::strcmp(error, "principal_not_allowed") == 0) {
    return PocketClientResult::PrincipalNotAllowed;
  }
  if (status == 413 && std::strcmp(error, "body_too_large") == 0) return PocketClientResult::BodyTooLarge;
  if (status == 415 && std::strcmp(error, "unsupported_media_type") == 0) {
    return PocketClientResult::UnsupportedMediaType;
  }
  if (status == 503 && std::strcmp(error, "server_busy") == 0) return PocketClientResult::ServerBusy;
  if (status == 429 && std::strcmp(error, "rate_limited") == 0) return PocketClientResult::RateLimited;

  // These are normative protocol errors but do not need a distinct UI branch.
  if (status == 400 &&
      (std::strcmp(error, "invalid_request") == 0 || std::strcmp(error, "unsupported_protocol") == 0)) {
    return PocketClientResult::HttpFailure;
  }

  constexpr const char* KNOWN_CODES[] = {
      "expired_or_unknown",    "not_claimed",    "consumed",
      "unauthorized",          "device_revoked", "scope_denied",
      "principal_not_allowed", "body_too_large", "unsupported_media_type",
      "server_busy",           "rate_limited",   "invalid_request",
      "unsupported_protocol",
  };
  if (std::any_of(std::begin(KNOWN_CODES), std::end(KNOWN_CODES),
                  [error](const char* code) { return std::strcmp(error, code) == 0; })) {
    return PocketClientResult::InvalidResponse;
  }
  return PocketClientResult::HttpFailure;
}

bool validAsciiField(const char* value) {
  if (value == nullptr) return false;
  const std::size_t length = std::strlen(value);
  if (length == 0 || length > 32) return false;
  for (std::size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (c < 0x20 || c > 0x7e || c == '"' || c == '\\') return false;
  }
  return true;
}

PocketClientOutcome invalidLocalField(const LocalValidationField field) {
  PocketClientOutcome outcome{};
  outcome.result = PocketClientResult::InvalidResponse;
  outcome.localValidation = field;
  return outcome;
}

}  // namespace

PocketClientOutcome PairingClient::execute(const GatewayRequest& request, const std::atomic<bool>& cancelled,
                                           GatewayResponse& response) {
  transport.request(request, cancelled, response);
  PocketClientOutcome outcome = baseOutcome(response);
  if (response.transport == GatewayTransportResult::Cancelled) {
    outcome.result = PocketClientResult::Cancelled;
  } else if (response.transport != GatewayTransportResult::Success) {
    outcome.result = PocketClientResult::TransportFailure;
  } else if (response.httpStatus < 200 || response.httpStatus > 299) {
    outcome = classifyError(response);
  } else {
    outcome.result = PocketClientResult::Success;
  }
  return outcome;
}

PocketClientOutcome PairingClient::classifyError(const GatewayResponse& response) {
  PocketClientOutcome outcome = baseOutcome(response);
  if (parsePocketError(response.body, response.bodyLength, outcome.error, sizeof(outcome.error)) !=
      JsonParseResult::Success) {
    outcome.result = PocketClientResult::InvalidResponse;
    return outcome;
  }
  outcome.result = resultForError(response.httpStatus, outcome.error);
  if (response.httpStatus == 429 && outcome.result == PocketClientResult::RateLimited) {
    if (response.retryAfter == 0) {
      outcome.result = PocketClientResult::InvalidResponse;
      return outcome;
    }
    outcome.retryAfter = response.retryAfter;
  }
  return outcome;
}

PocketClientOutcome PairingClient::start(const PairingIdentity& identity, const std::atomic<bool>& cancelled,
                                         PairingStartResponse& response) {
  if (identity.protocol != PAIRING_PROTOCOL_VERSION) return invalidLocalField(LocalValidationField::Protocol);
  if (!validAsciiField(identity.model) || std::strcmp(identity.model, POCKET_DEVICE_MODEL) != 0) {
    return invalidLocalField(LocalValidationField::Model);
  }
  if (!validAsciiField(identity.firmware)) return invalidLocalField(LocalValidationField::Firmware);
  char body[128];
  const int length = std::snprintf(body, sizeof(body), "{\"protocol\":%u,\"model\":\"%s\",\"firmware\":\"%s\"}",
                                   static_cast<unsigned int>(identity.protocol), identity.model, identity.firmware);
  if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(body)) return {PocketClientResult::InvalidResponse};
  GatewayResponse gateway{};
  const GatewayRequest request{GatewayMethod::Post, START_PATH, body, static_cast<std::size_t>(length), nullptr};
  PocketClientOutcome outcome = execute(request, cancelled, gateway);
  if (outcome.result == PocketClientResult::Success) {
    if (gateway.httpStatus != 200 ||
        parsePairingStartResponse(gateway.body, gateway.bodyLength, response) != JsonParseResult::Success) {
      outcome.result = PocketClientResult::InvalidResponse;
    }
  }
  secureClear(body, sizeof(body));
  secureClear(&gateway, sizeof(gateway));
  return outcome;
}

PocketClientOutcome PairingClient::poll(const char* deviceCode, const std::atomic<bool>& cancelled,
                                        PairingPollResponse& response) {
  if (!isBase64Url256(deviceCode, deviceCode == nullptr ? 0 : std::strlen(deviceCode))) {
    return {PocketClientResult::InvalidResponse};
  }
  char body[96];
  const int length = std::snprintf(body, sizeof(body), "{\"protocol\":1,\"device_code\":\"%s\"}", deviceCode);
  if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(body)) return {PocketClientResult::InvalidResponse};
  GatewayResponse gateway{};
  const GatewayRequest request{GatewayMethod::Post, POLL_PATH, body, static_cast<std::size_t>(length), nullptr};
  PocketClientOutcome outcome = execute(request, cancelled, gateway);
  if (outcome.result == PocketClientResult::Success) {
    if (gateway.httpStatus != 200 ||
        parsePairingPollResponse(gateway.body, gateway.bodyLength, response) != JsonParseResult::Success) {
      outcome.result = PocketClientResult::InvalidResponse;
    } else {
      outcome.result =
          response.status == PollStatus::Pending ? PocketClientResult::Pending : PocketClientResult::Claimed;
    }
  }
  secureClear(body, sizeof(body));
  secureClear(&gateway, sizeof(gateway));
  return outcome;
}

PocketClientOutcome PairingClient::finalize(const char* deviceCode, const bool confirm,
                                            const std::atomic<bool>& cancelled, PairingFinalizeResponse& response) {
  if (!isBase64Url256(deviceCode, deviceCode == nullptr ? 0 : std::strlen(deviceCode))) {
    return {PocketClientResult::InvalidResponse};
  }
  char body[128];
  const int length = std::snprintf(body, sizeof(body), "{\"protocol\":1,\"device_code\":\"%s\",\"confirm\":%s}",
                                   deviceCode, confirm ? "true" : "false");
  if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(body)) return {PocketClientResult::InvalidResponse};
  GatewayResponse gateway{};
  const GatewayRequest request{GatewayMethod::Post, FINALIZE_PATH, body, static_cast<std::size_t>(length), nullptr};
  PocketClientOutcome outcome = execute(request, cancelled, gateway);
  if (outcome.result != PocketClientResult::Success) {
    secureClear(body, sizeof(body));
    secureClear(&gateway, sizeof(gateway));
    return outcome;
  }
  if (!confirm) {
    if (gateway.httpStatus != 204 || gateway.bodyLength != 0) outcome.result = PocketClientResult::InvalidResponse;
    secureClear(body, sizeof(body));
    secureClear(&gateway, sizeof(gateway));
    return outcome;
  }
  if (gateway.httpStatus != 200 ||
      parsePairingFinalizeResponse(gateway.body, gateway.bodyLength, response) != JsonParseResult::Success) {
    outcome.result = PocketClientResult::InvalidResponse;
  }
  secureClear(body, sizeof(body));
  secureClear(&gateway, sizeof(gateway));
  return outcome;
}

PocketClientOutcome PairingClient::self(const char* bearer, const std::atomic<bool>& cancelled,
                                        PocketSelfResponse& response) {
  if (!isBase64Url256(bearer, bearer == nullptr ? 0 : std::strlen(bearer))) {
    return {PocketClientResult::InvalidResponse};
  }
  GatewayResponse gateway{};
  const GatewayRequest request{GatewayMethod::Get, SELF_PATH, nullptr, 0, bearer};
  PocketClientOutcome outcome = execute(request, cancelled, gateway);
  if (outcome.result == PocketClientResult::Success) {
    if (gateway.httpStatus != 200 ||
        parsePocketSelfResponse(gateway.body, gateway.bodyLength, response) != JsonParseResult::Success) {
      outcome.result = PocketClientResult::InvalidResponse;
    }
  }
  secureClear(&gateway, sizeof(gateway));
  return outcome;
}

PocketClientOutcome PairingClient::removeSelf(const char* bearer, const std::atomic<bool>& cancelled) {
  if (!isBase64Url256(bearer, bearer == nullptr ? 0 : std::strlen(bearer))) {
    return {PocketClientResult::InvalidResponse};
  }
  GatewayResponse gateway{};
  const GatewayRequest request{GatewayMethod::Delete, SELF_PATH, nullptr, 0, bearer};
  PocketClientOutcome outcome = execute(request, cancelled, gateway);
  if (outcome.result == PocketClientResult::Success) {
    if (gateway.httpStatus != 200 ||
        parsePocketStatusResponse(gateway.body, gateway.bodyLength, "revoked") != JsonParseResult::Success) {
      outcome.result = PocketClientResult::InvalidResponse;
    }
  }
  secureClear(&gateway, sizeof(gateway));
  return outcome;
}

PocketClientOutcome PairingClient::bundle(const char* bearer, const std::atomic<bool>& cancelled, char* json,
                                          const std::size_t jsonCapacity, std::size_t& jsonLength) {
  jsonLength = 0;
  if (!isBase64Url256(bearer, bearer == nullptr ? 0 : std::strlen(bearer)) || json == nullptr ||
      jsonCapacity != MAX_JSON_DOCUMENT_BYTES + 1) {
    return {PocketClientResult::InvalidResponse};
  }
  json[0] = '\0';
  GatewayResponse gateway{};
  const GatewayRequest request{GatewayMethod::Get, BUNDLE_PATH, nullptr, 0, bearer, json, jsonCapacity};
  PocketClientOutcome outcome = execute(request, cancelled, gateway);
  if (outcome.result == PocketClientResult::Success) {
    if (gateway.httpStatus != 200 ||
        validateCardBundle(json, gateway.bodyLength, MAX_REMOTE_CARDS) != ParseResult::Success) {
      outcome.result = PocketClientResult::InvalidResponse;
    } else {
      jsonLength = gateway.bodyLength;
    }
  }
  secureClear(&gateway, sizeof(gateway));
  return outcome;
}

PocketClientOutcome PairingClient::snapshot(const char* bearer, const std::atomic<bool>& cancelled, char* json,
                                            const std::size_t jsonCapacity, std::size_t& jsonLength) {
  jsonLength = 0;
  if (!isBase64Url256(bearer, bearer == nullptr ? 0 : std::strlen(bearer)) || json == nullptr ||
      jsonCapacity != MAX_SNAPSHOT_JSON_BYTES + 1) {
    return {PocketClientResult::InvalidResponse};
  }
  json[0] = '\0';
  GatewayResponse gateway{};
  const GatewayRequest request{GatewayMethod::Get, SNAPSHOT_PATH, nullptr, 0, bearer, json, jsonCapacity};
  PocketClientOutcome outcome = execute(request, cancelled, gateway);
  if (outcome.result == PocketClientResult::Success) {
    if (gateway.httpStatus != 200 || validatePocketSnapshot(json, gateway.bodyLength) != SnapshotParseResult::Success) {
      outcome.result = PocketClientResult::InvalidResponse;
    } else {
      jsonLength = gateway.bodyLength;
    }
  }
  secureClear(&gateway, sizeof(gateway));
  return outcome;
}

PocketClientOutcome PairingClient::readingManifest(const char* bearer, const std::atomic<bool>& cancelled, char* json,
                                                   const std::size_t jsonCapacity, std::size_t& jsonLength) {
  jsonLength = 0;
  if (!isBase64Url256(bearer, bearer == nullptr ? 0 : std::strlen(bearer)) || json == nullptr ||
      jsonCapacity != MAX_READING_MANIFEST_JSON_BYTES + 1) {
    return {PocketClientResult::InvalidResponse};
  }
  json[0] = '\0';
  GatewayResponse gateway{};
  GatewayRequest request{GatewayMethod::Get, READING_PATH, nullptr, 0, bearer, json, jsonCapacity};
  PocketClientOutcome outcome = execute(request, cancelled, gateway);
  if (outcome.result == PocketClientResult::Success) {
    if (gateway.httpStatus != 200 || validateReadingManifest(json, gateway.bodyLength) != ReadingParseResult::Success) {
      outcome.result = PocketClientResult::InvalidResponse;
    } else {
      jsonLength = gateway.bodyLength;
    }
  }
  secureClear(&gateway, sizeof(gateway));
  return outcome;
}

PocketClientOutcome PairingClient::readingContent(const char* bearer, const ReadingItem& item, GatewayBodySink& sink,
                                                  const std::atomic<bool>& cancelled) {
  if (!isBase64Url256(bearer, bearer == nullptr ? 0 : std::strlen(bearer)) || !isReadingUuid(item.id) ||
      item.bytes == 0 || item.bytes > MAX_READING_EPUB_BYTES) {
    return {PocketClientResult::InvalidResponse};
  }
  char path[128];
  const int pathLength = std::snprintf(path, sizeof(path), "/api/device/pocket/v1/reading/%s/content", item.id);
  if (pathLength <= 0 || static_cast<size_t>(pathLength) >= sizeof(path)) {
    return {PocketClientResult::InvalidResponse};
  }
  GatewayResponse gateway{};
  GatewayRequest request{};
  request.method = GatewayMethod::Get;
  request.path = path;
  request.bearer = bearer;
  request.successSink = &sink;
  request.successSinkMaximumBytes = item.bytes;
  request.successMediaType = GatewaySuccessMediaType::Epub;
  request.timeoutMs = READING_DOWNLOAD_TIMEOUT_MS;
  PocketClientOutcome outcome = execute(request, cancelled, gateway);
  if (outcome.result == PocketClientResult::Success && gateway.httpStatus != 200) {
    outcome.result = PocketClientResult::InvalidResponse;
  }
  secureClear(&gateway, sizeof(gateway));
  secureClear(path, sizeof(path));
  return outcome;
}

const char* pocketClientResultName(const PocketClientResult result) {
  switch (result) {
    case PocketClientResult::Success:
      return "success";
    case PocketClientResult::Pending:
      return "pending";
    case PocketClientResult::Claimed:
      return "claimed";
    case PocketClientResult::RateLimited:
      return "rate_limited";
    case PocketClientResult::ExpiredOrUnknown:
      return "expired_or_unknown";
    case PocketClientResult::NotClaimed:
      return "not_claimed";
    case PocketClientResult::Consumed:
      return "consumed";
    case PocketClientResult::Unauthorized:
      return "unauthorized";
    case PocketClientResult::Revoked:
      return "revoked";
    case PocketClientResult::ScopeDenied:
      return "scope_denied";
    case PocketClientResult::PrincipalNotAllowed:
      return "principal_not_allowed";
    case PocketClientResult::BodyTooLarge:
      return "body_too_large";
    case PocketClientResult::UnsupportedMediaType:
      return "unsupported_media_type";
    case PocketClientResult::ServerBusy:
      return "server_busy";
    case PocketClientResult::Cancelled:
      return "cancelled";
    case PocketClientResult::TransportFailure:
      return "transport_failure";
    case PocketClientResult::InvalidResponse:
      return "invalid_response";
    case PocketClientResult::HttpFailure:
      return "http_failure";
  }
  return "unknown";
}

const char* gatewayTransportResultName(const GatewayTransportResult result) {
  switch (result) {
    case GatewayTransportResult::Success:
      return "success";
    case GatewayTransportResult::NoWifi:
      return "no_wifi";
    case GatewayTransportResult::Cancelled:
      return "cancelled";
    case GatewayTransportResult::LowMemory:
      return "low_memory";
    case GatewayTransportResult::DnsOrConnectFailure:
      return "dns_or_connect_failure";
    case GatewayTransportResult::TlsFailure:
      return "tls_failure";
    case GatewayTransportResult::Timeout:
      return "timeout";
    case GatewayTransportResult::WriteFailure:
      return "write_failure";
    case GatewayTransportResult::MalformedHttp:
      return "malformed_http";
    case GatewayTransportResult::RedirectRejected:
      return "redirect_rejected";
    case GatewayTransportResult::UnsupportedContentType:
      return "unsupported_content_type";
    case GatewayTransportResult::OversizedResponse:
      return "oversized_response";
    case GatewayTransportResult::ReadFailure:
      return "read_failure";
    case GatewayTransportResult::StorageFailure:
      return "storage_failure";
  }
  return "unknown";
}

const char* localValidationFieldName(const LocalValidationField field) {
  switch (field) {
    case LocalValidationField::None:
      return "none";
    case LocalValidationField::Protocol:
      return "protocol";
    case LocalValidationField::Model:
      return "model";
    case LocalValidationField::Firmware:
      return "firmware";
  }
  return "unknown";
}

}  // namespace pocket
