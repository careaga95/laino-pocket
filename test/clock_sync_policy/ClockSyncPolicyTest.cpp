#include <gtest/gtest.h>

#include "ClockSyncPolicy.h"

TEST(ClockSyncPolicyTest, LegacyTimeOnlyInstallationRequiresOneUpgradeSync) {
  EXPECT_TRUE(ClockSyncPolicy::shouldAutoSync(true, 0));
}

TEST(ClockSyncPolicyTest, CompleteDateTimeVersionPreventsRepeatedAutoSync) {
  EXPECT_FALSE(ClockSyncPolicy::shouldAutoSync(true, ClockSyncPolicy::COMPLETE_DATE_TIME_VERSION));
}

TEST(ClockSyncPolicyTest, FreshInstallationRequiresSync) { EXPECT_TRUE(ClockSyncPolicy::shouldAutoSync(false, 0)); }

TEST(ClockSyncPolicyTest, LegacyBooleanResetStillForcesSync) {
  EXPECT_TRUE(ClockSyncPolicy::shouldAutoSync(false, ClockSyncPolicy::COMPLETE_DATE_TIME_VERSION));
}

TEST(ClockSyncPolicyTest, CompletionMarkerIsMonotonic) {
  EXPECT_EQ(ClockSyncPolicy::markComplete(0), ClockSyncPolicy::COMPLETE_DATE_TIME_VERSION);
  EXPECT_EQ(ClockSyncPolicy::markComplete(ClockSyncPolicy::COMPLETE_DATE_TIME_VERSION),
            ClockSyncPolicy::COMPLETE_DATE_TIME_VERSION);
  EXPECT_EQ(ClockSyncPolicy::markComplete(2), 2U);
}
