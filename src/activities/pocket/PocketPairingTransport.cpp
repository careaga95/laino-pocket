#include "PocketPairingTransport.h"

#include <Arduino.h>
#include <IPAddress.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>

#include "PocketCredential.h"
#include "PocketPairingTransportPolicy.h"

namespace pocket {
namespace {

constexpr char GATEWAY_HOST[] = "hari.laino.app";
constexpr uint16_t GATEWAY_PORT = 443;
constexpr uint32_t DNS_TIMEOUT_MS = 2500;
constexpr int32_t CONNECT_TIMEOUT_MS = 4000;
constexpr uint32_t TLS_HANDSHAKE_TIMEOUT_SECONDS = 5;
constexpr uint32_t RESPONSE_IO_TIMEOUT_MS = 4000;
constexpr std::size_t MAX_METADATA_BYTES = 8192;
constexpr uint32_t MIN_TLS_HEAP_BYTES = 55000;
constexpr int32_t ERROR_MALFORMED = -21001;
constexpr int32_t ERROR_DNS_DISPATCH = -21002;

struct DnsState {
  std::atomic<uint32_t> generation{0};
  std::atomic<int> status{0};
  std::atomic<int32_t> error{0};
  ip_addr_t address{};
};

DnsState dnsState;

void onDnsFound(const char*, const ip_addr_t* address, void* callbackArg) {
  const uint32_t generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(callbackArg));
  if (generation != dnsState.generation.load(std::memory_order_acquire)) return;
  if (address == nullptr) {
    dnsState.error.store(ERR_VAL, std::memory_order_relaxed);
    dnsState.status.store(-1, std::memory_order_release);
    return;
  }
  dnsState.address = *address;
  dnsState.status.store(1, std::memory_order_release);
}

void startDns(void* callbackArg) {
  const uint32_t generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(callbackArg));
  if (generation != dnsState.generation.load(std::memory_order_acquire)) return;
  ip_addr_t address{};
  const err_t result =
      dns_gethostbyname_addrtype(GATEWAY_HOST, &address, onDnsFound, callbackArg, LWIP_DNS_ADDRTYPE_DEFAULT);
  if (result == ERR_OK) {
    dnsState.address = address;
    dnsState.status.store(1, std::memory_order_release);
  } else if (result != ERR_INPROGRESS) {
    dnsState.error.store(result, std::memory_order_relaxed);
    dnsState.status.store(-1, std::memory_order_release);
  }
}

class DefaultBundleSecureClient final : public NetworkClientSecure {
 public:
  void useDefaultCertificateBundle() {
    attach_ssl_certificate_bundle(sslclient.get(), true);
    _use_ca_bundle = true;
    _use_insecure = false;
  }
};

enum class IoStatus : uint8_t { Ok, End, Cancelled, Timeout, Failure, Oversized };

bool equalsIgnoreCase(const char* value, const std::size_t length, const char* expected) {
  if (length != std::strlen(expected)) return false;
  for (std::size_t i = 0; i < length; ++i) {
    if (std::tolower(static_cast<unsigned char>(value[i])) != std::tolower(static_cast<unsigned char>(expected[i])))
      return false;
  }
  return true;
}

void trim(const char*& begin, const char*& end) {
  while (begin < end && (*begin == ' ' || *begin == '\t')) ++begin;
  while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) --end;
}

bool parseDecimal(const char* begin, const char* end, uint32_t& value) {
  trim(begin, end);
  if (begin == end) return false;
  uint32_t parsed = 0;
  for (const char* cursor = begin; cursor < end; ++cursor) {
    if (*cursor < '0' || *cursor > '9') return false;
    const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
    if (parsed > (std::numeric_limits<uint32_t>::max() - digit) / 10U) return false;
    parsed = parsed * 10U + digit;
  }
  value = parsed;
  return true;
}

