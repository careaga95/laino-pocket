#include "PocketPairingTransportPolicy.h"

namespace pocket {

TransportControlDecision checkTransportControl(const TransportCheckpoint, const bool cancelled,
                                               const uint32_t elapsedMs, const uint32_t timeoutMs) {
  if (cancelled) return TransportControlDecision::Cancelled;
  if (elapsedMs >= timeoutMs) return TransportControlDecision::TimedOut;
  return TransportControlDecision::Continue;
}

GatewayTransportResult validateResponseEnvelope(const ResponseEnvelope& envelope, const uint32_t maximumBodyBytes) {
  if (envelope.status >= 300 && envelope.status <= 399) return GatewayTransportResult::RedirectRejected;
  if (envelope.hasContentLength && envelope.hasTransferEncoding) return GatewayTransportResult::MalformedHttp;
  if (maximumBodyBytes > POCKET_MAX_LARGE_RESPONSE_BODY_BYTES ||
      envelope.contentLength > static_cast<int32_t>(maximumBodyBytes)) {
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

GatewayTransportResult validateBinaryResponseEnvelope(const ResponseEnvelope& envelope,
                                                      const uint32_t maximumBodyBytes) {
  if (envelope.status >= 300 && envelope.status <= 399) return GatewayTransportResult::RedirectRejected;
  if (envelope.status < 200 || envelope.status > 299) {
    return validateResponseEnvelope(envelope);
  }
  if (envelope.status != 200 || envelope.hasTransferEncoding || !envelope.hasContentLength ||
      envelope.contentLength <= 0) {
    return GatewayTransportResult::MalformedHttp;
  }
  if (maximumBodyBytes == 0 || maximumBodyBytes > POCKET_MAX_BINARY_RESPONSE_BODY_BYTES ||
      envelope.contentLength > static_cast<int32_t>(maximumBodyBytes)) {
    return GatewayTransportResult::OversizedResponse;
  }
  if (!envelope.hasContentType || !envelope.epubContentType) {
    return GatewayTransportResult::UnsupportedContentType;
  }
  return GatewayTransportResult::Success;
}

}  // namespace pocket
