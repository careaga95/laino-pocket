#pragma once

#include "PocketHttpsProbe.h"

namespace pocket {

inline constexpr const char* HTTPS_PROBE_URL = "https://example.com/";

HttpsProbeResult runPublicHttpsProbe();

}  // namespace pocket
