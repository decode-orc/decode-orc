/*
 * File:        render_coordinator_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Actor-style tests for RenderCoordinator request/response
 * behavior
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "render_coordinator.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QMetaType>
#include <QSignalSpy>
#include <QThread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "mocks/mock_render_presenter.h"

Q_DECLARE_METATYPE(orc::PreviewRenderResult)
Q_DECLARE_METATYPE(PreviewRenderDeliveryPtr)
Q_DECLARE_METATYPE(FrameSamplesDeliveryPtr)
Q_DECLARE_METATYPE(LineSamplesDeliveryPtr)
Q_DECLARE_METATYPE(orc::presenters::VideoParameterObservationView)
Q_DECLARE_METATYPE(orc::presenters::NtscFieldObservationsView)
Q_DECLARE_METATYPE(orc::CatalogueDataset)
Q_DECLARE_METATYPE(std::vector<orc::AudioPairView>)
Q_DECLARE_METATYPE(std::shared_ptr<orc::presenters::IAudioStreamReader>)

namespace gui_unit_test {

using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;

static bool registerRenderCoordinatorMetatypes() {
  qRegisterMetaType<orc::PreviewRenderResult>("orc::PreviewRenderResult");
  qRegisterMetaType<PreviewRenderDeliveryPtr>("PreviewRenderDeliveryPtr");
  qRegisterMetaType<FrameSamplesDeliveryPtr>("FrameSamplesDeliveryPtr");
  qRegisterMetaType<LineSamplesDeliveryPtr>("LineSamplesDeliveryPtr");
  qRegisterMetaType<orc::presenters::VideoParameterObservationView>(
      "orc::presenters::VideoParameterObservationView");
  qRegisterMetaType<orc::presenters::NtscFieldObservationsView>(
      "orc::presenters::NtscFieldObservationsView");
  qRegisterMetaType<orc::CatalogueDataset>("orc::CatalogueDataset");
  return true;
}

static const bool kMetatypesRegistered = registerRenderCoordinatorMetatypes();

static bool waitForCount(QSignalSpy& spy, int expected, int timeout_ms = 2000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents();
    if (spy.count() >= expected) {
      return true;
    }
    QThread::msleep(5);
  }
  return spy.count() >= expected;
}

TEST(RenderCoordinatorTest, TriggerRequestRoundTrip_EmitsTriggerComplete) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  EXPECT_CALL(*mock_presenter, setDAG(testing::_)).Times(1);
  EXPECT_CALL(*mock_presenter, setShowDropouts(false)).Times(1);
  EXPECT_CALL(*mock_presenter, triggerStage(orc::NodeID(7), testing::_))
      .WillOnce(Return(1001));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy trigger_complete_spy(&coordinator,
                                  &RenderCoordinator::triggerComplete);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(123));

  const uint64_t request_id = coordinator.requestTrigger(orc::NodeID(7));

  ASSERT_TRUE(waitForCount(trigger_complete_spy, 1));
  ASSERT_EQ(trigger_complete_spy.count(), 1);
  EXPECT_EQ(trigger_complete_spy.at(0).at(0).toULongLong(), request_id);
  EXPECT_TRUE(trigger_complete_spy.at(0).at(1).toBool());

  coordinator.stop();
}

TEST(RenderCoordinatorTest, WorkerLifecycleStartStop_IsStable) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  coordinator.start();
  coordinator.stop();

  coordinator.start();
  coordinator.stop();
}

TEST(RenderCoordinatorTest, TriggerRequests_AreProcessedInOrder) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  EXPECT_CALL(*mock_presenter, setDAG(testing::_)).Times(1);
  EXPECT_CALL(*mock_presenter, setShowDropouts(false)).Times(1);

  {
    InSequence seq;
    EXPECT_CALL(*mock_presenter, triggerStage(orc::NodeID(1), testing::_))
        .WillOnce(Invoke(
            [](orc::NodeID,
               orc::presenters::IRenderPresenter::TriggerProgressCallback) {
              std::this_thread::sleep_for(std::chrono::milliseconds(15));
              return 2001ULL;
            }));
    EXPECT_CALL(*mock_presenter, triggerStage(orc::NodeID(2), testing::_))
        .WillOnce(Return(2002ULL));
  }

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy trigger_complete_spy(&coordinator,
                                  &RenderCoordinator::triggerComplete);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(234));

  const uint64_t first_id = coordinator.requestTrigger(orc::NodeID(1));
  const uint64_t second_id = coordinator.requestTrigger(orc::NodeID(2));

  ASSERT_TRUE(waitForCount(trigger_complete_spy, 2));
  ASSERT_EQ(trigger_complete_spy.count(), 2);

  EXPECT_EQ(trigger_complete_spy.at(0).at(0).toULongLong(), first_id);
  EXPECT_EQ(trigger_complete_spy.at(1).at(0).toULongLong(), second_id);

  coordinator.stop();
}

namespace {

// A render the test can hold open: it announces that it has started, then
// blocks until released. Lets a test fill the request queue behind a render
// that is provably already in flight, instead of racing the worker.
struct BlockingRender {
  std::atomic<bool> started{false};
  std::atomic<bool> release{false};
  std::atomic<int> calls{0};

  void wait() {
    started.store(true);
    while (!release.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
};

orc::PreviewRenderResult makeRenderResult(orc::NodeID node_id,
                                          orc::PreviewOutputType output_type,
                                          uint64_t output_index) {
  return orc::PreviewRenderResult{
      {}, true, "", node_id, output_type, output_index, std::nullopt};
}

// Spin the calling thread (no event loop needed) until @p predicate holds.
template <typename Predicate>
bool waitForFlag(Predicate predicate, int timeout_ms = 2000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

}  // namespace

TEST(RenderCoordinatorTest, StalePreviewResponses_AreSuppressed) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  EXPECT_CALL(*mock_presenter, setDAG(testing::_)).Times(1);
  EXPECT_CALL(*mock_presenter, setShowDropouts(false)).Times(1);

  auto blocker = std::make_shared<BlockingRender>();
  EXPECT_CALL(
      *mock_presenter,
      renderPreview(orc::NodeID(9), orc::PreviewOutputType::Frame_Field1, 0, "",
                    testing::_))
      .Times(2)
      .WillRepeatedly(Invoke(
          [blocker](orc::NodeID node_id, orc::PreviewOutputType output_type,
                    uint64_t output_index, const std::string&,
                    orc::PreviewNavigationHint) {
            if (blocker->calls.fetch_add(1) == 0) {
              blocker->wait();
            }
            return makeRenderResult(node_id, output_type, output_index);
          }));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy preview_spy(&coordinator, &RenderCoordinator::previewReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(345));

  const uint64_t first_id = coordinator.requestPreview(
      orc::NodeID(9), orc::PreviewOutputType::Frame_Field1, 0);

  // The second request must be queued while the first is already rendering;
  // otherwise it would supersede the first in the queue and the first would
  // never run at all (that is the pruning test below, not this one).
  ASSERT_TRUE(waitForFlag([&] { return blocker->started.load(); }));
  const uint64_t second_id = coordinator.requestPreview(
      orc::NodeID(9), orc::PreviewOutputType::Frame_Field1, 0);
  blocker->release.store(true);

  ASSERT_TRUE(waitForCount(preview_spy, 1));
  EXPECT_EQ(preview_spy.count(), 1);
  EXPECT_EQ(preview_spy.at(0).at(0).toULongLong(), second_id);
  EXPECT_NE(first_id, second_id);

  coordinator.stop();
}

// A burst of navigations (playback catching up, or a scrub that outruns the
// renderer) used to queue one render per position; every one but the last was
// rendered in full and then thrown away. Only the newest is ever displayed, so
// the queued ones are dropped before they cost anything.
TEST(RenderCoordinatorTest, SupersededQueuedPreviews_AreDiscardedUnrendered) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  EXPECT_CALL(*mock_presenter, setDAG(testing::_)).Times(1);
  EXPECT_CALL(*mock_presenter, setShowDropouts(false)).Times(1);

  auto blocker = std::make_shared<BlockingRender>();
  std::mutex rendered_mutex;
  std::vector<uint64_t> rendered_indices;

  EXPECT_CALL(*mock_presenter, renderPreview(orc::NodeID(4), testing::_,
                                             testing::_, "", testing::_))
      .WillRepeatedly(Invoke(
          [&, blocker](orc::NodeID node_id, orc::PreviewOutputType output_type,
                       uint64_t output_index, const std::string&,
                       orc::PreviewNavigationHint) {
            {
              const std::lock_guard<std::mutex> lock(rendered_mutex);
              rendered_indices.push_back(output_index);
            }
            if (blocker->calls.fetch_add(1) == 0) {
              blocker->wait();
            }
            return makeRenderResult(node_id, output_type, output_index);
          }));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy preview_spy(&coordinator, &RenderCoordinator::previewReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(456));

  // Index 0 occupies the worker; 1..7 pile up behind it.
  coordinator.requestPreview(orc::NodeID(4),
                             orc::PreviewOutputType::Frame_Field1, 0);
  ASSERT_TRUE(waitForFlag([&] { return blocker->started.load(); }));

  uint64_t last_id = 0;
  for (uint64_t index = 1; index <= 7; ++index) {
    last_id = coordinator.requestPreview(
        orc::NodeID(4), orc::PreviewOutputType::Frame_Field1, index);
  }
  blocker->release.store(true);

  ASSERT_TRUE(waitForCount(preview_spy, 1));
  EXPECT_EQ(preview_spy.count(), 1);
  EXPECT_EQ(preview_spy.at(0).at(0).toULongLong(), last_id);

  // Give any wrongly-retained request time to be rendered before asserting.
  QThread::msleep(50);
  QCoreApplication::processEvents();

  const std::lock_guard<std::mutex> lock(rendered_mutex);
  EXPECT_EQ(rendered_indices, (std::vector<uint64_t>{0, 7}))
      << "superseded queued previews were rendered instead of discarded";

  coordinator.stop();
}

// The hint is what tells a stage that playback is running and adjacent frames
// are worth pre-fetching. It has to survive the trip through the queue.
TEST(RenderCoordinatorTest, PreviewNavigationHint_IsForwardedToThePresenter) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  EXPECT_CALL(*mock_presenter, setDAG(testing::_)).Times(1);
  EXPECT_CALL(*mock_presenter, setShowDropouts(false)).Times(1);

  EXPECT_CALL(*mock_presenter,
              renderPreview(orc::NodeID(5), testing::_, 3, "",
                            orc::PreviewNavigationHint::Sequential))
      .WillOnce(
          Invoke([](orc::NodeID node_id, orc::PreviewOutputType output_type,
                    uint64_t output_index, const std::string&,
                    orc::PreviewNavigationHint) {
            return makeRenderResult(node_id, output_type, output_index);
          }));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy preview_spy(&coordinator, &RenderCoordinator::previewReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(567));

  coordinator.requestPreview(orc::NodeID(5),
                             orc::PreviewOutputType::Frame_Field1, 3, "",
                             orc::PreviewNavigationHint::Sequential);

  ASSERT_TRUE(waitForCount(preview_spy, 1));
  coordinator.stop();
}

// Scrubbing and one-off navigations must keep rendering exactly as before.
TEST(RenderCoordinatorTest, PreviewRequestsDefaultToTheRandomHint) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  EXPECT_CALL(*mock_presenter, setDAG(testing::_)).Times(1);
  EXPECT_CALL(*mock_presenter, setShowDropouts(false)).Times(1);

  EXPECT_CALL(*mock_presenter,
              renderPreview(orc::NodeID(6), testing::_, 2, "",
                            orc::PreviewNavigationHint::Random))
      .WillOnce(
          Invoke([](orc::NodeID node_id, orc::PreviewOutputType output_type,
                    uint64_t output_index, const std::string&,
                    orc::PreviewNavigationHint) {
            return makeRenderResult(node_id, output_type, output_index);
          }));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy preview_spy(&coordinator, &RenderCoordinator::previewReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(678));

  coordinator.requestPreview(orc::NodeID(6),
                             orc::PreviewOutputType::Frame_Field1, 2);

  ASSERT_TRUE(waitForCount(preview_spy, 1));
  coordinator.stop();
}

// Poll the GUI event loop until @p predicate holds or the timeout elapses.
template <typename Predicate>
static bool waitForPredicate(Predicate predicate, int timeout_ms = 2000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents();
    if (predicate()) {
      return true;
    }
    QThread::msleep(5);
  }
  return predicate();
}

// Phase 3 Task 3.3: an invalidation from the presenter is forwarded to the
// observationsInvalidated signal with the correct node set.
TEST(RenderCoordinatorTest, ObservationInvalidation_ForwardedToSignal) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy invalidation_spy(&coordinator,
                              &RenderCoordinator::observationsInvalidated);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  // The coordinator subscribes when it creates the presenter on the worker.
  ASSERT_TRUE(waitForPredicate(
      [&] { return mock_presenter->invalidationSubscriberCount() == 1; }));

  mock_presenter->fireInvalidation({orc::NodeID(3), orc::NodeID(5)});

  ASSERT_TRUE(waitForCount(invalidation_spy, 1));
  ASSERT_EQ(invalidation_spy.count(), 1);
  const auto ids = invalidation_spy.at(0).at(0).value<QVector<int>>();
  EXPECT_EQ(ids, (QVector<int>{3, 5}));

  coordinator.stop();
}

// Once the coordinator unsubscribes (presenter torn down on empty project),
// further invalidations are not delivered.
TEST(RenderCoordinatorTest, ObservationInvalidation_UnsubscribeStopsDelivery) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy invalidation_spy(&coordinator,
                              &RenderCoordinator::observationsInvalidated);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));
  ASSERT_TRUE(waitForPredicate(
      [&] { return mock_presenter->invalidationSubscriberCount() == 1; }));

  // A null DAG (empty project) tears down the presenter and unsubscribes.
  coordinator.updateDAG(nullptr);
  ASSERT_TRUE(waitForPredicate(
      [&] { return mock_presenter->invalidationSubscriberCount() == 0; }));

  mock_presenter->fireInvalidation({orc::NodeID(7)});
  QCoreApplication::processEvents();
  EXPECT_EQ(invalidation_spy.count(), 0);

  coordinator.stop();
}

// --- Phase 5 Task 5.1: async observation requests ------------------------

// A store hit answers immediately: the presenter fires the callback
// synchronously and the coordinator emits observationDataReady(available=true).
TEST(RenderCoordinatorTest,
     ObservationRequest_StoreHit_EmitsDataReadyImmediate) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  mock_presenter->setImmediateAnswer(true);

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy data_spy(&coordinator, &RenderCoordinator::observationDataReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  const uint64_t request_id =
      coordinator.requestObservations(orc::NodeID(2), orc::FieldID(10));

  ASSERT_TRUE(waitForCount(data_spy, 1));
  ASSERT_EQ(data_spy.count(), 1);
  EXPECT_EQ(data_spy.at(0).at(0).toULongLong(), request_id);
  EXPECT_TRUE(data_spy.at(0).at(1).toBool());            // available
  EXPECT_EQ(data_spy.at(0).at(2).toULongLong(), 10ull);  // field id value

  coordinator.stop();
}

// A store miss defers: no signal until the awaited frame is delivered later.
TEST(RenderCoordinatorTest,
     ObservationRequest_Miss_EmitsAfterDeferredDelivery) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  mock_presenter->setImmediateAnswer(false);

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy data_spy(&coordinator, &RenderCoordinator::observationDataReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  const uint64_t request_id =
      coordinator.requestObservations(orc::NodeID(2), orc::FieldID(4));

  // The worker has forwarded the request; it now awaits a deferred delivery.
  ASSERT_TRUE(waitForPredicate(
      [&] { return mock_presenter->pendingObservationCount() == 1; }));
  QCoreApplication::processEvents();
  EXPECT_EQ(data_spy.count(), 0);  // nothing delivered yet

  // Background computation finishes -> the frame's observations arrive.
  ASSERT_TRUE(mock_presenter->deliverOldestObservation(/*available=*/true));

  ASSERT_TRUE(waitForCount(data_spy, 1));
  EXPECT_EQ(data_spy.at(0).at(0).toULongLong(), request_id);
  EXPECT_TRUE(data_spy.at(0).at(1).toBool());

  coordinator.stop();
}

