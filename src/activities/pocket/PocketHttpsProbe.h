#pragma once

#include <cstddef>
#include <cstdint>

namespace pocket {

constexpr std::size_t HTTPS_PROBE_MAX_RESPONSE_BYTES = 1024;
constexpr std::size_t HTTPS_PROBE_READ_BUFFER_BYTES = 128;
constexpr uint32_t HTTPS_PROBE_TOTAL_TIMEOUT_MS = 13000;

enum class HttpsProbeResultCode : uint8_t {
  Success,
  NoWifi,
  DnsOrConnectFailure,
  TlsFailure,
  Timeout,
  HttpFailure,
  RedirectRejected,
  OversizedResponse,
  ReadFailure,
  InternalFailure,
};

struct HttpsProbeResult {
  HttpsProbeResultCode code = HttpsProbeResultCode::InternalFailure;
  int16_t httpStatus = 0;
  uint16_t responseBytes = 0;
  uint32_t elapsedMs = 0;
  int32_t transportError = 0;
};

enum class HttpsProbeOpenStatus : uint8_t {
  Ready,
  DnsOrConnectFailure,
  TlsFailure,
  Timeout,
  ReadFailure,
  InternalFailure,
};

struct HttpsProbeResponseHead {
  HttpsProbeOpenStatus status = HttpsProbeOpenStatus::InternalFailure;
  int16_t httpStatus = 0;
  // -1 means that HTTP framing did not declare a decoded body length.
  int32_t contentLength = -1;
  int32_t transportError = 0;
};

enum class HttpsProbeReadStatus : uint8_t { Data, End, Timeout, Failure };

struct HttpsProbeReadResult {
  HttpsProbeReadStatus status = HttpsProbeReadStatus::Failure;
  std::size_t bytes = 0;
  int32_t transportError = 0;
};

// Small seam between deterministic response-classification tests and the ESP32 transport.
// Implementations must return decoded HTTP body bytes (chunk framing is not body data).
class HttpsProbeTransport {
 public:
  virtual ~HttpsProbeTransport() = default;
  virtual HttpsProbeResponseHead open() = 0;
  virtual HttpsProbeReadResult readBody(uint8_t* buffer, std::size_t capacity) = 0;
  virtual uint32_t elapsedMs() const = 0;
  virtual void close() = 0;
};

class HttpsProbePresentation {
 public:
  void reset();
  void complete(const HttpsProbeResult& completedResult);

  [[nodiscard]] bool isTesting() const { return testing; }
  [[nodiscard]] const HttpsProbeResult& result() const { return currentResult; }

 private:
  bool testing = true;
  HttpsProbeResult currentResult{};
};

HttpsProbeResult runHttpsProbe(bool wifiConnected, HttpsProbeTransport& transport);
const char* httpsProbeResultName(HttpsProbeResultCode code);
void formatHttpsProbeResult(const HttpsProbeResult& result, char* output, std::size_t outputSize);

}  // namespace pocket
