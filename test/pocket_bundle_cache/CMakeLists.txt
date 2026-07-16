add_executable(PocketBundleCacheTest
  PocketBundleCacheTest.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketBundleCache.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketBundleRuntime.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketCard.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketCardFixture.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketCardParser.cpp
)

target_include_directories(PocketBundleCacheTest PRIVATE
  ${REPO_ROOT}/src/activities/pocket
)

target_link_libraries(PocketBundleCacheTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PocketBundleCacheTest)
