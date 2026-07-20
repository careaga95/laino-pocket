#pragma once

#include "PocketPairingClient.h"

namespace pocket {

class Esp32PocketGatewayTransport final : public PocketGatewayTransport {
 public:
  void request(const GatewayRequest& request, const std::atomic<bool>& cancelled, GatewayResponse& response) override;
};

}  // namespace pocket