// Concurrent requests carry distinct ids and each response echoes its own id,
// so a consumer can drop stale (superseded) responses.
TEST(RenderCoordinatorTest, ObservationRequest_DistinctIdsSupportStaleDrop) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  mock_presenter->setImmediateAnswer(false);

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy data_spy(&coordinator, &RenderCoordinator::observationDataReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  const uint64_t first =
      coordinator.requestObservations(orc::NodeID(2), orc::FieldID(4));
  const uint64_t second =
      coordinator.requestObservations(orc::NodeID(2), orc::FieldID(6));
  EXPECT_NE(first, second);

  ASSERT_TRUE(waitForPredicate(
      [&] { return mock_presenter->pendingObservationCount() == 2; }));

  // Deliver both (oldest first). Each response carries its originating id.
  ASSERT_TRUE(mock_presenter->deliverOldestObservation(true));
  ASSERT_TRUE(mock_presenter->deliverOldestObservation(true));

  ASSERT_TRUE(waitForCount(data_spy, 2));
  EXPECT_EQ(data_spy.at(0).at(0).toULongLong(), first);
  EXPECT_EQ(data_spy.at(1).at(0).toULongLong(), second);

  coordinator.stop();
}

// --- Catalogue browser: GetCatalogueData request/response -----------------

