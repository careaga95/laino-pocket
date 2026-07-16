add_executable(HalClockDateTimeTest
  HalClockDateTimeTest.cpp
  ${REPO_ROOT}/lib/hal/HalClockDateTime.cpp
)

target_include_directories(HalClockDateTimeTest PRIVATE
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(HalClockDateTimeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HalClockDateTimeTest)
