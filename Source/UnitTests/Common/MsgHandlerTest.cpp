// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Common/Assert.h"
#include "Common/MsgHandler.h"

namespace
{
std::vector<std::string> s_alerts;

bool CaptureAlert(const char* caption, const char* text, bool yes_no, Common::MsgType style)
{
  EXPECT_STREQ(caption, "Warning");
  EXPECT_TRUE(yes_no);
  EXPECT_EQ(style, Common::MsgType::Warning);
  s_alerts.emplace_back(text);
  return true;
}

class MsgHandlerTest : public testing::Test
{
protected:
  void SetUp() override
  {
    s_alerts.clear();
    m_previous_handler = Common::RegisterMsgAlertHandler(CaptureAlert);
  }

  void TearDown() override
  {
    Common::RegisterMsgAlertHandler(m_previous_handler);
    s_alerts.clear();
  }

private:
  Common::MsgAlertHandler m_previous_handler = nullptr;
};

TEST_F(MsgHandlerTest, AssertForwardsZeroOneAndTwoFormatArguments)
{
  ASSERT_MSG(COMMON, false, "zero arguments");
  ASSERT_MSG(COMMON, false, "one argument {}", 17);
  ASSERT_MSG(COMMON, false, "two arguments {} {}", 17, "second");

  ASSERT_EQ(s_alerts.size(), 3u);
  EXPECT_NE(s_alerts[0].find("zero arguments\n\n  Condition: false\n"), std::string::npos);
  EXPECT_NE(s_alerts[1].find("one argument 17\n\n  Condition: false\n"), std::string::npos);
  EXPECT_NE(s_alerts[2].find("two arguments 17 second\n\n  Condition: false\n"), std::string::npos);
  for (const auto& text : s_alerts)
  {
    EXPECT_NE(text.find("MsgHandlerTest.cpp\n  Line: "), std::string::npos);
    EXPECT_NE(text.find("\n  Function: TestBody\n\nIgnore and continue?"), std::string::npos);
  }
}

TEST_F(MsgHandlerTest, SuccessfulConditionDoesNotEvaluateArgumentsOrReachSink)
{
  int evaluations = 0;
  ASSERT_MSG(COMMON, true, "not evaluated {}", ++evaluations);
  EXPECT_EQ(evaluations, 0);
  EXPECT_TRUE(s_alerts.empty());
}

TEST_F(MsgHandlerTest, HandlerRegistrationReturnsAndRestoresPreviousOwner)
{
  const auto previous_handler = Common::RegisterMsgAlertHandler(nullptr);
  EXPECT_EQ(previous_handler, CaptureAlert);
  EXPECT_EQ(Common::RegisterMsgAlertHandler(previous_handler), nullptr);
  EXPECT_TRUE(PanicYesNoFmt("restored handler"));
  ASSERT_EQ(s_alerts.size(), 1u);
  EXPECT_EQ(s_alerts.front(), "restored handler");
}
}  // namespace
