#include "PocketHttpsProbeTransport.h"

#include <Arduino.h>
#include <IPAddress.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>

namespace pocket {
namespace {

constexpr const char* PROBE_HOST = "example.com";
constexpr uint16_t PROBE_PORT = 443;
constexpr uint32_t DNS_TIMEOUT_MS = 2500;
constexpr int32_t CONNECT_TIMEOUT_MS = 4000;
constexpr uint32_t TLS_HANDSHAKE_TIMEOUT_SECONDS = 5;
constexpr uint32_t RESPONSE_IO_TIMEOUT_MS = 4000;
// Bounds response headers, chunk-size lines, and trailers without rejecting ordinary small-chunk framing.
constexpr std::size_t MAX_HTTP_METADATA_BYTES = 8192;
constexpr std::size_t MAX_LINE_BYTES = 256;
constexpr int32_t TRANSPORT_MALFORMED_HTTP = -20001;
constexpr int32_t TRANSPORT_WRITE_FAILED = -20002;
constexpr int32_t TRANSPORT_DNS_DISPATCH_FAILED = -20003;

constexpr char PROBE_REQUEST[] =
    "GET / HTTP/1.1\r\n"
    "Host: example.com\r\n"
    "User-Agent: Laino-Pocket-HTTPS-Probe/1\r\n"
    "Accept: */*\r\n"
    "Accept-Encoding: identity\r\n"
    "Connection: close\r\n"
    "\r\n";

struct DnsRequestState {
  std::atomic<uint32_t> generation{0};
  // 0: waiting for dispatch/answer, 1: success, -1: resolver failure.
  std::atomic<int> status{0};
  std::atomic<int32_t> error{0};
  ip_addr_t address{};
};

DnsRequestState dnsRequest;

void onDnsFound(const char*, const ip_addr_t* address, void* callbackArg) {
  const uint32_t generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(callbackArg));
  if (generation != dnsRequest.generation.load(std::memory_order_acquire)) {
    return;
  }
  if (address == nullptr) {
    dnsRequest.error.store(ERR_VAL, std::memory_order_relaxed);
    dnsRequest.status.store(-1, std::memory_order_release);
    return;
  }
  dnsRequest.address = *address;
  dnsRequest.status.store(1, std::memory_order_release);
}

void startDnsRequest(void* callbackArg) {
  const uint32_t generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(callbackArg));
  if (generation != dnsRequest.generation.load(std::memory_order_acquire)) {
    return;
  }

  ip_addr_t immediateAddress{};
  const err_t result =
      dns_gethostbyname_addrtype(PROBE_HOST, &immediateAddress, onDnsFound, callbackArg, LWIP_DNS_ADDRTYPE_DEFAULT);
  if (result == ERR_OK) {
    dnsRequest.address = immediateAddress;
    dnsRequest.status.store(1, std::memory_order_release);
  } else if (result != ERR_INPROGRESS) {
    dnsRequest.error.store(result, std::memory_order_relaxed);
    dnsRequest.status.store(-1, std::memory_order_release);
  }
}

enum class IoStatus : uint8_t { Ok, End, Timeout, Failure };

class DefaultBundleSecureClient final : public NetworkClientSecure {
 public:
  void useDefaultCertificateBundle() {
    // Arduino-ESP32 does not expose a no-argument default-bundle setter. Its maintained bundle is
    // present in this locked build, and the underlying callback uses it when no custom bundle is set.
    attach_ssl_certificate_bundle(sslclient.get(), true);
    _use_ca_bundle = true;
    _use_insecure = false;
  }
};

bool equalsIgnoreCase(const char* value, const std::size_t valueLength, const char* expected) {
  const std::size_t expectedLength = std::strlen(expected);
  if (valueLength != expectedLength) {
    return false;
  }
  for (std::size_t i = 0; i < valueLength; ++i) {
    if (std::tolower(static_cast<unsigned char>(value[i])) != std::tolower(static_cast<unsigned char>(expected[i]))) {
      return false;
    }
  }
  return true;
}