namespace {

// One trigger run's catalogue, as a stage would have cached it.
orc::CatalogueDataset makeCatalogue(const std::string& item_id) {
  orc::CatalogueDataset data;
  data.schema.columns = {orc::CatalogueColumn{"id", "Id", false}};
  data.schema.item_noun = "Page";
  orc::CatalogueItem item;
  item.id = item_id;
  item.find_key = item_id;
  item.values = {item_id};
  data.items.push_back(std::move(item));
  data.payloads.emplace_back();
  data.summary.headline = "100 packets recovered";
  return data;
}

}  // namespace

// A stage that has already been triggered answers straight from its cached
// catalogue, with no trigger run.
TEST(RenderCoordinatorTest, CatalogueRequest_ServesCachedCatalogue) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  EXPECT_CALL(*mock_presenter,
              getCatalogueData(orc::NodeID(2), std::string(), ::testing::_))
      .WillOnce(Return(makeCatalogue("100")));
  EXPECT_CALL(*mock_presenter, triggerStage(::testing::_, ::testing::_))
      .Times(0);

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy data_spy(&coordinator, &RenderCoordinator::catalogueDataReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  const uint64_t request_id = coordinator.requestCatalogueData(orc::NodeID(2));

  ASSERT_TRUE(waitForCount(data_spy, 1));
  EXPECT_EQ(data_spy.at(0).at(0).toULongLong(), request_id);
  const auto data = data_spy.at(0).at(1).value<orc::CatalogueDataset>();
  ASSERT_EQ(data.items.size(), 1u);
  EXPECT_EQ(data.items[0].id, "100");
  EXPECT_EQ(data.summary.headline, "100 packets recovered");

  coordinator.stop();
}

