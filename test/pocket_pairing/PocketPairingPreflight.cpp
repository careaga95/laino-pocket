#include <atomic>
#include <cstring>
#include <iostream>
#include <iterator>
#include <string>

#include "PocketPairingClient.h"
#include "PocketPairingProtocol.h"

namespace {

constexpr char DEVICE_CODE[] = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

class PayloadTransport final : public pocket::PocketGatewayTransport {
 public:
  std::string body;
  int calls = 0;

  void request(const pocket::GatewayRequest& request, const std::atomic<bool>&,
               pocket::GatewayResponse& response) override {
    ++calls;
    body.assign(request.jsonBody == nullptr ? "" : request.jsonBody, request.jsonBodyLength);
    const std::string normative = std::string("{\"protocol\":1,\"device_code\":\"") + DEVICE_CODE +
                                  "\",\"user_code\":\"ABCDEFGH\",\"expires_in\":600,"
                                  "\"interval\":10,\"first_poll_after\":5}";
    response.transport = pocket::GatewayTransportResult::Success;
    response.httpStatus = 200;
    response.bodyLength = static_cast<uint16_t>(normative.size());
    std::memcpy(response.body, normative.data(), normative.size());
    response.body[normative.size()] = '\0';
  }
};

int emitPayload() {
  PayloadTransport transport;
  pocket::PairingClient client(transport);
  pocket::PairingStartResponse response{};
  std::atomic<bool> cancelled{false};
  const pocket::PocketClientOutcome outcome =
      client.start(pocket::COMPILED_PAIRING_IDENTITY, cancelled, response);
  if (outcome.result != pocket::PocketClientResult::Success || transport.calls != 1 || transport.body.empty()) {
    return 1;
  }
  std::cout << transport.body;
  return 0;
}

int parseHandlerResponse() {
  const std::string json((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  pocket::PairingStartResponse response{};
  if (pocket::parsePairingStartResponse(json.data(), json.size(), response) != pocket::JsonParseResult::Success) {
    return 1;
  }
  if (!pocket::isCanonicalUserCode(response.userCode, std::strlen(response.userCode))) return 1;
  const std::string display = std::string(response.userCode, 4) + "-" + std::string(response.userCode + 4, 4);
  if (display.size() != 9 || display[4] != '-') return 1;
  std::cout << "user_code_presentable=true\n";
  return 0;
}

}  // namespace

int main(const int argc, char** argv) {
  if (argc != 2) return 2;
  if (std::strcmp(argv[1], "payload") == 0) return emitPayload();
  if (std::strcmp(argv[1], "parse") == 0) return parseHandlerResponse();
  return 2;
}
