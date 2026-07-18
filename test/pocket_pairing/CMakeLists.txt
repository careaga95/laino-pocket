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

file(STRINGS "${REPO_ROOT}/platformio.ini" POCKET_BASE_VERSION_LINE
  REGEX "^version = [0-9]+\\.[0-9]+\\.[0-9]+$"
  LIMIT_COUNT 1
)
string(REGEX REPLACE "^version = " "" POCKET_BASE_VERSION "${POCKET_BASE_VERSION_LINE}")
execute_process(
  COMMAND git rev-parse --short=7 HEAD
  WORKING_DIRECTORY "${REPO_ROOT}"
  RESULT_VARIABLE POCKET_GIT_RESULT
  OUTPUT_VARIABLE POCKET_GIT_SHA
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT POCKET_GIT_RESULT EQUAL 0 OR POCKET_GIT_SHA STREQUAL "")
  message(FATAL_ERROR "Pocket firmware tests require an exact Git commit")
endif()
target_compile_definitions(PocketPairingTest PRIVATE
  POCKET_FIRMWARE_ID="${POCKET_BASE_VERSION}+${POCKET_GIT_SHA}"
)

add_executable(PocketPairingPreflight
  PocketPairingPreflight.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketCredential.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketPairingClient.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketPairingProtocol.cpp
)
target_include_directories(PocketPairingPreflight PRIVATE
  ${REPO_ROOT}/src/activities/pocket
)
target_link_libraries(PocketPairingPreflight PRIVATE
  crosspoint_test_common
)
target_compile_definitions(PocketPairingPreflight PRIVATE
  POCKET_FIRMWARE_ID="${POCKET_BASE_VERSION}+${POCKET_GIT_SHA}"
)

gtest_discover_tests(PocketPairingTest)
