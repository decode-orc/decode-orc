/*
 * File:        render_coordinator_comparison_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     RenderCoordinator A/B comparison and export requests:
 *              delivery, stale-response suppression and shutdown
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QMetaType>
#include <QSignalSpy>
#include <QThread>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "mocks/mock_render_presenter.h"
#include "render_coordinator.h"

Q_DECLARE_METATYPE(ComparisonDeliveryPtr)
Q_DECLARE_METATYPE(orc::PreviewCompareExportResult)

namespace gui_unit_test {
namespace {

using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;

using MockPresenter = NiceMock<orc::presenters::test::MockRenderPresenter>;

const bool kMetatypesRegistered = [] {
  qRegisterMetaType<ComparisonDeliveryPtr>("ComparisonDeliveryPtr");
  qRegisterMetaType<orc::PreviewCompareExportResult>(
      "orc::PreviewCompareExportResult");
  return true;
}();

bool waitForCount(QSignalSpy& spy, int expected, int timeout_ms = 2000) {
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

// Holds the first call on the worker until the test releases it.
struct Blocker {
  std::atomic<bool> started{false};
  std::atomic<bool> release{false};
  std::atomic<int> calls{0};

  void waitIfFirst() {
    if (calls.fetch_add(1) != 0) {
      return;
    }
    started.store(true);
    while (!release.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
};

orc::PreviewImage grey(uint32_t width, uint32_t height, uint8_t level) {
  orc::PreviewImage image;
  image.width = width;
  image.height = height;
  image.rgb_data.assign(static_cast<size_t>(width) * height * 3, level);
  return image;
}

orc::PreviewCompareRequest compareRequest(uint64_t index) {
  orc::PreviewCompareRequest request;
  request.node_a = orc::NodeID(1);
  request.node_b = orc::NodeID(2);
  request.output_type = orc::PreviewOutputType::Frame_Field1_First;
  request.option_id = "interlaced_clamped";
  request.index = index;
  return request;
}

orc::PreviewCompareResult pairFor(const orc::PreviewCompareRequest& request) {
  orc::PreviewCompareResult result;
  result.success = true;
  result.request = request;
  result.image_a = grey(8, 4, 10);
  result.image_b = grey(8, 4, 20);
  result.count_a = 5;
  result.count_b = 5;
  return result;
}

orc::PreviewCompareViewImages normalView(
    const orc::PreviewCompareResult& result,
    const orc::PreviewCompareViewSettings&) {
  orc::PreviewCompareViewImages view;
  view.success = true;
  view.image_a = result.image_a;
  view.image_b = result.image_b;
  return view;
}

class CoordinatorFixture {
 public:
  explicit CoordinatorFixture(std::shared_ptr<MockPresenter> presenter)
      : presenter_(std::move(presenter)),
        coordinator_([p = presenter_](void*)
                         -> std::shared_ptr<orc::presenters::IRenderPresenter> {
          return p;
        }) {
    coordinator_.start();
    coordinator_.setProject(reinterpret_cast<void*>(0x1));
    coordinator_.updateDAG(std::make_shared<int>(1));
  }

  RenderCoordinator& coordinator() { return coordinator_; }

 private:
  std::shared_ptr<MockPresenter> presenter_;
  RenderCoordinator coordinator_;
};

}  // namespace

TEST(RenderCoordinatorComparisonTest, DeliversTheDerivedViewAsImages) {
  (void)kMetatypesRegistered;
  auto presenter = std::make_shared<MockPresenter>();
  EXPECT_CALL(*presenter, renderComparison(compareRequest(3)))
      .WillOnce(Invoke(pairFor));
  orc::PreviewCompareViewSettings seen;
  EXPECT_CALL(*presenter, deriveComparisonView(_, _))
      .WillOnce(
          Invoke([&seen](const orc::PreviewCompareResult& result,
                         const orc::PreviewCompareViewSettings& settings) {
            seen = settings;
            return normalView(result, settings);
          }));

  CoordinatorFixture fixture(presenter);
  QSignalSpy spy(&fixture.coordinator(), &RenderCoordinator::comparisonReady);

  orc::PreviewCompareViewSettings view;
  view.mode = orc::PreviewCompareViewMode::Stretched;
  view.stretch_gain = 6.0;
  const uint64_t id =
      fixture.coordinator().requestComparison(compareRequest(3), view);

  ASSERT_TRUE(waitForCount(spy, 1));
  EXPECT_EQ(spy.at(0).at(0).toULongLong(), id);
  const auto delivery = spy.at(0).at(1).value<ComparisonDeliveryPtr>();
  ASSERT_TRUE(delivery);
  EXPECT_TRUE(delivery->ok());
  EXPECT_EQ(delivery->native_width, 8u);
  EXPECT_EQ(delivery->native_height, 4u);
  EXPECT_EQ(delivery->image_a.size(), QSize(8, 4));
  EXPECT_EQ(delivery->image_b.size(), QSize(8, 4));
  EXPECT_EQ(qRed(delivery->image_b.pixel(0, 0)), 20);
  EXPECT_TRUE(delivery->difference.isNull());
  EXPECT_EQ(delivery->result.count_a, 5u);
  // The raw pair is not sent twice.
  EXPECT_TRUE(delivery->result.image_a.rgb_data.empty());
  EXPECT_EQ(seen.mode, orc::PreviewCompareViewMode::Stretched);
  EXPECT_DOUBLE_EQ(seen.stretch_gain, 6.0);
  EXPECT_EQ(delivery->view_settings.mode,
            orc::PreviewCompareViewMode::Stretched);

  fixture.coordinator().stop();
}

TEST(RenderCoordinatorComparisonTest,
     AFailedComparisonIsDeliveredWithItsError) {
  (void)kMetatypesRegistered;
  auto presenter = std::make_shared<MockPresenter>();
  EXPECT_CALL(*presenter, renderComparison(_))
      .WillOnce(Invoke([](const orc::PreviewCompareRequest& request) {
        orc::PreviewCompareResult result;
        result.request = request;
        result.error =
            "frame 9 is beyond node B (2), which has 5 (frames 1 to 5)";
        return result;
      }));
  EXPECT_CALL(*presenter, deriveComparisonView(_, _)).Times(0);

  CoordinatorFixture fixture(presenter);
  QSignalSpy spy(&fixture.coordinator(), &RenderCoordinator::comparisonReady);
  QSignalSpy error_spy(&fixture.coordinator(), &RenderCoordinator::error);

  fixture.coordinator().requestComparison(compareRequest(8), {});

  ASSERT_TRUE(waitForCount(spy, 1));
  const auto delivery = spy.at(0).at(1).value<ComparisonDeliveryPtr>();
  ASSERT_TRUE(delivery);
  EXPECT_FALSE(delivery->ok());
  EXPECT_EQ(delivery->result.error,
            "frame 9 is beyond node B (2), which has 5 (frames 1 to 5)");
  EXPECT_TRUE(delivery->image_a.isNull());
  // Shown in the window, not raised as a coordinator error.
  EXPECT_EQ(error_spy.count(), 0);

  fixture.coordinator().stop();
}

TEST(RenderCoordinatorComparisonTest,
     SupersededQueuedComparisonsAreDroppedAndOnlyTheNewestDelivered) {
  (void)kMetatypesRegistered;
  auto presenter = std::make_shared<MockPresenter>();
  auto blocker = std::make_shared<Blocker>();
  std::mutex mutex;
  std::vector<uint64_t> rendered;
  ON_CALL(*presenter, renderComparison(_))
      .WillByDefault(
          Invoke([&, blocker](const orc::PreviewCompareRequest& request) {
            {
              const std::lock_guard<std::mutex> lock(mutex);
              rendered.push_back(request.index);
            }
            blocker->waitIfFirst();
            return pairFor(request);
          }));
  ON_CALL(*presenter, deriveComparisonView(_, _))
      .WillByDefault(Invoke(normalView));

  CoordinatorFixture fixture(presenter);
  QSignalSpy spy(&fixture.coordinator(), &RenderCoordinator::comparisonReady);

  // Index 0 holds the worker; 1 to 5 queue behind it.
  fixture.coordinator().requestComparison(compareRequest(0), {});
  ASSERT_TRUE(waitForFlag([&] { return blocker->started.load(); }));
  uint64_t last = 0;
  for (uint64_t index = 1; index <= 5; ++index) {
    last = fixture.coordinator().requestComparison(compareRequest(index), {});
  }
  blocker->release.store(true);

  ASSERT_TRUE(waitForCount(spy, 1));
  QThread::msleep(50);
  QCoreApplication::processEvents();

  // The one in flight finished but was not delivered; the queued ones were
  // never rendered.
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.at(0).at(0).toULongLong(), last);
  const std::lock_guard<std::mutex> lock(mutex);
  EXPECT_EQ(rendered, (std::vector<uint64_t>{0, 5}));

  fixture.coordinator().stop();
}

TEST(RenderCoordinatorComparisonTest, QueuedExportsAreNeverDropped) {
  (void)kMetatypesRegistered;
  auto presenter = std::make_shared<MockPresenter>();
  auto blocker = std::make_shared<Blocker>();
  ON_CALL(*presenter, renderComparison(_))
      .WillByDefault(Invoke([blocker](const orc::PreviewCompareRequest& r) {
        blocker->waitIfFirst();
        return pairFor(r);
      }));
  ON_CALL(*presenter, deriveComparisonView(_, _))
      .WillByDefault(Invoke(normalView));
  std::mutex mutex;
  std::vector<std::string> paths;
  orc::PreviewCompareExportSettings seen;
  ON_CALL(*presenter, exportComparison(_, _, _))
      .WillByDefault(Invoke([&](const orc::PreviewCompareRequest&,
                                const orc::PreviewCompareExportSettings& s,
                                const std::string& path) {
        const std::lock_guard<std::mutex> lock(mutex);
        paths.push_back(path);
        seen = s;
        orc::PreviewCompareExportResult result;
        result.success = true;
        result.crop_width = s.crop_width;
        return result;
      }));

  CoordinatorFixture fixture(presenter);
  QSignalSpy exported(&fixture.coordinator(),
                      &RenderCoordinator::comparisonExported);

  fixture.coordinator().requestComparison(compareRequest(0), {});
  ASSERT_TRUE(waitForFlag([&] { return blocker->started.load(); }));
  orc::PreviewCompareExportSettings settings;
  settings.crop_mode = orc::PreviewCompareCropMode::Explicit;
  settings.crop_width = 64;
  const uint64_t first = fixture.coordinator().requestComparisonExport(
      compareRequest(0), settings, "one.webp");
  // A comparison queued after an export drops only other comparisons.
  fixture.coordinator().requestComparison(compareRequest(1), {});
  const uint64_t second = fixture.coordinator().requestComparisonExport(
      compareRequest(0), settings, "two.webp");
  blocker->release.store(true);

  ASSERT_TRUE(waitForCount(exported, 2));
  EXPECT_EQ(exported.at(0).at(0).toULongLong(), first);
  EXPECT_EQ(exported.at(1).at(0).toULongLong(), second);
  const auto result =
      exported.at(0).at(1).value<orc::PreviewCompareExportResult>();
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.crop_width, 64u);
  const std::lock_guard<std::mutex> lock(mutex);
  EXPECT_EQ(paths, (std::vector<std::string>{"one.webp", "two.webp"}));
  EXPECT_EQ(seen.crop_mode, orc::PreviewCompareCropMode::Explicit);

  fixture.coordinator().stop();
}

TEST(RenderCoordinatorComparisonTest, WithoutAProjectTheRequestIsAnswered) {
  (void)kMetatypesRegistered;
  RenderCoordinator coordinator(
      [](void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return nullptr;
      });
  QSignalSpy ready(&coordinator, &RenderCoordinator::comparisonReady);
  QSignalSpy exported(&coordinator, &RenderCoordinator::comparisonExported);
  coordinator.start();

  coordinator.requestComparison(compareRequest(0), {});
  coordinator.requestComparisonExport(compareRequest(0), {}, "x.webp");

  ASSERT_TRUE(waitForCount(ready, 1));
  ASSERT_TRUE(waitForCount(exported, 1));
  EXPECT_FALSE(ready.at(0).at(1).value<ComparisonDeliveryPtr>()->ok());
  EXPECT_FALSE(
      exported.at(0).at(1).value<orc::PreviewCompareExportResult>().success);
  coordinator.stop();
}

TEST(RenderCoordinatorComparisonTest, StopsCleanlyWithAComparisonInFlight) {
  (void)kMetatypesRegistered;
  auto presenter = std::make_shared<MockPresenter>();
  ON_CALL(*presenter, renderComparison(_))
      .WillByDefault(Invoke([](const orc::PreviewCompareRequest& request) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        return pairFor(request);
      }));
  ON_CALL(*presenter, deriveComparisonView(_, _))
      .WillByDefault(Invoke(normalView));

  auto fixture = std::make_unique<CoordinatorFixture>(presenter);
  QSignalSpy spy(&fixture->coordinator(), &RenderCoordinator::comparisonReady);
  fixture->coordinator().requestComparison(compareRequest(0), {});
  fixture->coordinator().requestComparison(compareRequest(1), {});
  fixture->coordinator().requestComparisonExport(compareRequest(1), {},
                                                 "x.webp");
  fixture->coordinator().stop();

  QCoreApplication::processEvents();
  EXPECT_LE(spy.count(), 1);
  fixture.reset();
  QCoreApplication::processEvents();
}

}  // namespace gui_unit_test
