add_executable(PocketHttpsProbeTest
  PocketHttpsProbeTest.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketHttpsProbe.cpp
)

target_include_directories(PocketHttpsProbeTest PRIVATE
  ${REPO_ROOT}/src/activities/pocket
)

target_link_libraries(PocketHttpsProbeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PocketHttpsProbeTest)
