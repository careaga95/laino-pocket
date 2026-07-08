#pragma once
// Host-test shim for the firmware Logging.h (which drags in Arduino Serial).
#include <cstdio>
#define LOG_DBG(tag, ...) (std::printf("[%s] ", tag), std::printf(__VA_ARGS__), std::printf("\n"))
#define LOG_INF(tag, ...) (std::printf("[%s] ", tag), std::printf(__VA_ARGS__), std::printf("\n"))
#define LOG_ERR(tag, ...) (std::printf("[%s] ", tag), std::printf(__VA_ARGS__), std::printf("\n"))