// Picking one of the schema's view options is still a read: it reaches the
// stage verbatim, and nothing is triggered to satisfy it.
TEST(RenderCoordinatorTest, CatalogueRequest_CarriesTheViewOption) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  EXPECT_CALL(
      *mock_presenter,
      getCatalogueData(orc::NodeID(2), std::string("512 x 400"), ::testing::_))
      .WillOnce(Return(makeCatalogue("100")));
  EXPECT_CALL(*mock_presenter, triggerStage(::testing::_, ::testing::_))
      .Times(0);

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy data_spy(&coordinator, &RenderCoordinator::catalogueDataReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  const uint64_t request_id =
      coordinator.requestCatalogueData(orc::NodeID(2), "512 x 400");

  ASSERT_TRUE(waitForCount(data_spy, 1));
  EXPECT_EQ(data_spy.at(0).at(0).toULongLong(), request_id);

  coordinator.stop();
}

// The first read is not a reader who has turned every toggle off — it is a
// reader who has not been asked yet, and the stage answers it with its own
// defaults. Sending an empty list instead would mean a stage could never offer
// a toggle that starts on, which is exactly what the NABTS records browser
// does with syntax repair.
TEST(RenderCoordinatorTest, CatalogueRequest_AsksForTheStagesOwnTogglesFirst) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  EXPECT_CALL(*mock_presenter, getCatalogueData(orc::NodeID(2), std::string(),
                                                ::testing::Eq(std::nullopt)))
      .WillOnce(Return(makeCatalogue("100")));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy data_spy(&coordinator, &RenderCoordinator::catalogueDataReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));
  coordinator.requestCatalogueData(orc::NodeID(2));

  ASSERT_TRUE(waitForCount(data_spy, 1));
  coordinator.stop();
}

// And once the reader has been asked, what they said travels verbatim —
// including "none of them", which is a different statement from the first read
// and must not be answered with the defaults.
TEST(RenderCoordinatorTest, CatalogueRequest_CarriesTheTogglesTheReaderChose) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  EXPECT_CALL(
      *mock_presenter,
      getCatalogueData(orc::NodeID(2), std::string(),
                       ::testing::Optional(::testing::ElementsAre("repair"))))
      .WillOnce(Return(makeCatalogue("100")));
  EXPECT_CALL(*mock_presenter,
              getCatalogueData(orc::NodeID(2), std::string(),
                               ::testing::Optional(::testing::IsEmpty())))
      .WillOnce(Return(makeCatalogue("101")));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy data_spy(&coordinator, &RenderCoordinator::catalogueDataReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));
  coordinator.requestCatalogueData(orc::NodeID(2), std::string(),
                                   std::vector<std::string>{"repair"});
  coordinator.requestCatalogueData(orc::NodeID(2), std::string(),
                                   std::vector<std::string>{});

  ASSERT_TRUE(waitForCount(data_spy, 2));
  coordinator.stop();
}

// Reading the catalogue of a node that has never been triggered runs nothing:
// the reader is told there is nothing to show, and the decode stays behind the
// Trigger Stage action where the reader put it.
TEST(RenderCoordinatorTest, CatalogueRequest_NeverTriggers) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  EXPECT_CALL(*mock_presenter,
              getCatalogueData(orc::NodeID(3), std::string(), ::testing::_))
      .WillOnce(Return(std::nullopt));
  EXPECT_CALL(*mock_presenter, triggerStage(::testing::_, ::testing::_))
      .Times(0);

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy data_spy(&coordinator, &RenderCoordinator::catalogueDataReady);
  QSignalSpy error_spy(&coordinator, &RenderCoordinator::error);
  QSignalSpy absent_spy(&coordinator, &RenderCoordinator::resultsNotAvailable);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  const uint64_t request_id = coordinator.requestCatalogueData(orc::NodeID(3));

  ASSERT_TRUE(waitForCount(absent_spy, 1));
  EXPECT_EQ(absent_spy.at(0).at(0).toULongLong(), request_id);
  EXPECT_EQ(data_spy.count(), 0);
  // Nothing to show is not a failure — the reader gets guidance, not an error.
  EXPECT_EQ(error_spy.count(), 0);

  coordinator.stop();
}

// A read that throws is still an error, and stays distinct from one that
// simply found nothing: something went wrong, rather than nothing having been
// produced yet.
TEST(RenderCoordinatorTest, CatalogueRequest_ThrowingReadIsAnError) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  EXPECT_CALL(*mock_presenter,
              getCatalogueData(orc::NodeID(4), std::string(), ::testing::_))
      .WillOnce(::testing::Throw(std::runtime_error("catalogue read blew up")));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy data_spy(&coordinator, &RenderCoordinator::catalogueDataReady);
  QSignalSpy error_spy(&coordinator, &RenderCoordinator::error);
  QSignalSpy absent_spy(&coordinator, &RenderCoordinator::resultsNotAvailable);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  const uint64_t request_id = coordinator.requestCatalogueData(orc::NodeID(4));

  ASSERT_TRUE(waitForCount(error_spy, 1));
  EXPECT_EQ(error_spy.at(0).at(0).toULongLong(), request_id);
  EXPECT_EQ(data_spy.count(), 0);
  EXPECT_EQ(absent_spy.count(), 0);

  coordinator.stop();
}

// Concurrent requests carry distinct ids and each response echoes its own id,
// so a consumer can drop stale (superseded) responses.
TEST(RenderCoordinatorTest, CatalogueRequest_DistinctIdsSupportStaleDrop) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  EXPECT_CALL(*mock_presenter,
              getCatalogueData(::testing::_, ::testing::_, ::testing::_))
      .WillRepeatedly(Return(makeCatalogue("100")));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy data_spy(&coordinator, &RenderCoordinator::catalogueDataReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  const uint64_t first = coordinator.requestCatalogueData(orc::NodeID(2));
  const uint64_t second = coordinator.requestCatalogueData(orc::NodeID(5));
  EXPECT_NE(first, second);

  ASSERT_TRUE(waitForCount(data_spy, 2));
  EXPECT_EQ(data_spy.at(0).at(0).toULongLong(), first);
  EXPECT_EQ(data_spy.at(1).at(0).toULongLong(), second);

  coordinator.stop();
}

// The worker thread is torn down cleanly with a catalogue request outstanding —
// AGENTS.md §4.5's shutdown requirement for every request type.
TEST(RenderCoordinatorTest, CatalogueRequest_StopsCleanlyWhileInFlight) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  EXPECT_CALL(*mock_presenter,
              getCatalogueData(::testing::_, ::testing::_, ::testing::_))
      .WillRepeatedly(Return(makeCatalogue("100")));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));
  for (int i = 0; i < 8; ++i) {
    coordinator.requestCatalogueData(orc::NodeID(2));
  }
  coordinator.stop();
}