void trimAscii(const char*& begin, const char*& end) {
  while (begin < end && std::isspace(static_cast<unsigned char>(*begin))) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
    --end;
  }
}

bool parseDecimal(const char* begin, const char* end, int32_t& value) {
  trimAscii(begin, end);
  if (begin == end) {
    return false;
  }
  uint32_t parsed = 0;
  for (const char* cursor = begin; cursor < end; ++cursor) {
    if (*cursor < '0' || *cursor > '9') {
      return false;
    }
    const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
    if (parsed > (static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) - digit) / 10) {
      return false;
    }
    parsed = parsed * 10 + digit;
  }
  value = static_cast<int32_t>(parsed);
  return true;
}

bool parseChunkSize(const char* line, const std::size_t length, uint32_t& value) {
  const char* begin = line;
  const char* end = line + length;
  const char* extension = static_cast<const char*>(std::memchr(begin, ';', length));
  if (extension != nullptr) {
    end = extension;
  }
  trimAscii(begin, end);
  if (begin == end) {
    return false;
  }

  uint32_t parsed = 0;
  for (const char* cursor = begin; cursor < end; ++cursor) {
    uint32_t digit = 0;
    if (*cursor >= '0' && *cursor <= '9') {
      digit = static_cast<uint32_t>(*cursor - '0');
    } else if (*cursor >= 'a' && *cursor <= 'f') {
      digit = static_cast<uint32_t>(*cursor - 'a' + 10);
    } else if (*cursor >= 'A' && *cursor <= 'F') {
      digit = static_cast<uint32_t>(*cursor - 'A' + 10);
    } else {
      return false;
    }
    if (parsed > (std::numeric_limits<uint32_t>::max() - digit) / 16) {
      return false;
    }
    parsed = parsed * 16 + digit;
  }
  value = parsed;
  return true;
}

class Esp32HttpsProbeTransport final : public HttpsProbeTransport {
 public:
  Esp32HttpsProbeTransport() : startedAt(millis()) {}
  ~Esp32HttpsProbeTransport() override { close(); }

  HttpsProbeResponseHead open() override {
    IPAddress address;
    int32_t dnsError = 0;
    const IoStatus dnsStatus = resolve(address, dnsError);
    if (dnsStatus == IoStatus::Timeout) {
      return {HttpsProbeOpenStatus::Timeout, 0, -1, dnsError};
    }
    if (dnsStatus != IoStatus::Ok) {
      return {HttpsProbeOpenStatus::DnsOrConnectFailure, 0, -1, dnsError};
    }

    client.setTimeout(CONNECT_TIMEOUT_MS);
    client.useDefaultCertificateBundle();
    client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_SECONDS);
    const uint32_t connectStartedAt = millis();
    if (!client.connect(address, PROBE_PORT, PROBE_HOST, nullptr, nullptr, nullptr)) {
      char ignored[1];
      const int32_t error = client.lastError(ignored, sizeof(ignored));
      const uint32_t connectElapsed = millis() - connectStartedAt;
      if (timedOut() || (error == -1 && connectElapsed >= static_cast<uint32_t>(CONNECT_TIMEOUT_MS))) {
        return {HttpsProbeOpenStatus::Timeout, 0, -1, error};
      }
      if (error != 0 && error != -1) {
        return {HttpsProbeOpenStatus::TlsFailure, 0, -1, error};
      }
      return {HttpsProbeOpenStatus::DnsOrConnectFailure, 0, -1, error};
    }

    const uint32_t remaining = remainingMs();
    if (remaining == 0) {
      return {HttpsProbeOpenStatus::Timeout, 0, -1, 0};
    }
    client.setTimeout(std::min(RESPONSE_IO_TIMEOUT_MS, remaining));
    if (client.write(reinterpret_cast<const uint8_t*>(PROBE_REQUEST), sizeof(PROBE_REQUEST) - 1) !=
        sizeof(PROBE_REQUEST) - 1) {
      return {timedOut() ? HttpsProbeOpenStatus::Timeout : HttpsProbeOpenStatus::ReadFailure, 0, -1,
              TRANSPORT_WRITE_FAILED};
    }

