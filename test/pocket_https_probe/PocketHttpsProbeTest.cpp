#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "PocketHttpsProbe.h"

namespace {

class FakeTransport final : public pocket::HttpsProbeTransport {
 public:
  pocket::HttpsProbeResponseHead head{pocket::HttpsProbeOpenStatus::Ready, 200, -1, 0};
  std::vector<uint8_t> body;
  pocket::HttpsProbeReadStatus terminalStatus = pocket::HttpsProbeReadStatus::End;
  int32_t terminalError = 0;
  uint32_t elapsed = 37;
  int openCalls = 0;
  int readCalls = 0;
  int closeCalls = 0;

  pocket::HttpsProbeResponseHead open() override {
    ++openCalls;
    return head;
  }

  pocket::HttpsProbeReadResult readBody(uint8_t* buffer, const std::size_t capacity) override {
    ++readCalls;
    if (offset == body.size()) {
      return {terminalStatus, 0, terminalError};
    }
    const std::size_t count = std::min(capacity, body.size() - offset);
    std::memcpy(buffer, body.data() + offset, count);
    offset += count;
    return {pocket::HttpsProbeReadStatus::Data, count, 0};
  }

  uint32_t elapsedMs() const override { return elapsed; }
  void close() override { ++closeCalls; }

 private:
  std::size_t offset = 0;
};

std::vector<uint8_t> bytes(const std::size_t count) { return std::vector<uint8_t>(count, 0x5a); }

}  // namespace

TEST(PocketHttpsProbeTest, NoWifiIsClassifiedWithoutOpeningTransport) {
  FakeTransport transport;

  const auto result = pocket::runHttpsProbe(false, transport);

  EXPECT_EQ(result.code, pocket::HttpsProbeResultCode::NoWifi);
  EXPECT_EQ(transport.openCalls, 0);
  EXPECT_EQ(transport.closeCalls, 1);
}

TEST(PocketHttpsProbeTest, Successful2xxResponseReportsStatusBytesAndElapsedTime) {
  FakeTransport transport;
  transport.head = {pocket::HttpsProbeOpenStatus::Ready, 204, 4, 0};
  transport.body = bytes(4);
  transport.elapsed = 1234;

  const auto result = pocket::runHttpsProbe(true, transport);

  EXPECT_EQ(result.code, pocket::HttpsProbeResultCode::Success);
  EXPECT_EQ(result.httpStatus, 204);
  EXPECT_EQ(result.responseBytes, 4);
  EXPECT_EQ(result.elapsedMs, 1234U);
  EXPECT_EQ(transport.closeCalls, 1);
}

TEST(PocketHttpsProbeTest, Non2xxResponseIsHttpFailureWithoutReadingBody) {
  FakeTransport transport;
  transport.head = {pocket::HttpsProbeOpenStatus::Ready, 503, 12, 0};

  const auto result = pocket::runHttpsProbe(true, transport);

  EXPECT_EQ(result.code, pocket::HttpsProbeResultCode::HttpFailure);
  EXPECT_EQ(result.httpStatus, 503);
  EXPECT_EQ(transport.readCalls, 0);
}

TEST(PocketHttpsProbeTest, DeclaredContentLengthExactly1024Succeeds) {
  FakeTransport transport;
  transport.head.contentLength = 1024;
  transport.body = bytes(1024);

  const auto result = pocket::runHttpsProbe(true, transport);

  EXPECT_EQ(result.code, pocket::HttpsProbeResultCode::Success);
  EXPECT_EQ(result.responseBytes, 1024);
}

TEST(PocketHttpsProbeTest, DeclaredContentLength1025IsRejectedBeforeBodyRead) {
  FakeTransport transport;
  transport.head.contentLength = 1025;
  transport.body = bytes(1025);

  const auto result = pocket::runHttpsProbe(true, transport);

  EXPECT_EQ(result.code, pocket::HttpsProbeResultCode::OversizedResponse);
  EXPECT_EQ(transport.readCalls, 0);
}

TEST(PocketHttpsProbeTest, UnknownOrChunkedLengthWithinLimitSucceeds) {
  FakeTransport transport;
  transport.head.contentLength = -1;
  transport.body = bytes(1024);

  const auto result = pocket::runHttpsProbe(true, transport);

  EXPECT_EQ(result.code, pocket::HttpsProbeResultCode::Success);
  EXPECT_EQ(result.responseBytes, 1024);
}

TEST(PocketHttpsProbeTest, UnknownOrChunkedLengthCrossingLimitIsRejectedAt1025) {
  FakeTransport transport;
  transport.head.contentLength = -1;
  transport.body = bytes(1025);

  const auto result = pocket::runHttpsProbe(true, transport);

  EXPECT_EQ(result.code, pocket::HttpsProbeResultCode::OversizedResponse);
  EXPECT_EQ(result.responseBytes, 1025);
}

