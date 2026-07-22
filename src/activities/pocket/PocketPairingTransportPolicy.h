#pragma once

#include <cstddef>
#include <cstdint>

#include "PocketPairingClient.h"

namespace pocket {

// Cloudflare's dynamic Report-To header can exceed 256 bytes. Keep the
// per-line bound finite while leaving enough room for edge-generated metadata.
inline constexpr std::size_t POCKET_MAX_RESPONSE_LINE_BYTES = 512;

inline constexpr uint32_t POCKET_TOTAL_REQUEST_TIMEOUT_MS = 13000;
inline constexpr uint32_t POCKET_MAX_RESPONSE_BODY_BYTES = 1024;
inline constexpr uint32_t POCKET_MAX_LARGE_RESPONSE_BODY_BYTES = 16384;

enum class TransportCheckpoint : uint8_t { BeforeRequest, DnsWait, ResponseRead, BetweenPolls };
enum class TransportControlDecision : uint8_t { Continue, Cancelled, TimedOut };

TransportControlDecision checkTransportControl(TransportCheckpoint checkpoint, bool cancelled, uint32_t elapsedMs,
                                               uint32_t timeoutMs = POCKET_TOTAL_REQUEST_TIMEOUT_MS);

struct ResponseEnvelope {
  int16_t status = 0;
  bool hasContentLength = false;
  bool hasTransferEncoding = false;
  bool hasContentType = false;
  bool jsonContentType = false;
  int32_t contentLength = -1;
};

GatewayTransportResult validateResponseEnvelope(const ResponseEnvelope& envelope,
                                                uint32_t maximumBodyBytes = POCKET_MAX_RESPONSE_BODY_BYTES);

}  // namespace pocket