// --- Phase 5 Task 5.4: background-workload progress ----------------------

// A scheduler workload snapshot is forwarded to the observationProgress signal.
TEST(RenderCoordinatorTest, ObservationProgress_ForwardedToSignal) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy progress_spy(&coordinator,
                          &RenderCoordinator::observationProgress);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  ASSERT_TRUE(waitForPredicate(
      [&] { return mock_presenter->progressSubscriberCount() == 1; }));

  orc::presenters::ObservationProgressEvent event;
  event.active = true;
  event.percent_complete = 42;
  event.computing = true;
  event.outstanding_nodes = 3;
  mock_presenter->fireProgress(event);

  ASSERT_TRUE(waitForCount(progress_spy, 1));
  EXPECT_TRUE(progress_spy.at(0).at(0).toBool());
  EXPECT_EQ(progress_spy.at(0).at(1).toInt(), 42);
  EXPECT_TRUE(progress_spy.at(0).at(2).toBool());
  EXPECT_EQ(progress_spy.at(0).at(3).toULongLong(), 3ull);

  // Idle snapshot delivered when the queue drains.
  orc::presenters::ObservationProgressEvent idle;
  mock_presenter->fireProgress(idle);
  ASSERT_TRUE(waitForCount(progress_spy, 2));
  EXPECT_FALSE(progress_spy.at(1).at(0).toBool());

  coordinator.stop();
}

// Progress delivery stops once the coordinator tears the presenter down.
TEST(RenderCoordinatorTest, ObservationProgress_UnsubscribeStopsDelivery) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy progress_spy(&coordinator,
                          &RenderCoordinator::observationProgress);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));
  ASSERT_TRUE(waitForPredicate(
      [&] { return mock_presenter->progressSubscriberCount() == 1; }));

  coordinator.updateDAG(nullptr);  // tears down the presenter, unsubscribes
  ASSERT_TRUE(waitForPredicate(
      [&] { return mock_presenter->progressSubscriberCount() == 0; }));

  orc::presenters::ObservationProgressEvent event;
  event.active = true;
  mock_presenter->fireProgress(event);
  QCoreApplication::processEvents();
  EXPECT_EQ(progress_spy.count(), 0);

  coordinator.stop();
}

// --- On-demand execution progress (project-load feedback) ----------------

// The coordinator installs an execution observer on the presenter it creates,
// and each node event reaches the GUI thread as an executionProgress signal.
TEST(RenderCoordinatorTest, ExecutionProgress_ForwardedToSignal) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy execution_spy(&coordinator, &RenderCoordinator::executionProgress);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  // The observer is installed when the presenter is created on the worker.
  ASSERT_TRUE(waitForPredicate(
      [&] { return mock_presenter->hasExecutionProgressCallback(); }));

  mock_presenter->fireExecutionProgress(/*node_id=*/4, /*current=*/1,
                                        /*total=*/3);

  ASSERT_TRUE(waitForCount(execution_spy, 1));
  EXPECT_EQ(execution_spy.at(0).at(0).toInt(), 4);
  EXPECT_EQ(execution_spy.at(0).at(1).toULongLong(), 1ull);
  EXPECT_EQ(execution_spy.at(0).at(2).toULongLong(), 3ull);

  // Every node of the execution order is reported, not just the first.
  mock_presenter->fireExecutionProgress(/*node_id=*/7, /*current=*/2,
                                        /*total=*/3);
  ASSERT_TRUE(waitForCount(execution_spy, 2));
  EXPECT_EQ(execution_spy.at(1).at(0).toInt(), 7);
  EXPECT_EQ(execution_spy.at(1).at(1).toULongLong(), 2ull);

  coordinator.stop();
}

// --- Preview audio playback: pair enumeration and reader creation ---------

namespace {

// Minimal IAudioStreamReader stand-in: the coordinator only moves the pointer
// across threads, so no behaviour is needed beyond identity.
class StubAudioStreamReader final : public orc::presenters::IAudioStreamReader {
 public:
  void prime(const orc::presenters::AudioPrimeProgressCallback&) override {}
  std::vector<float> readFrames(orc::FrameID, uint64_t) override { return {}; }
  uint64_t frameForPairPosition(uint64_t) const override { return 0; }
  uint64_t pairPositionForFrame(orc::FrameID) const override { return 0; }
  orc::FrameIDRange frameRange() const override { return {0, 0}; }
};

orc::AudioPairView makePairView(size_t index, std::string name,
                                std::string origin) {
  orc::AudioPairView view;
  view.index = index;
  view.name = std::move(name);
  view.origin = std::move(origin);
  return view;
}

}  // namespace

TEST(RenderCoordinatorTest, AudioChannelPairsRequest_RoundTripsThroughWorker) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  EXPECT_CALL(*mock_presenter, getAudioChannelPairs(orc::NodeID(3)))
      .WillOnce(Return(std::vector<orc::AudioPairView>{
          makePairView(0, "Analogue", "analogue"),
          makePairView(1, "EFM digital audio", "efm")}));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy pairs_spy(&coordinator,
                       &RenderCoordinator::audioChannelPairsReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  const uint64_t request_id =
      coordinator.requestAudioChannelPairs(orc::NodeID(3));

  ASSERT_TRUE(waitForCount(pairs_spy, 1));
  EXPECT_EQ(pairs_spy.at(0).at(0).toULongLong(), request_id);
  const auto pairs =
      pairs_spy.at(0).at(1).value<std::vector<orc::AudioPairView>>();
  ASSERT_EQ(pairs.size(), 2u);
  EXPECT_EQ(pairs[1].index, 1u);
  EXPECT_EQ(pairs[1].name, "EFM digital audio");
  EXPECT_EQ(pairs[1].origin, "efm");

  coordinator.stop();
}

// An audio-less node answers with an empty list, not an error: the dialogue's
// combo simply stays disabled.
TEST(RenderCoordinatorTest,
     AudioChannelPairsRequest_EmptyListForNodeWithNoAudio) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  EXPECT_CALL(*mock_presenter, getAudioChannelPairs(orc::NodeID(5)))
      .WillOnce(Return(std::vector<orc::AudioPairView>{}));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy pairs_spy(&coordinator,
                       &RenderCoordinator::audioChannelPairsReady);
  QSignalSpy error_spy(&coordinator, &RenderCoordinator::error);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  coordinator.requestAudioChannelPairs(orc::NodeID(5));

  ASSERT_TRUE(waitForCount(pairs_spy, 1));
  EXPECT_TRUE(
      pairs_spy.at(0).at(1).value<std::vector<orc::AudioPairView>>().empty());
  EXPECT_EQ(error_spy.count(), 0);

  coordinator.stop();
}