TEST(PocketHttpsProbeTest, OpenTimeoutIsClassifiedAndPreservesTransportError) {
  FakeTransport transport;
  transport.head = {pocket::HttpsProbeOpenStatus::Timeout, 0, -1, -3};

  const auto result = pocket::runHttpsProbe(true, transport);

  EXPECT_EQ(result.code, pocket::HttpsProbeResultCode::Timeout);
  EXPECT_EQ(result.transportError, -3);
}

TEST(PocketHttpsProbeTest, BodyTimeoutIsClassifiedAfterBytesAlreadyConsumed) {
  FakeTransport transport;
  transport.body = bytes(10);
  transport.terminalStatus = pocket::HttpsProbeReadStatus::Timeout;
  transport.terminalError = -4;

  const auto result = pocket::runHttpsProbe(true, transport);

  EXPECT_EQ(result.code, pocket::HttpsProbeResultCode::Timeout);
  EXPECT_EQ(result.responseBytes, 10);
  EXPECT_EQ(result.transportError, -4);
}

TEST(PocketHttpsProbeTest, TransportReadFailureIsClassified) {
  FakeTransport transport;
  transport.body = bytes(7);
  transport.terminalStatus = pocket::HttpsProbeReadStatus::Failure;
  transport.terminalError = -99;

  const auto result = pocket::runHttpsProbe(true, transport);

  EXPECT_EQ(result.code, pocket::HttpsProbeResultCode::ReadFailure);
  EXPECT_EQ(result.responseBytes, 7);
  EXPECT_EQ(result.transportError, -99);
}

TEST(PocketHttpsProbeTest, RedirectIsRejectedWithoutFollowingOrReading) {
  FakeTransport transport;
  transport.head = {pocket::HttpsProbeOpenStatus::Ready, 302, 0, 0};

  const auto result = pocket::runHttpsProbe(true, transport);

  EXPECT_EQ(result.code, pocket::HttpsProbeResultCode::RedirectRejected);
  EXPECT_EQ(transport.readCalls, 0);
}

TEST(PocketHttpsProbeTest, TlsFailureRemainsDistinctFromConnectFailure) {
  FakeTransport tlsTransport;
  tlsTransport.head = {pocket::HttpsProbeOpenStatus::TlsFailure, 0, -1, -9984};
  FakeTransport connectTransport;
  connectTransport.head = {pocket::HttpsProbeOpenStatus::DnsOrConnectFailure, 0, -1, -1};

  EXPECT_EQ(pocket::runHttpsProbe(true, tlsTransport).code, pocket::HttpsProbeResultCode::TlsFailure);
  EXPECT_EQ(pocket::runHttpsProbe(true, connectTransport).code, pocket::HttpsProbeResultCode::DnsOrConnectFailure);
}

TEST(PocketHttpsProbeTest, DeclaredBodyEndingEarlyIsReadFailure) {
  FakeTransport transport;
  transport.head.contentLength = 10;
  transport.body = bytes(9);

  const auto result = pocket::runHttpsProbe(true, transport);

  EXPECT_EQ(result.code, pocket::HttpsProbeResultCode::ReadFailure);
  EXPECT_EQ(result.responseBytes, 9);
}

TEST(PocketHttpsProbeTest, ResultFormattingIsAlwaysNullTerminatedWhenTruncated) {
  const pocket::HttpsProbeResult result{pocket::HttpsProbeResultCode::TlsFailure, 0, 0, 12000, -9984};
  char output[8];
  std::memset(output, 'x', sizeof(output));

  pocket::formatHttpsProbeResult(result, output, sizeof(output));

  EXPECT_EQ(output[sizeof(output) - 1], '\0');
  EXPECT_STREQ(output, "TLS val");
}

TEST(PocketHttpsProbeTest, RetryResetClearsEveryPriorResultField) {
  pocket::HttpsProbePresentation presentation;
  presentation.complete({pocket::HttpsProbeResultCode::Success, 200, 513, 999, -17});
  ASSERT_FALSE(presentation.isTesting());

  presentation.reset();

  EXPECT_TRUE(presentation.isTesting());
  EXPECT_EQ(presentation.result().code, pocket::HttpsProbeResultCode::InternalFailure);
  EXPECT_EQ(presentation.result().httpStatus, 0);
  EXPECT_EQ(presentation.result().responseBytes, 0);
  EXPECT_EQ(presentation.result().elapsedMs, 0U);
  EXPECT_EQ(presentation.result().transportError, 0);
}
