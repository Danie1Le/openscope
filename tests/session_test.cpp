#include "openscope/session.hpp"

#include <gtest/gtest.h>

#include <string>

#include "openscope/scpi_error.hpp"

namespace openscope {
namespace {

// Handlers build error text at run time (a parameter name, a measurement status), so a queued
// error must not point into the caller's string.
TEST(SessionTest, QueuedErrorOwnsItsMessageText) {
  Session session;
  {
    std::string detail = "Parameter error; value out of range for this channel";
    session.push_error(ScpiError{.code = -222, .message = detail});
    detail.assign(detail.size(), 'x');  // Reuses the buffer a view would still point into.
  }

  const ScpiError error = session.pop_error();

  EXPECT_EQ(error.code, -222);
  EXPECT_EQ(error.message, "Parameter error; value out of range for this channel");
}

TEST(ErrorReplyTest, IsTheCodeThenTheQuotedMessage) {
  EXPECT_EQ(error_reply(kNoError), R"(0,"No error")");
  EXPECT_EQ(error_reply(kSettingsConflict), R"(-221,"Settings conflict")");
}

}  // namespace
}  // namespace openscope
