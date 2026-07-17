add_executable(PocketPairingTest
  PocketPairingTest.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketCredential.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketCredentialStorage.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketPairingClient.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketPairingProtocol.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketPairingWorker.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketPairingTransportPolicy.cpp
)

target_include_directories(PocketPairingTest PRIVATE
  ${REPO_ROOT}/src/activities/pocket
)

target_link_libraries(PocketPairingTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_compile_definitions(PocketPairingTest PRIVATE
  PAIRING_ACTIVITY_SOURCE="${REPO_ROOT}/src/activities/pocket/PocketPairingActivity.cpp"
)

gtest_discover_tests(PocketPairingTest)