// The viewed node can change while a heavy DAG is still enumerating, so only
// the newest enumeration is delivered.
TEST(RenderCoordinatorTest, StaleAudioChannelPairsResponses_AreSuppressed) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  EXPECT_CALL(*mock_presenter, getAudioChannelPairs(testing::_))
      .WillOnce(Invoke([](orc::NodeID) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return std::vector<orc::AudioPairView>{
            makePairView(0, "First", "analogue")};
      }))
      .WillOnce(Invoke([](orc::NodeID) {
        return std::vector<orc::AudioPairView>{
            makePairView(0, "Second", "efm")};
      }));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy pairs_spy(&coordinator,
                       &RenderCoordinator::audioChannelPairsReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  const uint64_t first_id =
      coordinator.requestAudioChannelPairs(orc::NodeID(1));
  const uint64_t second_id =
      coordinator.requestAudioChannelPairs(orc::NodeID(2));

  ASSERT_TRUE(waitForCount(pairs_spy, 1));
  EXPECT_EQ(pairs_spy.count(), 1);
  EXPECT_EQ(pairs_spy.at(0).at(0).toULongLong(), second_id);
  EXPECT_NE(first_id, second_id);
  EXPECT_EQ(
      pairs_spy.at(0).at(1).value<std::vector<orc::AudioPairView>>().at(0).name,
      "Second");

  coordinator.stop();
}

TEST(RenderCoordinatorTest, AudioStreamReaderRequest_DeliversReaderFromWorker) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  auto reader = std::make_shared<StubAudioStreamReader>();

  EXPECT_CALL(*mock_presenter, createAudioStreamReader(orc::NodeID(3), 1u))
      .WillOnce(Return(reader));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy reader_spy(&coordinator,
                        &RenderCoordinator::audioStreamReaderReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  const uint64_t request_id =
      coordinator.requestAudioStreamReader(orc::NodeID(3), 1);

  ASSERT_TRUE(waitForCount(reader_spy, 1));
  EXPECT_EQ(reader_spy.at(0).at(0).toULongLong(), request_id);
  EXPECT_EQ(reader_spy.at(0)
                .at(1)
                .value<std::shared_ptr<orc::presenters::IAudioStreamReader>>()
                .get(),
            reader.get());

  coordinator.stop();
}

// An unusable pair is reported as a null reader rather than an error, so the
// dialogue can fall back to the video-only playback path.
TEST(RenderCoordinatorTest,
     AudioStreamReaderRequest_NullReaderForUnusablePair) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  EXPECT_CALL(*mock_presenter, createAudioStreamReader(orc::NodeID(3), 0u))
      .WillOnce(Return(nullptr));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy reader_spy(&coordinator,
                        &RenderCoordinator::audioStreamReaderReady);
  QSignalSpy error_spy(&coordinator, &RenderCoordinator::error);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  coordinator.requestAudioStreamReader(orc::NodeID(3), 0);

  ASSERT_TRUE(waitForCount(reader_spy, 1));
  EXPECT_EQ(reader_spy.at(0)
                .at(1)
                .value<std::shared_ptr<orc::presenters::IAudioStreamReader>>(),
            nullptr);
  EXPECT_EQ(error_spy.count(), 0);

  coordinator.stop();
}

// Reselecting the pair while the first creation is still resolving must not
// deliver the superseded reader.
TEST(RenderCoordinatorTest, StaleAudioStreamReaderResponses_AreSuppressed) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  auto first_reader = std::make_shared<StubAudioStreamReader>();
  auto second_reader = std::make_shared<StubAudioStreamReader>();

  EXPECT_CALL(*mock_presenter, createAudioStreamReader(orc::NodeID(3), 0u))
      .WillOnce(Invoke([first_reader](orc::NodeID, size_t) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return std::static_pointer_cast<orc::presenters::IAudioStreamReader>(
            first_reader);
      }));
  EXPECT_CALL(*mock_presenter, createAudioStreamReader(orc::NodeID(3), 1u))
      .WillOnce(Return(second_reader));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy reader_spy(&coordinator,
                        &RenderCoordinator::audioStreamReaderReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  const uint64_t first_id =
      coordinator.requestAudioStreamReader(orc::NodeID(3), 0);
  const uint64_t second_id =
      coordinator.requestAudioStreamReader(orc::NodeID(3), 1);

  ASSERT_TRUE(waitForCount(reader_spy, 1));
  EXPECT_EQ(reader_spy.count(), 1);
  EXPECT_EQ(reader_spy.at(0).at(0).toULongLong(), second_id);
  EXPECT_NE(first_id, second_id);
  EXPECT_EQ(reader_spy.at(0)
                .at(1)
                .value<std::shared_ptr<orc::presenters::IAudioStreamReader>>()
                .get(),
            second_reader.get());

  coordinator.stop();
}

// Requests still queued at shutdown are simply dropped: stop() must return and
// no reader may be delivered afterwards.
TEST(RenderCoordinatorTest, Shutdown_WithPendingAudioReaderRequest_IsClean) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  auto reader = std::make_shared<StubAudioStreamReader>();

  // A slow first creation keeps the worker busy while stop() is called.
  ON_CALL(*mock_presenter, createAudioStreamReader(testing::_, testing::_))
      .WillByDefault(Invoke([reader](orc::NodeID, size_t) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        return std::static_pointer_cast<orc::presenters::IAudioStreamReader>(
            reader);
      }));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy reader_spy(&coordinator,
                        &RenderCoordinator::audioStreamReaderReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  coordinator.requestAudioStreamReader(orc::NodeID(3), 0);
  coordinator.requestAudioStreamReader(orc::NodeID(3), 1);
  coordinator.stop();

  // Deliveries are queued signals; none may arrive for a stopped coordinator.
  QCoreApplication::processEvents();
  EXPECT_LE(reader_spy.count(), 1);
}

// ---------------------------------------------------------------------------
// Scope payloads travel with the render
//
// Both scope dialogues plot the frame the preview is showing. Asking for their
// payloads after the render, from the GUI thread, decoded the same frame again
// once per open dialogue while the GUI thread waited. The render now produces
// them on the worker from the carrier it already holds.
// ---------------------------------------------------------------------------

