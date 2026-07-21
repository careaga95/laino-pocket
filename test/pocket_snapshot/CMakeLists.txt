add_executable(PocketSnapshotTest
  PocketSnapshotTest.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketSnapshot.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketSnapshotParser.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketSnapshotCache.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketBundleCache.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketCard.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketCardParser.cpp
)

target_include_directories(PocketSnapshotTest PRIVATE
  ${REPO_ROOT}/src/activities/pocket
  ${REPO_ROOT}/.pio/libdeps/gh_release/ArduinoJson/src
)

target_link_libraries(PocketSnapshotTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PocketSnapshotTest)