bool parseChunkSize(const char* line, const std::size_t length, uint32_t& value) {
  const char* begin = line;
  const char* end = line + length;
  const char* extension = static_cast<const char*>(std::memchr(begin, ';', length));
  if (extension != nullptr) end = extension;
  trim(begin, end);
  if (begin == end) return false;
  uint32_t parsed = 0;
  for (const char* cursor = begin; cursor < end; ++cursor) {
    uint32_t digit = 0;
    if (*cursor >= '0' && *cursor <= '9')
      digit = static_cast<uint32_t>(*cursor - '0');
    else if (*cursor >= 'a' && *cursor <= 'f')
      digit = static_cast<uint32_t>(*cursor - 'a' + 10);
    else if (*cursor >= 'A' && *cursor <= 'F')
      digit = static_cast<uint32_t>(*cursor - 'A' + 10);
    else
      return false;
    if (parsed > (std::numeric_limits<uint32_t>::max() - digit) / 16U) return false;
    parsed = parsed * 16U + digit;
  }
  value = parsed;
  return true;
}

bool isJsonContentType(const char* begin, const char* end) {
  trim(begin, end);
  const char* semicolon = static_cast<const char*>(std::memchr(begin, ';', static_cast<std::size_t>(end - begin)));
  const char* mediaEnd = semicolon == nullptr ? end : semicolon;
  while (mediaEnd > begin && (mediaEnd[-1] == ' ' || mediaEnd[-1] == '\t')) --mediaEnd;
  return equalsIgnoreCase(begin, static_cast<std::size_t>(mediaEnd - begin), "application/json");
}

bool isEpubContentType(const char* begin, const char* end) {
  trim(begin, end);
  const char* semicolon = static_cast<const char*>(std::memchr(begin, ';', static_cast<std::size_t>(end - begin)));
  const char* mediaEnd = semicolon == nullptr ? end : semicolon;
  while (mediaEnd > begin && (mediaEnd[-1] == ' ' || mediaEnd[-1] == '\t')) --mediaEnd;
  return equalsIgnoreCase(begin, static_cast<std::size_t>(mediaEnd - begin), "application/epub+zip");
}

const char* methodName(const GatewayMethod method) {
  switch (method) {
    case GatewayMethod::Get:
      return "GET";
    case GatewayMethod::Post:
      return "POST";
    case GatewayMethod::Delete:
      return "DELETE";
  }
  return "GET";
}

class RequestSession {
 public:
  RequestSession(const std::atomic<bool>& cancelled, GatewayResponse& response)
      : cancelled(cancelled), response(response), startedAt(millis()) {}
  ~RequestSession() {
    client.stop();
    if (sinkStarted && !sinkCommitted && sink != nullptr) sink->abort();
  }