TEST(RenderCoordinatorTest, PreviewDelivery_CarriesScopePayloadsFromOneCall) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  ON_CALL(*mock_presenter, renderPreview(testing::_, testing::_, testing::_,
                                         testing::_, testing::_))
      .WillByDefault(
          Invoke([](orc::NodeID node_id, orc::PreviewOutputType output_type,
                    uint64_t output_index, const std::string&,
                    orc::PreviewNavigationHint) {
            return makeRenderResult(node_id, output_type, output_index);
          }));

  orc::PreviewScopeRequest seen_request;
  int scope_calls = 0;
  EXPECT_CALL(*mock_presenter, getPreviewScopes(testing::_, testing::_))
      .WillRepeatedly(
          Invoke([&](orc::NodeID, const orc::PreviewScopeRequest& request) {
            ++scope_calls;
            seen_request = request;
            orc::PreviewScopePayloads payloads;
            payloads.vectorscope = orc::VectorscopeData{};
            payloads.histogram = orc::VideoHistogramData{};
            return payloads;
          }));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy preview_spy(&coordinator, &RenderCoordinator::previewReady);
  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  orc::PreviewScopeRequest scopes;
  scopes.want_vectorscope = true;
  scopes.want_histogram = true;
  scopes.vectorscope_view_id = "vectorscope";
  scopes.data_type = orc::VideoDataType::ColourPAL;
  scopes.coordinate.field_index = 11;
  scopes.coordinate.vectorscope_active_area_only = false;
  scopes.coordinate.vectorscope_first_line = 20;
  scopes.coordinate.vectorscope_last_line = 60;

  coordinator.requestPreview(orc::NodeID(4),
                             orc::PreviewOutputType::Frame_Field1_First, 11, "",
                             orc::PreviewNavigationHint::Sequential, scopes);

  ASSERT_TRUE(waitForCount(preview_spy, 1));

  // One call for both scopes: that is the whole point, since each call is a
  // carrier fetch and therefore a chroma decode.
  EXPECT_EQ(scope_calls, 1);
  EXPECT_EQ(seen_request.coordinate.field_index, 11u);
  EXPECT_EQ(seen_request, scopes) << "the selection must reach the worker "
                                     "unchanged, or the plot is of the wrong "
                                     "lines";

  const auto delivery =
      preview_spy.at(0).at(1).value<PreviewRenderDeliveryPtr>();
  ASSERT_TRUE(delivery);
  EXPECT_TRUE(delivery->result.success);
  EXPECT_TRUE(delivery->scopes.vectorscope.has_value());
  EXPECT_TRUE(delivery->scopes.histogram.has_value());

  coordinator.stop();
}

TEST(RenderCoordinatorTest, PreviewDelivery_NoScopesRequestedFetchesNoCarrier) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  ON_CALL(*mock_presenter, renderPreview(testing::_, testing::_, testing::_,
                                         testing::_, testing::_))
      .WillByDefault(
          Invoke([](orc::NodeID node_id, orc::PreviewOutputType output_type,
                    uint64_t output_index, const std::string&,
                    orc::PreviewNavigationHint) {
            return makeRenderResult(node_id, output_type, output_index);
          }));

  // With every scope dialogue closed the render must not touch a carrier at
  // all; this is the common case and it has to stay free.
  EXPECT_CALL(*mock_presenter, getPreviewScopes(testing::_, testing::_))
      .Times(0);

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy preview_spy(&coordinator, &RenderCoordinator::previewReady);
  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  coordinator.requestPreview(orc::NodeID(4),
                             orc::PreviewOutputType::Frame_Field1_First, 3);

  ASSERT_TRUE(waitForCount(preview_spy, 1));
  const auto delivery =
      preview_spy.at(0).at(1).value<PreviewRenderDeliveryPtr>();
  ASSERT_TRUE(delivery);
  EXPECT_FALSE(delivery->scopes.vectorscope.has_value());
  EXPECT_FALSE(delivery->scopes.histogram.has_value());

  coordinator.stop();
}

TEST(RenderCoordinatorTest, PreviewDelivery_IsSharedNotCopiedPerConnection) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  ON_CALL(*mock_presenter, renderPreview(testing::_, testing::_, testing::_,
                                         testing::_, testing::_))
      .WillByDefault(
          Invoke([](orc::NodeID node_id, orc::PreviewOutputType output_type,
                    uint64_t output_index, const std::string&,
                    orc::PreviewNavigationHint) {
            return makeRenderResult(node_id, output_type, output_index);
          }));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  // Two spies stand in for two connected consumers. The payload behind the
  // pointer must be one object, not one copy each.
  QSignalSpy first_spy(&coordinator, &RenderCoordinator::previewReady);
  QSignalSpy second_spy(&coordinator, &RenderCoordinator::previewReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));
  coordinator.requestPreview(orc::NodeID(4),
                             orc::PreviewOutputType::Frame_Field1_First, 0);

  ASSERT_TRUE(waitForCount(first_spy, 1));
  ASSERT_TRUE(waitForCount(second_spy, 1));

  const auto from_first =
      first_spy.at(0).at(1).value<PreviewRenderDeliveryPtr>();
  const auto from_second =
      second_spy.at(0).at(1).value<PreviewRenderDeliveryPtr>();
  ASSERT_TRUE(from_first);
  EXPECT_EQ(from_first.get(), from_second.get())
      << "each consumer received its own copy of the render payload";

  coordinator.stop();
}

// ---------------------------------------------------------------------------
// One extraction serves both sample dialogues
//
// The frame timing and waveform monitor dialogues plot the same extraction.
// A request each walked the frame twice per displayed frame and queued twice
// in front of the next render.
// ---------------------------------------------------------------------------

namespace {

orc::presenters::IRenderPresenter::LineSampleData makeYCSamples() {
  orc::presenters::IRenderPresenter::LineSampleData data;
  data.has_separate_channels = true;
  data.y_samples = {1, 2, 3, 4};
  data.c_samples = {5, 6, 7, 8};
  data.first_field_height = 2;
  data.second_field_height = 2;
  return data;
}

}  // namespace

TEST(RenderCoordinatorTest, FrameSamples_OneExtractionServesBothDialogues) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  int extractions = 0;
  EXPECT_CALL(*mock_presenter,
              getFieldSamplesForTiming(testing::_, testing::_, testing::_))
      .WillRepeatedly(
          Invoke([&](orc::NodeID, orc::PreviewOutputType, uint64_t) {
            ++extractions;
            return makeYCSamples();
          }));
  ON_CALL(*mock_presenter, getVideoParameters(testing::_))
      .WillByDefault(Return(std::nullopt));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy samples_spy(&coordinator, &RenderCoordinator::frameSamplesReady);
  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  coordinator.requestFrameSamples(orc::NodeID(2),
                                  orc::PreviewOutputType::Frame_Field1_First, 6,
                                  /*for_frame_timing=*/true,
                                  /*for_waveform_monitor=*/true);

  ASSERT_TRUE(waitForCount(samples_spy, 1));
  EXPECT_EQ(extractions, 1) << "both dialogues must share one extraction";

  const auto delivery =
      samples_spy.at(0).at(1).value<FrameSamplesDeliveryPtr>();
  ASSERT_TRUE(delivery);
  EXPECT_TRUE(delivery->for_frame_timing);
  EXPECT_TRUE(delivery->for_waveform_monitor);

  coordinator.stop();
}

