#include "PocketPairingTransportPolicy.h"

namespace pocket {

TransportControlDecision checkTransportControl(const TransportCheckpoint, const bool cancelled,
                                               const uint32_t elapsedMs, const uint32_t timeoutMs) {
  if (cancelled) return TransportControlDecision::Cancelled;
  if (elapsedMs >= timeoutMs) return TransportControlDecision::TimedOut;
  return TransportControlDecision::Continue;
}

GatewayTransportResult validateResponseEnvelope(const ResponseEnvelope& envelope) {
  if (envelope.status >= 300 && envelope.status <= 399) return GatewayTransportResult::RedirectRejected;
  if (envelope.hasContentLength && envelope.hasTransferEncoding) return GatewayTransportResult::MalformedHttp;
  if (envelope.contentLength > static_cast<int32_t>(POCKET_MAX_RESPONSE_BODY_BYTES)) {
    return GatewayTransportResult::OversizedResponse;
  }
  if (envelope.status == 204) {
    if (envelope.hasTransferEncoding || envelope.contentLength > 0) return GatewayTransportResult::MalformedHttp;
    return GatewayTransportResult::Success;
  }
  if (!envelope.hasContentType || !envelope.jsonContentType) {
    return GatewayTransportResult::UnsupportedContentType;
  }
  return GatewayTransportResult::Success;
}

}  // namespace pocket