  void run(const GatewayRequest& request) {
    requestTimeoutMs = request.timeoutMs;
    if (checkTransportControl(TransportCheckpoint::BeforeRequest, cancelled.load(std::memory_order_acquire), 0) ==
        TransportControlDecision::Cancelled) {
      return finish(GatewayTransportResult::Cancelled);
    }
    if (WiFi.status() != WL_CONNECTED) return finish(GatewayTransportResult::NoWifi);
    if (ESP.getFreeHeap() < MIN_TLS_HEAP_BYTES) return finish(GatewayTransportResult::LowMemory);
    const bool hasSuccessBody = request.successBody != nullptr;
    const bool hasSuccessSink = request.successSink != nullptr;
    if (request.path == nullptr || request.path[0] != '/' || std::strchr(request.path, '?') != nullptr ||
        request.jsonBodyLength > 256 || (request.jsonBodyLength > 0 && request.jsonBody == nullptr) ||
        hasSuccessBody != (request.successBodyCapacity > 0) ||
        (hasSuccessBody &&
         (request.successBodyCapacity < 2 || request.successBodyCapacity > POCKET_MAX_LARGE_RESPONSE_BODY_BYTES + 1)) ||
        (hasSuccessBody && hasSuccessSink) || hasSuccessSink != (request.successSinkMaximumBytes > 0) ||
        (hasSuccessSink && (request.successMediaType != GatewaySuccessMediaType::Epub ||
                            request.successSinkMaximumBytes > POCKET_MAX_BINARY_RESPONSE_BODY_BYTES)) ||
        (!hasSuccessSink && request.successMediaType != GatewaySuccessMediaType::Json) || requestTimeoutMs < 1000 ||
        requestTimeoutMs > POCKET_MAX_REQUEST_TIMEOUT_MS) {
      return finish(GatewayTransportResult::WriteFailure, ERROR_MALFORMED);
    }
    if (hasSuccessBody) request.successBody[0] = '\0';
    sink = request.successSink;
    sinkMaximumBytes = request.successSinkMaximumBytes;

    IPAddress address;
    const IoStatus dns = resolve(address);
    if (dns != IoStatus::Ok) return finishForIo(dns, GatewayTransportResult::DnsOrConnectFailure);

    client.setTimeout(CONNECT_TIMEOUT_MS);
    client.useDefaultCertificateBundle();
    client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_SECONDS);
    const uint32_t connectStartedAt = millis();
    if (!client.connect(address, GATEWAY_PORT, GATEWAY_HOST, nullptr, nullptr, nullptr)) {
      const int32_t error = lastError();
      if (timedOut() || (error == -1 && static_cast<uint32_t>(millis() - connectStartedAt) >= CONNECT_TIMEOUT_MS)) {
        return finish(GatewayTransportResult::Timeout, error);
      }
      return finish(
          error != 0 && error != -1 ? GatewayTransportResult::TlsFailure : GatewayTransportResult::DnsOrConnectFailure,
          error);
    }
    if (cancelled.load(std::memory_order_acquire)) return finish(GatewayTransportResult::Cancelled);
    if (!writeRequest(request)) return;
    readResponse(request);
  }

 private:
  DefaultBundleSecureClient client;
  const std::atomic<bool>& cancelled;
  GatewayResponse& response;
  const uint32_t startedAt;
  std::size_t metadataBytes = 0;
  int32_t contentRemaining = -1;
  bool chunked = false;
  bool chunkNeedsCrlf = false;
  bool chunkEnded = false;
  uint32_t chunkRemaining = 0;
  char* body = response.body;
  std::size_t bodyCapacity = sizeof(response.body);
  GatewayBodySink* sink = nullptr;
  uint32_t streamedBodyLength = 0;
  uint32_t sinkMaximumBytes = 0;
  uint32_t requestTimeoutMs = POCKET_TOTAL_REQUEST_TIMEOUT_MS;
  bool sinkStarted = false;
  bool sinkCommitted = false;

  bool timedOut() const {
    return checkTransportControl(TransportCheckpoint::ResponseRead, false, static_cast<uint32_t>(millis() - startedAt),
                                 requestTimeoutMs) == TransportControlDecision::TimedOut;
  }
  uint32_t remainingMs() const {
    const uint32_t elapsed = static_cast<uint32_t>(millis() - startedAt);
    return elapsed >= requestTimeoutMs ? 0 : requestTimeoutMs - elapsed;
  }
  int32_t lastError() {
    char ignored[1];
    return client.lastError(ignored, sizeof(ignored));
  }
  void finish(const GatewayTransportResult result, const int32_t error = 0) {
    response.transport = result;
    response.transportError = error;
    response.elapsedMs = static_cast<uint32_t>(millis() - startedAt);
  }
  void finishForIo(const IoStatus status, const GatewayTransportResult failure) {
    if (status == IoStatus::Cancelled)
      finish(GatewayTransportResult::Cancelled);
    else if (status == IoStatus::Timeout)
      finish(GatewayTransportResult::Timeout);
    else
      finish(failure, response.transportError != 0 ? response.transportError : lastError());
  }

  IoStatus resolve(IPAddress& address) {
    uint32_t generation = dnsState.generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (generation == 0) generation = dnsState.generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    dnsState.status.store(0, std::memory_order_release);
    dnsState.error.store(0, std::memory_order_relaxed);
    if (tcpip_callback(startDns, reinterpret_cast<void*>(static_cast<uintptr_t>(generation))) != ERR_OK) {
      response.transportError = ERROR_DNS_DISPATCH;
      return IoStatus::Failure;
    }
    const uint32_t dnsStartedAt = millis();
    while (static_cast<uint32_t>(millis() - dnsStartedAt) < DNS_TIMEOUT_MS) {
      const TransportControlDecision control =
          checkTransportControl(TransportCheckpoint::DnsWait, cancelled.load(std::memory_order_acquire),
                                static_cast<uint32_t>(millis() - startedAt), requestTimeoutMs);
      if (control == TransportControlDecision::Cancelled) return IoStatus::Cancelled;
      if (control == TransportControlDecision::TimedOut) return IoStatus::Timeout;
      const int status = dnsState.status.load(std::memory_order_acquire);
      if (status == 1) {
        address = IPAddress(&dnsState.address);
        return IoStatus::Ok;
      }
      if (status == -1) {
        response.transportError = dnsState.error.load(std::memory_order_relaxed);
        return IoStatus::Failure;
      }
      delay(1);
    }
    return IoStatus::Timeout;
  }

  bool writeAll(const char* data, const std::size_t length) {
    if (cancelled.load(std::memory_order_acquire)) {
      finish(GatewayTransportResult::Cancelled);
      return false;
    }
    if (remainingMs() == 0) {
      finish(GatewayTransportResult::Timeout);
      return false;
    }
    client.setTimeout(std::min(RESPONSE_IO_TIMEOUT_MS, remainingMs()));
    if (client.write(reinterpret_cast<const uint8_t*>(data), length) != length) {
      finish(timedOut() ? GatewayTransportResult::Timeout : GatewayTransportResult::WriteFailure, lastError());
      return false;
    }
    return true;
  }

  bool writeRequest(const GatewayRequest& request) {
    char headers[512];
    const char* accept =
        request.successMediaType == GatewaySuccessMediaType::Epub ? "application/epub+zip" : "application/json";
    int length = std::snprintf(headers, sizeof(headers),
                               "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Laino-Pocket/1\r\n"
                               "Accept: %s\r\nAccept-Encoding: identity\r\nConnection: close\r\n",
                               methodName(request.method), request.path, GATEWAY_HOST, accept);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(headers)) {
      finish(GatewayTransportResult::WriteFailure, ERROR_MALFORMED);
      return false;
    }
    std::size_t used = static_cast<std::size_t>(length);
    if (request.bearer != nullptr) {
      length = std::snprintf(headers + used, sizeof(headers) - used, "Authorization: Bearer %s\r\n", request.bearer);
      if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(headers) - used) {
        finish(GatewayTransportResult::WriteFailure, ERROR_MALFORMED);
        return false;
      }
      used += static_cast<std::size_t>(length);
    }
    if (request.jsonBody != nullptr) {
      length = std::snprintf(headers + used, sizeof(headers) - used,
                             "Content-Type: application/json\r\nContent-Length: %u\r\n",
                             static_cast<unsigned>(request.jsonBodyLength));
      if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(headers) - used) {
        finish(GatewayTransportResult::WriteFailure, ERROR_MALFORMED);
        return false;
      }
      used += static_cast<std::size_t>(length);
    }
    if (used + 2 > sizeof(headers)) {
      finish(GatewayTransportResult::WriteFailure, ERROR_MALFORMED);
      return false;
    }
    headers[used++] = '\r';
    headers[used++] = '\n';
    const bool wroteHeaders = writeAll(headers, used);
    secureClear(headers, sizeof(headers));
    if (!wroteHeaders) return false;
    return request.jsonBody == nullptr || writeAll(request.jsonBody, request.jsonBodyLength);
  }

  IoStatus readByte(uint8_t& byte) {
    while (true) {
      const TransportControlDecision control =
          checkTransportControl(TransportCheckpoint::ResponseRead, cancelled.load(std::memory_order_acquire),
                                static_cast<uint32_t>(millis() - startedAt), requestTimeoutMs);
      if (control == TransportControlDecision::Cancelled) return IoStatus::Cancelled;
      if (control == TransportControlDecision::TimedOut) return IoStatus::Timeout;
      const int available = client.available();
      if (available > 0) return client.read(&byte, 1) == 1 ? IoStatus::Ok : IoStatus::Failure;
      if (!client.connected()) return IoStatus::End;
      delay(1);
    }
  }

  IoStatus readLine(char* output, const std::size_t outputSize, std::size_t& length) {
    length = 0;
    while (true) {
      uint8_t byte = 0;
      const IoStatus status = readByte(byte);
      if (status != IoStatus::Ok) return status;
      if (++metadataBytes > MAX_METADATA_BYTES) return IoStatus::Oversized;
      if (byte == '\n') {
        if (length > 0 && output[length - 1] == '\r') --length;
        output[length] = '\0';
        return IoStatus::Ok;
      }
      if (length + 1 >= outputSize) return IoStatus::Oversized;
      output[length++] = static_cast<char>(byte);
    }
  }

  IoStatus readSome(uint8_t* output, const std::size_t capacity, std::size_t& count) {
    count = 0;
    while (true) {
      const TransportControlDecision control =
          checkTransportControl(TransportCheckpoint::ResponseRead, cancelled.load(std::memory_order_acquire),
                                static_cast<uint32_t>(millis() - startedAt), requestTimeoutMs);
      if (control == TransportControlDecision::Cancelled) return IoStatus::Cancelled;
      if (control == TransportControlDecision::TimedOut) return IoStatus::Timeout;
      const int available = client.available();
      if (available > 0) {
        const std::size_t requested = std::min(capacity, static_cast<std::size_t>(available));
        const int read = client.read(output, requested);
        if (read <= 0) return IoStatus::Failure;
        count = static_cast<std::size_t>(read);
        return IoStatus::Ok;
      }
      if (!client.connected()) return IoStatus::End;
      delay(1);
    }
  }

  void failForIo(const IoStatus status) {
    if (status == IoStatus::Cancelled)
      finish(GatewayTransportResult::Cancelled);
    else if (status == IoStatus::Timeout)
      finish(GatewayTransportResult::Timeout);
    else if (status == IoStatus::Oversized)
      finish(GatewayTransportResult::MalformedHttp, ERROR_MALFORMED);
    else
      finish(GatewayTransportResult::ReadFailure, lastError());
  }

  void readResponse(const GatewayRequest& request) {
    char line[POCKET_MAX_RESPONSE_LINE_BYTES];
    std::size_t length = 0;
    IoStatus io = readLine(line, sizeof(line), length);
    if (io != IoStatus::Ok) return failForIo(io);
    if (length < 12 || std::memcmp(line, "HTTP/1.", 7) != 0 || (line[7] != '0' && line[7] != '1') || line[8] != ' ' ||
        !std::isdigit(static_cast<unsigned char>(line[9])) || !std::isdigit(static_cast<unsigned char>(line[10])) ||
        !std::isdigit(static_cast<unsigned char>(line[11])) || (length > 12 && line[12] != ' ')) {
      return finish(GatewayTransportResult::MalformedHttp, ERROR_MALFORMED);
    }
    response.httpStatus = static_cast<int16_t>((line[9] - '0') * 100 + (line[10] - '0') * 10 + line[11] - '0');
    if (validateResponseEnvelope(ResponseEnvelope{response.httpStatus}) == GatewayTransportResult::RedirectRejected) {
      return finish(GatewayTransportResult::RedirectRejected);
    }
    if (response.httpStatus >= 200 && response.httpStatus <= 299 && request.successBody != nullptr) {
      body = request.successBody;
      bodyCapacity = request.successBodyCapacity;
    }

    bool sawLength = false;
    bool sawTransfer = false;
    bool sawContentType = false;
    bool jsonContentType = false;
    bool epubContentType = false;
    bool sawRetryAfter = false;
    while (true) {
      io = readLine(line, sizeof(line), length);
      if (io != IoStatus::Ok) return failForIo(io);
      if (length == 0) break;
      char* colon = static_cast<char*>(std::memchr(line, ':', length));
      if (colon == nullptr) return finish(GatewayTransportResult::MalformedHttp, ERROR_MALFORMED);
      const std::size_t nameLength = static_cast<std::size_t>(colon - line);
      const char* valueBegin = colon + 1;
      const char* valueEnd = line + length;
      trim(valueBegin, valueEnd);
      if (equalsIgnoreCase(line, nameLength, "Content-Length")) {
        uint32_t value = 0;
        if (sawLength || !parseDecimal(valueBegin, valueEnd, value) || value > INT32_MAX) {
          return finish(GatewayTransportResult::MalformedHttp, ERROR_MALFORMED);
        }
        sawLength = true;
        contentRemaining = static_cast<int32_t>(value);
      } else if (equalsIgnoreCase(line, nameLength, "Transfer-Encoding")) {
        if (sawTransfer || !equalsIgnoreCase(valueBegin, static_cast<std::size_t>(valueEnd - valueBegin), "chunked")) {
          return finish(GatewayTransportResult::MalformedHttp, ERROR_MALFORMED);
        }
        sawTransfer = true;
        chunked = true;
      } else if (equalsIgnoreCase(line, nameLength, "Content-Type")) {
        if (sawContentType) return finish(GatewayTransportResult::MalformedHttp, ERROR_MALFORMED);
        sawContentType = true;
        jsonContentType = isJsonContentType(valueBegin, valueEnd);
        epubContentType = isEpubContentType(valueBegin, valueEnd);
      } else if (equalsIgnoreCase(line, nameLength, "Retry-After")) {
        uint32_t value = 0;
        if (sawRetryAfter || !parseDecimal(valueBegin, valueEnd, value)) {
          return finish(GatewayTransportResult::MalformedHttp, ERROR_MALFORMED);
        }
        sawRetryAfter = true;
        response.retryAfter = static_cast<uint8_t>(value > 255 ? 255 : value);
      }
    }
    const ResponseEnvelope responseEnvelope{response.httpStatus, sawLength,        sawTransfer,    sawContentType,
                                            jsonContentType,     contentRemaining, epubContentType};
    const bool binarySuccess =
        response.httpStatus >= 200 && response.httpStatus <= 299 && request.successSink != nullptr;
    const GatewayTransportResult envelope =
        binarySuccess ? validateBinaryResponseEnvelope(responseEnvelope, request.successSinkMaximumBytes)
                      : validateResponseEnvelope(responseEnvelope, static_cast<uint32_t>(bodyCapacity - 1));
    if (envelope != GatewayTransportResult::Success) {
      return finish(envelope, envelope == GatewayTransportResult::MalformedHttp ? ERROR_MALFORMED : 0);
    }
    if (binarySuccess) {
      sinkStarted = true;
      if (!sink->begin(static_cast<uint32_t>(contentRemaining))) {
        return finish(GatewayTransportResult::StorageFailure);
      }
    }
    if (response.httpStatus == 204) {
      if (chunked || contentRemaining > 0) return finish(GatewayTransportResult::MalformedHttp, ERROR_MALFORMED);
      body[0] = '\0';
      return finish(GatewayTransportResult::Success);
    }
    readBody();
  }

  IoStatus consumeChunkCrlf() {
    uint8_t first = 0;
    uint8_t second = 0;
    IoStatus status = readByte(first);
    if (status != IoStatus::Ok) return status;
    status = readByte(second);
    return status == IoStatus::Ok && first == '\r' && second == '\n' ? IoStatus::Ok : IoStatus::Failure;
  }

  IoStatus prepareChunk() {
    if (chunkEnded) return IoStatus::End;
    if (chunkNeedsCrlf) {
      const IoStatus status = consumeChunkCrlf();
      if (status != IoStatus::Ok) return status;
      chunkNeedsCrlf = false;
    }
    char line[POCKET_MAX_RESPONSE_LINE_BYTES];
    std::size_t length = 0;
    IoStatus status = readLine(line, sizeof(line), length);
    if (status != IoStatus::Ok) return status;
    if (!parseChunkSize(line, length, chunkRemaining)) return IoStatus::Failure;
    if (chunkRemaining != 0) return IoStatus::Ok;
    while (true) {
      status = readLine(line, sizeof(line), length);
      if (status != IoStatus::Ok) return status;
      if (length == 0) break;
      if (std::memchr(line, ':', length) == nullptr) return IoStatus::Failure;
    }
    chunkEnded = true;
    return IoStatus::End;
  }

  IoStatus readBodyPart(uint8_t* output, const std::size_t capacity, std::size_t& count) {
    if (!chunked) {
      if (contentRemaining == 0) return IoStatus::End;
      const std::size_t requested =
          contentRemaining > 0 ? std::min(capacity, static_cast<std::size_t>(contentRemaining)) : capacity;
      const IoStatus status = readSome(output, requested, count);
      if (status == IoStatus::End && contentRemaining > 0) return IoStatus::Failure;
      if (status == IoStatus::Ok && contentRemaining > 0) contentRemaining -= static_cast<int32_t>(count);
      return status;
    }
    if (chunkRemaining == 0) {
      const IoStatus status = prepareChunk();
      if (status != IoStatus::Ok) return status;
    }
    const IoStatus status = readSome(output, std::min(capacity, static_cast<std::size_t>(chunkRemaining)), count);
    if (status != IoStatus::Ok) return status == IoStatus::End ? IoStatus::Failure : status;
    chunkRemaining -= static_cast<uint32_t>(count);
    if (chunkRemaining == 0) chunkNeedsCrlf = true;
    return IoStatus::Ok;
  }

  void readBody() {
    while (true) {
      // A 1 KiB block keeps SD mutex/write overhead bounded for multi-megabyte
      // EPUBs while staying within the dedicated 8 KiB worker stack.
      uint8_t buffer[1024];
      std::size_t count = 0;
      const IoStatus status = readBodyPart(buffer, sizeof(buffer), count);
      if (status == IoStatus::End) {
        if (sinkStarted) {
          if (!sink->finish()) return finish(GatewayTransportResult::StorageFailure);
          sinkCommitted = true;
          return finish(GatewayTransportResult::Success);
        }
        body[response.bodyLength] = '\0';
        return finish(GatewayTransportResult::Success);
      }
      if (status != IoStatus::Ok || count == 0) return failForIo(status == IoStatus::Ok ? IoStatus::Failure : status);
      if (sinkStarted) {
        const bool oversized = static_cast<uint64_t>(streamedBodyLength) + count > sinkMaximumBytes;
        if (oversized || !sink->write(buffer, count)) {
          secureClear(buffer, sizeof(buffer));
          return finish(oversized ? GatewayTransportResult::OversizedResponse : GatewayTransportResult::StorageFailure);
        }
        streamedBodyLength += static_cast<uint32_t>(count);
        secureClear(buffer, sizeof(buffer));
        continue;
      }
      if (response.bodyLength + count >= bodyCapacity) {
        secureClear(buffer, sizeof(buffer));
        return finish(GatewayTransportResult::OversizedResponse);
      }
      std::memcpy(body + response.bodyLength, buffer, count);
      response.bodyLength = static_cast<uint16_t>(response.bodyLength + count);
      secureClear(buffer, sizeof(buffer));
    }
  }
};

}  // namespace

void Esp32PocketGatewayTransport::request(const GatewayRequest& request, const std::atomic<bool>& cancelled,
                                          GatewayResponse& response) {
  response = {};
  RequestSession(cancelled, response).run(request);
}

}  // namespace pocket