TEST(RenderCoordinatorTest, FrameSamples_FrameModeReportsBothFieldIndices) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  ON_CALL(*mock_presenter,
          getFieldSamplesForTiming(testing::_, testing::_, testing::_))
      .WillByDefault(Return(makeYCSamples()));
  ON_CALL(*mock_presenter, getVideoParameters(testing::_))
      .WillByDefault(Return(std::nullopt));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy samples_spy(&coordinator, &RenderCoordinator::frameSamplesReady);
  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  // A frame index addresses the field pair it weaves.
  coordinator.requestFrameSamples(orc::NodeID(2),
                                  orc::PreviewOutputType::Frame_Field1_First, 6,
                                  true, false);
  ASSERT_TRUE(waitForCount(samples_spy, 1));
  auto delivery = samples_spy.at(0).at(1).value<FrameSamplesDeliveryPtr>();
  ASSERT_TRUE(delivery);
  EXPECT_EQ(delivery->field_index, 12u);
  ASSERT_TRUE(delivery->field_index_2.has_value());
  EXPECT_EQ(*delivery->field_index_2, 13u);

  // A flat field mode already counts fields, so the index passes through.
  coordinator.requestFrameSamples(
      orc::NodeID(2), orc::PreviewOutputType::Frame_Field1, 6, true, false);
  ASSERT_TRUE(waitForCount(samples_spy, 2));
  delivery = samples_spy.at(1).at(1).value<FrameSamplesDeliveryPtr>();
  ASSERT_TRUE(delivery);
  EXPECT_EQ(delivery->field_index, 6u);
  EXPECT_FALSE(delivery->field_index_2.has_value());

  coordinator.stop();
}

TEST(RenderCoordinatorTest, FrameSamples_VideoParametersTravelWithTheSamples) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  ON_CALL(*mock_presenter,
          getFieldSamplesForTiming(testing::_, testing::_, testing::_))
      .WillByDefault(Return(makeYCSamples()));

  orc::SourceParameters params;
  params.frame_width_nominal = 1135;
  EXPECT_CALL(*mock_presenter, getVideoParameters(orc::NodeID(2)))
      .WillRepeatedly(Return(std::optional<orc::SourceParameters>(params)));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy samples_spy(&coordinator, &RenderCoordinator::frameSamplesReady);
  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  coordinator.requestFrameSamples(
      orc::NodeID(2), orc::PreviewOutputType::Frame_Field1, 0, true, true);

  ASSERT_TRUE(waitForCount(samples_spy, 1));
  const auto delivery =
      samples_spy.at(0).at(1).value<FrameSamplesDeliveryPtr>();
  ASSERT_TRUE(delivery);
  // Without this the dialogues built a throwaway presenter and fingerprinted
  // the DAG on the GUI thread, once each, per displayed frame.
  ASSERT_TRUE(delivery->video_params.has_value());
  EXPECT_EQ(delivery->video_params->frame_width_nominal, 1135);

  coordinator.stop();
}

TEST(LineSampleData, CompositeSubstitutesLumaForAYCSourceWithoutCopying) {
  const auto data = makeYCSamples();
  // The copy this replaces was a whole frame of samples per extraction.
  EXPECT_TRUE(data.composite_samples.empty());
  EXPECT_EQ(&data.composite(), &data.y_samples);
  EXPECT_FALSE(data.empty());
}

TEST(LineSampleData, CompositeIsItsOwnBufferForACompositeSource) {
  orc::presenters::IRenderPresenter::LineSampleData data;
  data.has_separate_channels = false;
  data.composite_samples = {9, 9};
  EXPECT_EQ(&data.composite(), &data.composite_samples);
  EXPECT_FALSE(data.empty());
}

TEST(LineSampleData, EmptyWhenNothingWasExtracted) {
  orc::presenters::IRenderPresenter::LineSampleData data;
  data.has_separate_channels = true;
  EXPECT_TRUE(data.empty());
  EXPECT_TRUE(data.composite().empty());
}

TEST(RenderCoordinatorTest, LineSamples_DeliverViewParametersAndShareBuffers) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  orc::presenters::IRenderPresenter::LineSampleData line;
  line.has_separate_channels = false;
  line.composite_samples = {3, 1, 4, 1, 5};
  ON_CALL(*mock_presenter,
          getLineSamplesWithYC(testing::_, testing::_, testing::_, testing::_,
                               testing::_, testing::_))
      .WillByDefault(Return(line));

  orc::SourceParameters params;
  params.frame_width_nominal = 910;
  ON_CALL(*mock_presenter, getVideoParameters(testing::_))
      .WillByDefault(Return(std::optional<orc::SourceParameters>(params)));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy first_spy(&coordinator, &RenderCoordinator::lineSamplesReady);
  QSignalSpy second_spy(&coordinator, &RenderCoordinator::lineSamplesReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));
  coordinator.requestLineSamples(
      orc::NodeID(5), orc::PreviewOutputType::Frame_Field1, 2, 7, 3, 720);

  ASSERT_TRUE(waitForCount(first_spy, 1));
  ASSERT_TRUE(waitForCount(second_spy, 1));

  const auto from_first = first_spy.at(0).at(1).value<LineSamplesDeliveryPtr>();
  const auto from_second =
      second_spy.at(0).at(1).value<LineSamplesDeliveryPtr>();
  ASSERT_TRUE(from_first);
  EXPECT_EQ(from_first.get(), from_second.get())
      << "each consumer received its own copy of the sample buffers";

  EXPECT_EQ(from_first->field_index, 2u);
  EXPECT_EQ(from_first->line_number, 7);
  EXPECT_EQ(from_first->sample_x, 3);
  // Converted on the worker, so the GUI thread does no parameter work.
  ASSERT_TRUE(from_first->video_params.has_value());
  EXPECT_EQ(from_first->video_params->frame_width_nominal, 910);

  coordinator.stop();
}

}  // namespace gui_unit_test
