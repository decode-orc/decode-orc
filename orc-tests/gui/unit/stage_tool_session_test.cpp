/*
 * File:        stage_tool_session_test.cpp
 * Module:      orc-gui unit tests (gui-model)
 * Purpose:     A plugin view's tool session: replies in request order
 *              whatever order the transport answers in, and nothing reaches
 *              the view once the session has closed
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "presenters/include/stage_tool_session.h"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace orc_unit_test {
namespace {

using orc::presenters::IStageToolTransport;
using orc::presenters::StageToolSession;
using orc::view::ToolReply;

// Holds every request until the test answers it, in whatever order.
class HeldTransport final : public IStageToolTransport {
 public:
  struct Sent {
    orc::NodeID node;
    std::string contract_id;
    std::string verb;
    std::vector<uint8_t> payload;
    std::function<void(ToolReply)> on_done;
  };

  void send(orc::NodeID node, const std::string& contract_id,
            const std::string& verb, std::vector<uint8_t> payload,
            std::function<void(ToolReply)> on_done) override {
    sent.push_back(
        {node, contract_id, verb, std::move(payload), std::move(on_done)});
  }

  void answer(size_t index, const std::string& text) {
    ToolReply reply;
    reply.ok = true;
    reply.data.assign(text.begin(), text.end());
    sent.at(index).on_done(std::move(reply));
  }

  std::vector<Sent> sent;
};

std::string text_of(const ToolReply& reply) {
  return std::string(reply.data.begin(), reply.data.end());
}

struct Fixture {
  std::shared_ptr<HeldTransport> transport = std::make_shared<HeldTransport>();
  std::shared_ptr<StageToolSession> session =
      std::make_shared<StageToolSession>(transport, orc::NodeID(4),
                                         "test.view.v1", "Pages - Sink (4)");
  std::vector<std::string> delivered;

  void ask(const std::string& verb) {
    session->request(verb, {}, [this](ToolReply reply) {
      delivered.push_back(text_of(reply));
    });
  }
};

}  // namespace

TEST(StageToolSessionTest, SendsTheRequestToTheNodesStageUntouched) {
  Fixture f;
  f.session->request("page", {1, 2, 3}, [](ToolReply) {});

  ASSERT_EQ(f.transport->sent.size(), 1u);
  const auto& sent = f.transport->sent[0];
  EXPECT_EQ(sent.node, orc::NodeID(4));
  EXPECT_EQ(sent.contract_id, "test.view.v1");
  EXPECT_EQ(sent.verb, "page");
  EXPECT_EQ(sent.payload, (std::vector<uint8_t>{1, 2, 3}));
  EXPECT_EQ(f.session->contract_id(), "test.view.v1");
  EXPECT_EQ(f.session->title(), "Pages - Sink (4)");
  EXPECT_TRUE(f.session->is_open());
}

TEST(StageToolSessionTest, RepliesArriveInRequestOrderWhenAnsweredInOrder) {
  Fixture f;
  f.ask("a");
  f.ask("b");
  f.ask("c");
  f.transport->answer(0, "A");
  f.transport->answer(1, "B");
  f.transport->answer(2, "C");
  EXPECT_EQ(f.delivered, (std::vector<std::string>{"A", "B", "C"}));
}

// A later answer waits for the earlier ones, then they all go together.
TEST(StageToolSessionTest, AnOutOfOrderReplyIsHeldUntilItsTurn) {
  Fixture f;
  f.ask("a");
  f.ask("b");
  f.ask("c");

  f.transport->answer(2, "C");
  EXPECT_TRUE(f.delivered.empty());
  f.transport->answer(1, "B");
  EXPECT_TRUE(f.delivered.empty());
  f.transport->answer(0, "A");
  EXPECT_EQ(f.delivered, (std::vector<std::string>{"A", "B", "C"}));
}

TEST(StageToolSessionTest, AFailedReplyStillTakesItsTurn) {
  Fixture f;
  std::vector<bool> oks;
  for (int i = 0; i < 2; ++i) {
    f.session->request("x", {},
                       [&oks](ToolReply reply) { oks.push_back(reply.ok); });
  }
  ToolReply refused;
  refused.error = "The node's stage does not answer tool requests";
  f.transport->answer(1, "fine");
  f.transport->sent[0].on_done(refused);
  EXPECT_EQ(oks, (std::vector<bool>{false, true}));
}

// --- Shutdown
// -----------------------------------------------------------------

TEST(StageToolSessionTest, CloseDropsEveryReplyStillInFlight) {
  Fixture f;
  f.ask("a");
  f.ask("b");
  f.session->close();
  f.transport->answer(0, "A");
  f.transport->answer(1, "B");
  EXPECT_TRUE(f.delivered.empty());
  EXPECT_FALSE(f.session->is_open());
}

TEST(StageToolSessionTest, ARequestOnAClosedSessionIsNeverSent) {
  Fixture f;
  f.session->close();
  f.ask("a");
  EXPECT_TRUE(f.transport->sent.empty());
  EXPECT_TRUE(f.delivered.empty());
}

// The view's widget can be destroyed (and the session with it) while its
// requests are still being answered on the render worker.
TEST(StageToolSessionTest, AReplyAfterTheSessionIsGoneFindsNothingToCall) {
  Fixture f;
  bool called = false;
  f.session->request("a", {}, [&called](ToolReply) { called = true; });
  f.session.reset();
  f.transport->answer(0, "A");
  EXPECT_FALSE(called);
}

// A view may close itself from a reply handler, as a dialog does when it is
// told the node is gone. The replies after it are not delivered.
TEST(StageToolSessionTest, AHandlerThatClosesTheSessionStopsDelivery) {
  Fixture f;
  std::vector<std::string> seen;
  f.session->request("a", {}, [&](ToolReply reply) {
    seen.push_back(text_of(reply));
    f.session->close();
  });
  f.session->request("b", {},
                     [&](ToolReply reply) { seen.push_back(text_of(reply)); });
  f.transport->answer(1, "B");
  f.transport->answer(0, "A");
  EXPECT_EQ(seen, (std::vector<std::string>{"A"}));
}

// --- Results changed
// ------------------------------------------------------------

TEST(StageToolSessionTest, ResultsChangedReachesTheViewUntilClosed) {
  Fixture f;
  int changes = 0;
  f.session->set_results_changed_handler([&changes] { ++changes; });
  f.session->notifyResultsChanged();
  EXPECT_EQ(changes, 1);

  f.session->close();
  f.session->notifyResultsChanged();
  f.session->set_results_changed_handler([&changes] { changes += 10; });
  f.session->notifyResultsChanged();
  EXPECT_EQ(changes, 1);
}

}  // namespace orc_unit_test