    return readResponseHead();
  }

  HttpsProbeReadResult readBody(uint8_t* buffer, const std::size_t capacity) override {
    if (buffer == nullptr || capacity == 0) {
      return {HttpsProbeReadStatus::Failure, 0, TRANSPORT_MALFORMED_HTTP};
    }
    if (chunked) {
      return readChunkedBody(buffer, capacity);
    }
    if (contentRemaining == 0) {
      return {HttpsProbeReadStatus::End, 0, 0};
    }

    std::size_t requested = capacity;
    if (contentRemaining > 0) {
      requested = std::min(requested, static_cast<std::size_t>(contentRemaining));
    }
    std::size_t bytesRead = 0;
    const IoStatus status = readSome(buffer, requested, bytesRead);
    if (status == IoStatus::Timeout) {
      return {HttpsProbeReadStatus::Timeout, 0, 0};
    }
    if (status == IoStatus::Failure) {
      return {HttpsProbeReadStatus::Failure, 0, lastClientError()};
    }
    if (status == IoStatus::End) {
      return {contentRemaining < 0 ? HttpsProbeReadStatus::End : HttpsProbeReadStatus::Failure, 0, 0};
    }
    if (contentRemaining > 0) {
      contentRemaining -= static_cast<int32_t>(bytesRead);
    }
    return {HttpsProbeReadStatus::Data, bytesRead, 0};
  }

  uint32_t elapsedMs() const override { return millis() - startedAt; }

  void close() override {
    if (!closed) {
      client.stop();
      closed = true;
    }
  }

 private:
  DefaultBundleSecureClient client;
  uint32_t startedAt;
  std::size_t headerBytes = 0;
  int32_t contentRemaining = -1;
  bool chunked = false;
  bool chunkNeedsCrlf = false;
  bool chunkStreamEnded = false;
  uint32_t chunkRemaining = 0;
  bool closed = false;

  [[nodiscard]] bool timedOut() const { return elapsedMs() >= HTTPS_PROBE_TOTAL_TIMEOUT_MS; }

  [[nodiscard]] uint32_t remainingMs() const {
    const uint32_t elapsed = elapsedMs();
    return elapsed >= HTTPS_PROBE_TOTAL_TIMEOUT_MS ? 0 : HTTPS_PROBE_TOTAL_TIMEOUT_MS - elapsed;
  }

  int32_t lastClientError() {
    char ignored[1];
    return client.lastError(ignored, sizeof(ignored));
  }

  IoStatus resolve(IPAddress& address, int32_t& error) {
    uint32_t generation = dnsRequest.generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (generation == 0) {
      generation = dnsRequest.generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    dnsRequest.error.store(0, std::memory_order_relaxed);
    dnsRequest.status.store(0, std::memory_order_release);

    const err_t dispatched =
        tcpip_callback(startDnsRequest, reinterpret_cast<void*>(static_cast<uintptr_t>(generation)));
    if (dispatched != ERR_OK) {
      error = TRANSPORT_DNS_DISPATCH_FAILED;
      return IoStatus::Failure;
    }

    const uint32_t dnsStartedAt = millis();
    while ((millis() - dnsStartedAt) < DNS_TIMEOUT_MS && !timedOut()) {
      const int status = dnsRequest.status.load(std::memory_order_acquire);
      if (status == 1) {
        address = IPAddress(&dnsRequest.address);
        return IoStatus::Ok;
      }
      if (status == -1) {
        error = dnsRequest.error.load(std::memory_order_relaxed);
        return IoStatus::Failure;
      }
      delay(1);
    }
    error = ERR_TIMEOUT;
    return IoStatus::Timeout;
  }

  IoStatus readByte(uint8_t& byte) {
    while (!timedOut()) {
      const int available = client.available();
      if (available > 0) {
        const int read = client.read(&byte, 1);
        return read == 1 ? IoStatus::Ok : IoStatus::Failure;
      }
      if (!client.connected()) {
        return IoStatus::End;
      }
      delay(1);
    }
    return IoStatus::Timeout;
  }

  IoStatus readSome(uint8_t* buffer, const std::size_t capacity, std::size_t& bytesRead) {
    bytesRead = 0;
    while (!timedOut()) {
      const int available = client.available();
      if (available > 0) {
        const std::size_t requested = std::min(capacity, static_cast<std::size_t>(available));
        const int read = client.read(buffer, requested);
        if (read <= 0) {
          return IoStatus::Failure;
        }
        bytesRead = static_cast<std::size_t>(read);
        return IoStatus::Ok;
      }
      if (!client.connected()) {
        return IoStatus::End;
      }
      delay(1);
    }
    return IoStatus::Timeout;
  }

  IoStatus readLine(char* output, const std::size_t outputSize, std::size_t& length) {
    length = 0;
    if (output == nullptr || outputSize < 2) {
      return IoStatus::Failure;
    }
    while (true) {
      uint8_t byte = 0;
      const IoStatus status = readByte(byte);
      if (status != IoStatus::Ok) {
        return status;
      }
      ++headerBytes;
      if (headerBytes > MAX_HTTP_METADATA_BYTES) {
        return IoStatus::Failure;
      }
      if (byte == '\n') {
        if (length > 0 && output[length - 1] == '\r') {
          --length;
        }
        output[length] = '\0';
        return IoStatus::Ok;
      }
      if (length + 1 >= outputSize) {
        return IoStatus::Failure;
      }
      output[length++] = static_cast<char>(byte);
    }
  }

  HttpsProbeResponseHead malformedHead(const int16_t httpStatus = 0) {
    return {HttpsProbeOpenStatus::ReadFailure, httpStatus, -1, TRANSPORT_MALFORMED_HTTP};
  }

  HttpsProbeResponseHead readResponseHead() {
    char line[MAX_LINE_BYTES];
    std::size_t length = 0;
    IoStatus status = readLine(line, sizeof(line), length);
    if (status == IoStatus::Timeout) {
      return {HttpsProbeOpenStatus::Timeout, 0, -1, 0};
    }
    if (status != IoStatus::Ok || length < 12 || std::memcmp(line, "HTTP/1.", 7) != 0 ||
        (line[7] != '0' && line[7] != '1') || line[8] != ' ' || !std::isdigit(static_cast<unsigned char>(line[9])) ||
        !std::isdigit(static_cast<unsigned char>(line[10])) || !std::isdigit(static_cast<unsigned char>(line[11])) ||
        (length > 12 && line[12] != ' ')) {
      return malformedHead();
    }
    const int16_t httpStatus = static_cast<int16_t>((line[9] - '0') * 100 + (line[10] - '0') * 10 + line[11] - '0');

    int32_t declaredLength = -1;
    bool sawContentLength = false;
    bool sawChunked = false;
    while (true) {
      status = readLine(line, sizeof(line), length);
      if (status == IoStatus::Timeout) {
        return {HttpsProbeOpenStatus::Timeout, httpStatus, -1, 0};
      }
      if (status != IoStatus::Ok) {
        return malformedHead(httpStatus);
      }
      if (length == 0) {
        break;
      }

      char* colon = static_cast<char*>(std::memchr(line, ':', length));
      if (colon == nullptr) {
        return malformedHead(httpStatus);
      }
      const char* valueBegin = colon + 1;
      const char* valueEnd = line + length;
      trimAscii(valueBegin, valueEnd);
      const std::size_t nameLength = static_cast<std::size_t>(colon - line);

      if (equalsIgnoreCase(line, nameLength, "Content-Length")) {
        int32_t parsed = -1;
        if (sawContentLength || !parseDecimal(valueBegin, valueEnd, parsed)) {
          return malformedHead(httpStatus);
        }
        declaredLength = parsed;
        sawContentLength = true;
      } else if (equalsIgnoreCase(line, nameLength, "Transfer-Encoding")) {
        if (sawChunked || !equalsIgnoreCase(valueBegin, static_cast<std::size_t>(valueEnd - valueBegin), "chunked")) {
          return malformedHead(httpStatus);
        }
        sawChunked = true;
      }
    }

    if (sawContentLength && sawChunked) {
      return malformedHead(httpStatus);
    }
    chunked = sawChunked;
    contentRemaining = sawContentLength ? declaredLength : -1;
    return {HttpsProbeOpenStatus::Ready, httpStatus, contentRemaining, 0};
  }

  IoStatus readExpectedCrlf() {
    uint8_t first = 0;
    uint8_t second = 0;
    const IoStatus firstStatus = readByte(first);
    if (firstStatus != IoStatus::Ok) {
      return firstStatus;
    }
    const IoStatus secondStatus = readByte(second);
    if (secondStatus != IoStatus::Ok) {
      return secondStatus;
    }
    return first == '\r' && second == '\n' ? IoStatus::Ok : IoStatus::Failure;
  }

  HttpsProbeReadResult readChunkedBody(uint8_t* buffer, const std::size_t capacity) {
    if (chunkStreamEnded) {
      return {HttpsProbeReadStatus::End, 0, 0};
    }

    while (chunkRemaining == 0) {
      if (chunkNeedsCrlf) {
        const IoStatus crlf = readExpectedCrlf();
        if (crlf == IoStatus::Timeout) {
          return {HttpsProbeReadStatus::Timeout, 0, 0};
        }
        if (crlf != IoStatus::Ok) {
          return {HttpsProbeReadStatus::Failure, 0, TRANSPORT_MALFORMED_HTTP};
        }
        chunkNeedsCrlf = false;
      }

      char line[MAX_LINE_BYTES];
      std::size_t length = 0;
      const IoStatus lineStatus = readLine(line, sizeof(line), length);
      if (lineStatus == IoStatus::Timeout) {
        return {HttpsProbeReadStatus::Timeout, 0, 0};
      }
      if (lineStatus != IoStatus::Ok || !parseChunkSize(line, length, chunkRemaining)) {
        return {HttpsProbeReadStatus::Failure, 0, TRANSPORT_MALFORMED_HTTP};
      }
      if (chunkRemaining == 0) {
        // Consume bounded trailers through the terminating empty line.
        do {
          const IoStatus trailerStatus = readLine(line, sizeof(line), length);
          if (trailerStatus == IoStatus::Timeout) {
            return {HttpsProbeReadStatus::Timeout, 0, 0};
          }
          if (trailerStatus != IoStatus::Ok) {
            return {HttpsProbeReadStatus::Failure, 0, TRANSPORT_MALFORMED_HTTP};
          }
        } while (length != 0);
        chunkStreamEnded = true;
        return {HttpsProbeReadStatus::End, 0, 0};
      }
    }

    const std::size_t requested = std::min(capacity, static_cast<std::size_t>(chunkRemaining));
    std::size_t bytesRead = 0;
    const IoStatus status = readSome(buffer, requested, bytesRead);
    if (status == IoStatus::Timeout) {
      return {HttpsProbeReadStatus::Timeout, 0, 0};
    }
    if (status != IoStatus::Ok) {
      return {HttpsProbeReadStatus::Failure, 0, status == IoStatus::Failure ? lastClientError() : 0};
    }
    chunkRemaining -= static_cast<uint32_t>(bytesRead);
    if (chunkRemaining == 0) {
      chunkNeedsCrlf = true;
    }
    return {HttpsProbeReadStatus::Data, bytesRead, 0};
  }
};

}  // namespace

HttpsProbeResult runPublicHttpsProbe() {
  Esp32HttpsProbeTransport transport;
  return runHttpsProbe(WiFi.status() == WL_CONNECTED, transport);
}

}  // namespace pocket
