add_executable(PocketReadingTest
  PocketReadingTest.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketBundleCache.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketCard.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketCardParser.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketReading.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketReadingCache.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketReadingParser.cpp
)

target_include_directories(PocketReadingTest PRIVATE
  ${REPO_ROOT}/src/activities/pocket
  ${REPO_ROOT}/.pio/libdeps/gh_release/ArduinoJson/src
)

target_link_libraries(PocketReadingTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PocketReadingTest)
