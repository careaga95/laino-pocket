#include "PocketHttpsProbe.h"

#include <cstdio>

namespace pocket {
namespace {

class TransportCloseGuard {
 public:
  explicit TransportCloseGuard(HttpsProbeTransport& transport) : transport(transport) {}
  ~TransportCloseGuard() { transport.close(); }

 private:
  HttpsProbeTransport& transport;
};

HttpsProbeResult makeResult(const HttpsProbeResultCode code, const HttpsProbeTransport& transport,
                            const int16_t httpStatus = 0, const uint16_t responseBytes = 0,
                            const int32_t transportError = 0) {
  return {code, httpStatus, responseBytes, transport.elapsedMs(), transportError};
}

}  // namespace

void HttpsProbePresentation::reset() {
  testing = true;
  currentResult = {};
}

void HttpsProbePresentation::complete(const HttpsProbeResult& completedResult) {
  currentResult = completedResult;
  testing = false;
}

HttpsProbeResult runHttpsProbe(const bool wifiConnected, HttpsProbeTransport& transport) {
  TransportCloseGuard closeGuard(transport);

  if (!wifiConnected) {
    return makeResult(HttpsProbeResultCode::NoWifi, transport);
  }

  const HttpsProbeResponseHead head = transport.open();
  switch (head.status) {
    case HttpsProbeOpenStatus::DnsOrConnectFailure:
      return makeResult(HttpsProbeResultCode::DnsOrConnectFailure, transport, head.httpStatus, 0, head.transportError);
    case HttpsProbeOpenStatus::TlsFailure:
      return makeResult(HttpsProbeResultCode::TlsFailure, transport, head.httpStatus, 0, head.transportError);
    case HttpsProbeOpenStatus::Timeout:
      return makeResult(HttpsProbeResultCode::Timeout, transport, head.httpStatus, 0, head.transportError);
    case HttpsProbeOpenStatus::ReadFailure:
      return makeResult(HttpsProbeResultCode::ReadFailure, transport, head.httpStatus, 0, head.transportError);
    case HttpsProbeOpenStatus::InternalFailure:
      return makeResult(HttpsProbeResultCode::InternalFailure, transport, head.httpStatus, 0, head.transportError);
    case HttpsProbeOpenStatus::Ready:
      break;
  }

  if (head.httpStatus >= 300 && head.httpStatus <= 399) {
    return makeResult(HttpsProbeResultCode::RedirectRejected, transport, head.httpStatus);
  }
  if (head.httpStatus < 200 || head.httpStatus > 299) {
    return makeResult(HttpsProbeResultCode::HttpFailure, transport, head.httpStatus);
  }
  if (head.contentLength < -1) {
    return makeResult(HttpsProbeResultCode::InternalFailure, transport, head.httpStatus);
  }
  if (head.contentLength > static_cast<int32_t>(HTTPS_PROBE_MAX_RESPONSE_BYTES)) {
    return makeResult(HttpsProbeResultCode::OversizedResponse, transport, head.httpStatus);
  }

  uint8_t buffer[HTTPS_PROBE_READ_BUFFER_BYTES];
  std::size_t total = 0;
  while (true) {
    const std::size_t remainingThroughLimit = HTTPS_PROBE_MAX_RESPONSE_BYTES + 1 - total;
    const std::size_t capacity = remainingThroughLimit < sizeof(buffer) ? remainingThroughLimit : sizeof(buffer);
    const HttpsProbeReadResult read = transport.readBody(buffer, capacity);

    if (read.status == HttpsProbeReadStatus::Timeout) {
      return makeResult(HttpsProbeResultCode::Timeout, transport, head.httpStatus, static_cast<uint16_t>(total),
                        read.transportError);
    }
    if (read.status == HttpsProbeReadStatus::Failure ||
        (read.status == HttpsProbeReadStatus::Data && (read.bytes == 0 || read.bytes > capacity))) {
      return makeResult(HttpsProbeResultCode::ReadFailure, transport, head.httpStatus, static_cast<uint16_t>(total),
                        read.transportError);
    }
    if (read.status == HttpsProbeReadStatus::End) {
      if (head.contentLength >= 0 && total != static_cast<std::size_t>(head.contentLength)) {
        return makeResult(HttpsProbeResultCode::ReadFailure, transport, head.httpStatus, static_cast<uint16_t>(total),
                          read.transportError);
      }
      return makeResult(HttpsProbeResultCode::Success, transport, head.httpStatus, static_cast<uint16_t>(total));
    }

    total += read.bytes;
    if (total > HTTPS_PROBE_MAX_RESPONSE_BYTES) {
      return makeResult(HttpsProbeResultCode::OversizedResponse, transport, head.httpStatus,
                        static_cast<uint16_t>(total));
    }
    if (head.contentLength >= 0 && total > static_cast<std::size_t>(head.contentLength)) {
      return makeResult(HttpsProbeResultCode::ReadFailure, transport, head.httpStatus, static_cast<uint16_t>(total));
    }
  }
}

const char* httpsProbeResultName(const HttpsProbeResultCode code) {
  switch (code) {
    case HttpsProbeResultCode::Success:
      return "Success";
    case HttpsProbeResultCode::NoWifi:
      return "No Wi-Fi";
    case HttpsProbeResultCode::DnsOrConnectFailure:
      return "DNS/connect failed";
    case HttpsProbeResultCode::TlsFailure:
      return "TLS validation failed";
    case HttpsProbeResultCode::Timeout:
      return "Timed out";
    case HttpsProbeResultCode::HttpFailure:
      return "HTTP failed";
    case HttpsProbeResultCode::RedirectRejected:
      return "Redirect rejected";
    case HttpsProbeResultCode::OversizedResponse:
      return "Response too large";
    case HttpsProbeResultCode::ReadFailure:
      return "Response read failed";
    case HttpsProbeResultCode::InternalFailure:
      return "Internal failure";
  }
  return "Internal failure";
}

void formatHttpsProbeResult(const HttpsProbeResult& result, char* output, const std::size_t outputSize) {
  if (output == nullptr || outputSize == 0) {
    return;
  }

  if (result.code == HttpsProbeResultCode::Success) {
    std::snprintf(output, outputSize, "HTTP %d, %u bytes, %lu ms", result.httpStatus, result.responseBytes,
                  static_cast<unsigned long>(result.elapsedMs));
  } else if (result.httpStatus != 0) {
    std::snprintf(output, outputSize, "%s (HTTP %d, %lu ms)", httpsProbeResultName(result.code), result.httpStatus,
                  static_cast<unsigned long>(result.elapsedMs));
  } else if (result.transportError != 0) {
    std::snprintf(output, outputSize, "%s (code %ld, %lu ms)", httpsProbeResultName(result.code),
                  static_cast<long>(result.transportError), static_cast<unsigned long>(result.elapsedMs));
  } else {
    std::snprintf(output, outputSize, "%s (%lu ms)", httpsProbeResultName(result.code),
                  static_cast<unsigned long>(result.elapsedMs));
  }
  output[outputSize - 1] = '\0';
}

}  // namespace pocket
