// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/wm/toplevel_window_event_handler.h"

#include "ash/accelerators/accelerator_controller_impl.h"
#include "ash/app_list/test/app_list_test_helper.h"
#include "ash/app_list/views/app_list_view.h"
#include "ash/app_list/views/search_box_view.h"
#include "ash/display/screen_orientation_controller.h"
#include "ash/display/screen_orientation_controller_test_api.h"
#include "ash/home_screen/home_screen_controller.h"
#include "ash/public/cpp/app_types.h"
#include "ash/public/cpp/ash_features.h"
#include "ash/public/cpp/shell_window_ids.h"
#include "ash/root_window_controller.h"
#include "ash/screen_util.h"
#include "ash/session/session_controller_impl.h"
#include "ash/shell.h"
#include "ash/shell_delegate.h"
#include "ash/system/overview/overview_button_tray.h"
#include "ash/system/status_area_widget.h"
#include "ash/system/status_area_widget_test_helper.h"
#include "ash/test/ash_test_base.h"
#include "ash/test_shell_delegate.h"
#include "ash/window_factory.h"
#include "ash/wm/desks/desks_util.h"
#include "ash/wm/overview/overview_controller.h"
#include "ash/wm/overview/overview_grid.h"
#include "ash/wm/overview/overview_item.h"
#include "ash/wm/resize_shadow.h"
#include "ash/wm/resize_shadow_controller.h"
#include "ash/wm/splitview/split_view_divider.h"
#include "ash/wm/tablet_mode/tablet_mode_controller_test_api.h"
#include "ash/wm/tablet_mode/tablet_mode_window_manager.h"
#include "ash/wm/window_state.h"
#include "ash/wm/window_util.h"
#include "ash/wm/wm_event.h"
#include "ash/wm/workspace_controller.h"
#include "base/bind.h"
#include "base/compiler_specific.h"
#include "base/run_loop.h"
#include "base/test/scoped_feature_list.h"
#include "base/threading/thread_task_runner_handle.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/aura/client/aura_constants.h"
#include "ui/aura/client/capture_client.h"
#include "ui/aura/test/aura_test_base.h"
#include "ui/aura/test/test_window_delegate.h"
#include "ui/aura/window_event_dispatcher.h"
#include "ui/aura/window_observer.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/accelerators/test_accelerator_target.h"
#include "ui/base/hit_test.h"
#include "ui/display/display_layout_builder.h"
#include "ui/display/manager/display_manager.h"
#include "ui/display/screen.h"
#include "ui/display/test/display_manager_test_api.h"
#include "ui/events/event.h"
#include "ui/events/test/event_generator.h"
#include "ui/wm/core/window_util.h"
#include "ui/wm/public/window_move_client.h"

namespace ash {

namespace {

// A simple window delegate that returns the specified hit-test code when
// requested and applies a minimum size constraint if there is one.
class TestWindowDelegate : public aura::test::TestWindowDelegate {
 public:
  explicit TestWindowDelegate(int hittest_code) {
    set_window_component(hittest_code);
  }
  ~TestWindowDelegate() override = default;

 private:
  // Overridden from aura::Test::TestWindowDelegate:
  void OnWindowDestroyed(aura::Window* window) override { delete this; }

  DISALLOW_COPY_AND_ASSIGN(TestWindowDelegate);
};

class ResizeLoopWindowObserver : public aura::WindowObserver {
 public:
  explicit ResizeLoopWindowObserver(aura::Window* w) : window_(w) {
    window_->AddObserver(this);
  }
  ~ResizeLoopWindowObserver() override {
    if (window_)
      window_->RemoveObserver(this);
  }

  bool in_resize_loop() const { return in_resize_loop_; }

  // aura::WindowObserver:
  void OnResizeLoopStarted(aura::Window* window) override {
    EXPECT_FALSE(in_resize_loop_);
    in_resize_loop_ = true;
  }
  void OnResizeLoopEnded(aura::Window* window) override {
    EXPECT_TRUE(in_resize_loop_);
    in_resize_loop_ = false;
  }
  void OnWindowDestroying(aura::Window* window) override {
    window_->RemoveObserver(this);
    window_ = nullptr;
  }

 private:
  aura::Window* window_;
  bool in_resize_loop_ = false;

  DISALLOW_COPY_AND_ASSIGN(ResizeLoopWindowObserver);
};

class ToplevelWindowEventHandlerTest : public AshTestBase {
 public:
  ToplevelWindowEventHandlerTest() = default;
  ~ToplevelWindowEventHandlerTest() override = default;

 protected:
  aura::Window* CreateWindow(int hittest_code) {
    TestWindowDelegate* d1 = new TestWindowDelegate(hittest_code);
    aura::Window* w1 =
        window_factory::NewWindow(d1, aura::client::WINDOW_TYPE_NORMAL)
            .release();
    w1->set_id(1);
    w1->Init(ui::LAYER_TEXTURED);
    aura::Window* parent = Shell::GetContainer(
        Shell::GetPrimaryRootWindow(), desks_util::GetActiveDeskContainerId());
    parent->AddChild(w1);
    w1->SetBounds(gfx::Rect(0, 0, 100, 100));
    w1->Show();
    return w1;
  }

  void DragFromCenterBy(aura::Window* window, int dx, int dy) {
    ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(), window);
    generator.DragMouseBy(dx, dy);
  }

  void TouchDragFromCenterBy(aura::Window* window, int dx, int dy) {
    ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(), window);
    generator.PressMoveAndReleaseTouchBy(dx, dy);
  }

  std::unique_ptr<ToplevelWindowEventHandler> handler_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ToplevelWindowEventHandlerTest);
};
}  // namespace

TEST_F(ToplevelWindowEventHandlerTest, Caption) {
  std::unique_ptr<aura::Window> w1(CreateWindow(HTCAPTION));
  gfx::Size size = w1->bounds().size();
  DragFromCenterBy(w1.get(), 100, 100);
  // Position should have been offset by 100,100.
  EXPECT_EQ("100,100", w1->bounds().origin().ToString());
  // Size should not have.
  EXPECT_EQ(size.ToString(), w1->bounds().size().ToString());

  TouchDragFromCenterBy(w1.get(), 100, 100);
  // Position should have been offset by 100,100.
  EXPECT_EQ("200,200", w1->bounds().origin().ToString());
  // Size should not have.
  EXPECT_EQ(size.ToString(), w1->bounds().size().ToString());
}

namespace {

void ContinueAndCompleteDrag(ui::test::EventGenerator* generator,
                             WindowState* window_state,
                             aura::Window* window) {
  ASSERT_TRUE(window->HasCapture());
  ASSERT_FALSE(window_state->GetWindowPositionManaged());
  generator->DragMouseBy(100, 100);
  generator->ReleaseLeftButton();
}

}  // namespace

// Tests dragging restores expected window position auto manage property
// correctly.
TEST_F(ToplevelWindowEventHandlerTest, WindowPositionAutoManagement) {
  std::unique_ptr<aura::Window> w1(CreateWindow(HTNOWHERE));
  const gfx::Size size = w1->bounds().size();
  WindowState* window_state = ash::WindowState::Get(w1.get());
  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(), w1.get());

  // Explicitly enable window position auto management, and expect it to be
  // restored after drag completes.
  window_state->SetWindowPositionManaged(true);
  generator.PressLeftButton();
  ::wm::WindowMoveClient* move_client =
      ::wm::GetWindowMoveClient(w1->GetRootWindow());
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&ContinueAndCompleteDrag, base::Unretained(&generator),
                     base::Unretained(window_state),
                     base::Unretained(w1.get())));
  EXPECT_EQ(::wm::MOVE_SUCCESSFUL,
            move_client->RunMoveLoop(w1.get(), gfx::Vector2d(100, 100),
                                     ::wm::WINDOW_MOVE_SOURCE_MOUSE));
  // Window position auto manage property should be restored to true.
  EXPECT_TRUE(window_state->GetWindowPositionManaged());
  // Position should have been offset by 100,100.
  EXPECT_EQ("100,100", w1->bounds().origin().ToString());
  // Size should remain the same.
  EXPECT_EQ(size.ToString(), w1->bounds().size().ToString());

  // Explicitly disable window position auto management, and expect it to be
  // restored after drag completes.
  window_state->SetWindowPositionManaged(false);
  generator.PressLeftButton();
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&ContinueAndCompleteDrag, base::Unretained(&generator),
                     base::Unretained(window_state),
                     base::Unretained(w1.get())));
  EXPECT_EQ(::wm::MOVE_SUCCESSFUL,
            move_client->RunMoveLoop(w1.get(), gfx::Vector2d(100, 100),
                                     ::wm::WINDOW_MOVE_SOURCE_MOUSE));
  // Window position auto manage property should be restored to true.
  EXPECT_FALSE(window_state->GetWindowPositionManaged());
  // Position should have been offset by 100,100.
  EXPECT_EQ("200,200", w1->bounds().origin().ToString());
  // Size should remain the same.
  EXPECT_EQ(size.ToString(), w1->bounds().size().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, BottomRight) {
  std::unique_ptr<aura::Window> w1(CreateWindow(HTBOTTOMRIGHT));
  gfx::Point position = w1->bounds().origin();
  DragFromCenterBy(w1.get(), 100, 100);
  // Position should not have changed.
  EXPECT_EQ(position.ToString(), w1->bounds().origin().ToString());
  // Size should have increased by 100,100.
  EXPECT_EQ(gfx::Size(200, 200).ToString(), w1->bounds().size().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, GrowBox) {
  std::unique_ptr<aura::Window> w1(CreateWindow(HTGROWBOX));
  TestWindowDelegate* window_delegate =
      static_cast<TestWindowDelegate*>(w1->delegate());
  window_delegate->set_minimum_size(gfx::Size(40, 40));

  gfx::Point position = w1->bounds().origin();
  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow());
  generator.MoveMouseToCenterOf(w1.get());
  generator.DragMouseBy(100, 100);
  // Position should not have changed.
  EXPECT_EQ(position.ToString(), w1->bounds().origin().ToString());
  // Size should have increased by 100,100.
  EXPECT_EQ(gfx::Size(200, 200).ToString(), w1->bounds().size().ToString());

  // Shrink the window by (-100, -100).
  generator.DragMouseBy(-100, -100);
  // Position should not have changed.
  EXPECT_EQ(position.ToString(), w1->bounds().origin().ToString());
  // Size should have decreased by 100,100.
  EXPECT_EQ(gfx::Size(100, 100).ToString(), w1->bounds().size().ToString());

  // Enforce minimum size.
  generator.DragMouseBy(-60, -60);
  EXPECT_EQ(position.ToString(), w1->bounds().origin().ToString());
  EXPECT_EQ(gfx::Size(40, 40).ToString(), w1->bounds().size().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, Right) {
  std::unique_ptr<aura::Window> w1(CreateWindow(HTRIGHT));
  gfx::Point position = w1->bounds().origin();
  DragFromCenterBy(w1.get(), 100, 100);
  // Position should not have changed.
  EXPECT_EQ(position.ToString(), w1->bounds().origin().ToString());
  // Size should have increased by 100,0.
  EXPECT_EQ(gfx::Size(200, 100).ToString(), w1->bounds().size().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, Bottom) {
  std::unique_ptr<aura::Window> w1(CreateWindow(HTBOTTOM));
  gfx::Point position = w1->bounds().origin();
  DragFromCenterBy(w1.get(), 100, 100);
  // Position should not have changed.
  EXPECT_EQ(position.ToString(), w1->bounds().origin().ToString());
  // Size should have increased by 0,100.
  EXPECT_EQ(gfx::Size(100, 200).ToString(), w1->bounds().size().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, TopRight) {
  std::unique_ptr<aura::Window> w1(CreateWindow(HTTOPRIGHT));
  DragFromCenterBy(w1.get(), -50, 50);
  // Position should have been offset by 0,50.
  EXPECT_EQ(gfx::Point(0, 50).ToString(), w1->bounds().origin().ToString());
  // Size should have decreased by 50,50.
  EXPECT_EQ(gfx::Size(50, 50).ToString(), w1->bounds().size().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, Top) {
  std::unique_ptr<aura::Window> w1(CreateWindow(HTTOP));
  DragFromCenterBy(w1.get(), 50, 50);
  // Position should have been offset by 0,50.
  EXPECT_EQ(gfx::Point(0, 50).ToString(), w1->bounds().origin().ToString());
  // Size should have decreased by 0,50.
  EXPECT_EQ(gfx::Size(100, 50).ToString(), w1->bounds().size().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, Left) {
  std::unique_ptr<aura::Window> w1(CreateWindow(HTLEFT));
  DragFromCenterBy(w1.get(), 50, 50);
  // Position should have been offset by 50,0.
  EXPECT_EQ(gfx::Point(50, 0).ToString(), w1->bounds().origin().ToString());
  // Size should have decreased by 50,0.
  EXPECT_EQ(gfx::Size(50, 100).ToString(), w1->bounds().size().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, BottomLeft) {
  std::unique_ptr<aura::Window> w1(CreateWindow(HTBOTTOMLEFT));
  DragFromCenterBy(w1.get(), 50, -50);
  // Position should have been offset by 50,0.
  EXPECT_EQ(gfx::Point(50, 0).ToString(), w1->bounds().origin().ToString());
  // Size should have decreased by 50,50.
  EXPECT_EQ(gfx::Size(50, 50).ToString(), w1->bounds().size().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, TopLeft) {
  std::unique_ptr<aura::Window> w1(CreateWindow(HTTOPLEFT));
  DragFromCenterBy(w1.get(), 50, 50);
  // Position should have been offset by 50,50.
  EXPECT_EQ(gfx::Point(50, 50).ToString(), w1->bounds().origin().ToString());
  // Size should have decreased by 50,50.
  EXPECT_EQ(gfx::Size(50, 50).ToString(), w1->bounds().size().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, Client) {
  std::unique_ptr<aura::Window> w1(CreateWindow(HTCLIENT));
  gfx::Rect bounds = w1->bounds();
  DragFromCenterBy(w1.get(), 100, 100);
  // Neither position nor size should have changed.
  EXPECT_EQ(bounds.ToString(), w1->bounds().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, LeftPastMinimum) {
  std::unique_ptr<aura::Window> w1(CreateWindow(HTLEFT));
  TestWindowDelegate* window_delegate =
      static_cast<TestWindowDelegate*>(w1->delegate());
  window_delegate->set_minimum_size(gfx::Size(40, 40));

  // Simulate a large left-to-right drag.  Window width should be clamped to
  // minimum and position change should be limited as well.
  DragFromCenterBy(w1.get(), 333, 0);
  EXPECT_EQ(gfx::Point(60, 0).ToString(), w1->bounds().origin().ToString());
  EXPECT_EQ(gfx::Size(40, 100).ToString(), w1->bounds().size().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, RightPastMinimum) {
  std::unique_ptr<aura::Window> w1(CreateWindow(HTRIGHT));
  TestWindowDelegate* window_delegate =
      static_cast<TestWindowDelegate*>(w1->delegate());
  window_delegate->set_minimum_size(gfx::Size(40, 40));
  gfx::Point position = w1->bounds().origin();

  // Simulate a large right-to-left drag.  Window width should be clamped to
  // minimum and position should not change.
  DragFromCenterBy(w1.get(), -333, 0);
  EXPECT_EQ(position.ToString(), w1->bounds().origin().ToString());
  EXPECT_EQ(gfx::Size(40, 100).ToString(), w1->bounds().size().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, TopLeftPastMinimum) {
  std::unique_ptr<aura::Window> w1(CreateWindow(HTTOPLEFT));
  TestWindowDelegate* window_delegate =
      static_cast<TestWindowDelegate*>(w1->delegate());
  window_delegate->set_minimum_size(gfx::Size(40, 40));

  // Simulate a large top-left to bottom-right drag.  Window width should be
  // clamped to minimum and position should be limited.
  DragFromCenterBy(w1.get(), 333, 444);
  EXPECT_EQ(gfx::Point(60, 60).ToString(), w1->bounds().origin().ToString());
  EXPECT_EQ(gfx::Size(40, 40).ToString(), w1->bounds().size().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, TopRightPastMinimum) {
  std::unique_ptr<aura::Window> w1(CreateWindow(HTTOPRIGHT));
  TestWindowDelegate* window_delegate =
      static_cast<TestWindowDelegate*>(w1->delegate());
  window_delegate->set_minimum_size(gfx::Size(40, 40));

  // Simulate a large top-right to bottom-left drag.  Window size should be
  // clamped to minimum, x position should not change, and y position should
  // be clamped.
  DragFromCenterBy(w1.get(), -333, 444);
  EXPECT_EQ(gfx::Point(0, 60).ToString(), w1->bounds().origin().ToString());
  EXPECT_EQ(gfx::Size(40, 40).ToString(), w1->bounds().size().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, BottomLeftPastMinimum) {
  std::unique_ptr<aura::Window> w1(CreateWindow(HTBOTTOMLEFT));
  TestWindowDelegate* window_delegate =
      static_cast<TestWindowDelegate*>(w1->delegate());
  window_delegate->set_minimum_size(gfx::Size(40, 40));

  // Simulate a large bottom-left to top-right drag.  Window size should be
  // clamped to minimum, x position should be clamped, and y position should
  // not change.
  DragFromCenterBy(w1.get(), 333, -444);
  EXPECT_EQ(gfx::Point(60, 0).ToString(), w1->bounds().origin().ToString());
  EXPECT_EQ(gfx::Size(40, 40).ToString(), w1->bounds().size().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, BottomRightPastMinimum) {
  std::unique_ptr<aura::Window> w1(CreateWindow(HTBOTTOMRIGHT));
  TestWindowDelegate* window_delegate =
      static_cast<TestWindowDelegate*>(w1->delegate());
  window_delegate->set_minimum_size(gfx::Size(40, 40));
  gfx::Point position = w1->bounds().origin();

  // Simulate a large bottom-right to top-left drag.  Window size should be
  // clamped to minimum and position should not change.
  DragFromCenterBy(w1.get(), -333, -444);
  EXPECT_EQ(position.ToString(), w1->bounds().origin().ToString());
  EXPECT_EQ(gfx::Size(40, 40).ToString(), w1->bounds().size().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, BottomRightWorkArea) {
  std::unique_ptr<aura::Window> target(CreateWindow(HTBOTTOMRIGHT));
  gfx::Rect work_area = display::Screen::GetScreen()
                            ->GetDisplayNearestWindow(target.get())
                            .work_area();
  gfx::Point position = target->bounds().origin();
  // Drag further than work_area bottom.
  DragFromCenterBy(target.get(), 100, work_area.height());
  // Position should not have changed.
  EXPECT_EQ(position.ToString(), target->bounds().origin().ToString());
  // Size should have increased by 100, work_area.height() - target->bounds.y()
  EXPECT_EQ(
      gfx::Size(200, work_area.height() - target->bounds().y()).ToString(),
      target->bounds().size().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, BottomLeftWorkArea) {
  std::unique_ptr<aura::Window> target(CreateWindow(HTBOTTOMLEFT));
  gfx::Rect work_area = display::Screen::GetScreen()
                            ->GetDisplayNearestWindow(target.get())
                            .work_area();
  gfx::Point position = target->bounds().origin();
  // Drag further than work_area bottom.
  DragFromCenterBy(target.get(), -30, work_area.height());
  // origin is now at 70, 100.
  EXPECT_EQ(position.x() - 30, target->bounds().x());
  EXPECT_EQ(position.y(), target->bounds().y());
  // Size should have increased by 30, work_area.height() - target->bounds.y()
  EXPECT_EQ(
      gfx::Size(130, work_area.height() - target->bounds().y()).ToString(),
      target->bounds().size().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, BottomWorkArea) {
  std::unique_ptr<aura::Window> target(CreateWindow(HTBOTTOM));
  gfx::Rect work_area = display::Screen::GetScreen()
                            ->GetDisplayNearestWindow(target.get())
                            .work_area();
  gfx::Point position = target->bounds().origin();
  // Drag further than work_area bottom.
  DragFromCenterBy(target.get(), 0, work_area.height());
  // Position should not have changed.
  EXPECT_EQ(position.ToString(), target->bounds().origin().ToString());
  // Size should have increased by 0, work_area.height() - target->bounds.y()
  EXPECT_EQ(
      gfx::Size(100, work_area.height() - target->bounds().y()).ToString(),
      target->bounds().size().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, DontDragIfModalChild) {
  std::unique_ptr<aura::Window> w1(CreateWindow(HTCAPTION));
  std::unique_ptr<aura::Window> w2(CreateWindow(HTCAPTION));
  w2->SetBounds(gfx::Rect(100, 0, 100, 100));
  w2->SetProperty(aura::client::kModalKey, ui::MODAL_TYPE_WINDOW);
  ::wm::AddTransientChild(w1.get(), w2.get());
  gfx::Size size = w1->bounds().size();

  // Attempt to drag w1, position and size should not change because w1 has a
  // modal child.
  DragFromCenterBy(w1.get(), 100, 100);
  EXPECT_EQ("0,0", w1->bounds().origin().ToString());
  EXPECT_EQ(size.ToString(), w1->bounds().size().ToString());

  TouchDragFromCenterBy(w1.get(), 100, 100);
  EXPECT_EQ("0,0", w1->bounds().origin().ToString());
  EXPECT_EQ(size.ToString(), w1->bounds().size().ToString());
}

// Verifies we don't let windows drag to a -y location.
TEST_F(ToplevelWindowEventHandlerTest, DontDragToNegativeY) {
  std::unique_ptr<aura::Window> target(CreateWindow(HTTOP));
  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(),
                                     target.get());
  generator.MoveMouseTo(0, 5);
  generator.DragMouseBy(0, -5);
  // The y location and height should not have changed.
  EXPECT_EQ(0, target->bounds().y());
  EXPECT_EQ(100, target->bounds().height());
}

// Verifies we don't let windows go bigger than the display width.
TEST_F(ToplevelWindowEventHandlerTest, DontGotWiderThanScreen) {
  std::unique_ptr<aura::Window> target(CreateWindow(HTRIGHT));
  gfx::Rect work_area = display::Screen::GetScreen()
                            ->GetDisplayNearestWindow(target.get())
                            .bounds();
  DragFromCenterBy(target.get(), work_area.width() * 2, 0);
  // The y location and height should not have changed.
  EXPECT_EQ(work_area.width(), target->bounds().width());
}

// Verifies that touch-gestures drag the window correctly.
TEST_F(ToplevelWindowEventHandlerTest, GestureDrag) {
  std::unique_ptr<aura::Window> target(CreateTestWindowInShellWithDelegate(
      new TestWindowDelegate(HTCAPTION), 0, gfx::Rect(0, 0, 100, 100)));
  WindowState* window_state = WindowState::Get(target.get());
  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(),
                                     target.get());
  gfx::Rect old_bounds = target->bounds();
  gfx::Point location(5, 5);
  target->SetProperty(aura::client::kResizeBehaviorKey,
                      aura::client::kResizeBehaviorCanResize |
                          aura::client::kResizeBehaviorCanMaximize |
                          aura::client::kResizeBehaviorCanMinimize);

  gfx::Point end = location;

  // Snap right;
  end.Offset(100, 0);
  generator.GestureScrollSequence(location, end,
                                  base::TimeDelta::FromMilliseconds(5), 10);
  base::RunLoop().RunUntilIdle();

  // Verify that the window has moved after the gesture.
  EXPECT_NE(old_bounds.ToString(), target->bounds().ToString());
  EXPECT_EQ(WindowStateType::kRightSnapped, window_state->GetStateType());

  old_bounds = target->bounds();

  // Snap left.
  end = location = target->GetBoundsInRootWindow().CenterPoint();
  end.Offset(-100, 0);
  generator.GestureScrollSequence(location, end,
                                  base::TimeDelta::FromMilliseconds(5), 10);
  base::RunLoop().RunUntilIdle();

  EXPECT_NE(old_bounds.ToString(), target->bounds().ToString());
  EXPECT_EQ(WindowStateType::kLeftSnapped, window_state->GetStateType());

  gfx::Rect bounds_before_maximization = target->bounds();
  bounds_before_maximization.Offset(0, 100);
  target->SetBounds(bounds_before_maximization);
  old_bounds = target->bounds();

  // Maximize.
  end = location = target->GetBoundsInRootWindow().CenterPoint();
  end.Offset(0, -100);
  generator.GestureScrollSequence(location, end,
                                  base::TimeDelta::FromMilliseconds(5), 10);
  base::RunLoop().RunUntilIdle();

  EXPECT_NE(old_bounds.ToString(), target->bounds().ToString());
  EXPECT_TRUE(window_state->IsMaximized());
  EXPECT_EQ(old_bounds.ToString(),
            window_state->GetRestoreBoundsInScreen().ToString());

  window_state->Restore();
  target->SetBounds(old_bounds);

  // Minimize.
  end = location = target->GetBoundsInRootWindow().CenterPoint();
  end.Offset(0, 100);
  generator.GestureScrollSequence(location, end,
                                  base::TimeDelta::FromMilliseconds(5), 10);
  base::RunLoop().RunUntilIdle();
  EXPECT_NE(old_bounds.ToString(), target->bounds().ToString());
  EXPECT_TRUE(window_state->IsMinimized());
  EXPECT_TRUE(window_state->unminimize_to_restore_bounds());
  EXPECT_EQ(old_bounds.ToString(),
            window_state->GetRestoreBoundsInScreen().ToString());
}

// Verifies that window dragged by touch-gestures to the edge of display
// will not lead to system crash (see https://crbug.com/917060).
TEST_F(ToplevelWindowEventHandlerTest, GestureDragMultiDisplays) {
  UpdateDisplay("800x600, 800x600");
  std::unique_ptr<aura::Window> target(CreateTestWindowInShellWithDelegate(
      new TestWindowDelegate(HTCAPTION), 0, gfx::Rect(0, 0, 100, 100)));
  WindowState* window_state = WindowState::Get(target.get());
  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(),
                                     target.get());
  gfx::Rect old_bounds = target->bounds();
  gfx::Point location(5, 5);
  gfx::Point end = location;

  // On real device, gesture event's location may not be accurate. For example,
  // when window is dragged by touch-gestures to the edge of display, it may
  // create gesture events with location out of the display bounds. Let |end| be
  // out of the primary display's bounds to emulate this situation.
  end.Offset(800, 0);
  generator.GestureScrollSequence(location, end,
                                  base::TimeDelta::FromMilliseconds(5), 10);

  // Verify that the window has moved after the gesture.
  EXPECT_NE(old_bounds.ToString(), target->bounds().ToString());
  EXPECT_EQ(WindowStateType::kRightSnapped, window_state->GetStateType());
}

// Tests that a gesture cannot minimize an unminimizeable window.
TEST_F(ToplevelWindowEventHandlerTest,
       GestureAttemptMinimizeUnminimizeableWindow) {
  std::unique_ptr<aura::Window> target(CreateWindow(HTCAPTION));
  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(),
                                     target.get());
  gfx::Point location(5, 5);
  target->SetProperty(aura::client::kResizeBehaviorKey,
                      aura::client::kResizeBehaviorCanMaximize);

  gfx::Point end = location;
  end.Offset(0, 100);
  generator.GestureScrollSequence(location, end,
                                  base::TimeDelta::FromMilliseconds(5), 10);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(WindowState::Get(target.get())->IsMinimized());
}

TEST_F(ToplevelWindowEventHandlerTest, TwoFingerDragDifferentDelta) {
  std::unique_ptr<aura::Window> target(CreateWindow(HTCAPTION));
  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(),
                                     target.get());

  const int kSteps = 10;
  const int kTouchPoints = 2;
  gfx::Point points[kTouchPoints] = {
      gfx::Point(5, 5),   // Within caption.
      gfx::Point(55, 5),  // Within caption.
  };
  gfx::Vector2d delta[kTouchPoints] = {
      gfx::Vector2d(80, 80), gfx::Vector2d(20, 20),
  };
  int delay_adding_finger_ms[kTouchPoints] = {0, 0};
  int delay_releasing_finger_ms[kTouchPoints] = {150, 150};

  gfx::Rect bounds = target->bounds();
  // Swipe right and down starting with two fingers. Two fingers have different
  // moving deltas. The window position should move along the average vector of
  // these two fingers.
  generator.GestureMultiFingerScrollWithDelays(
      kTouchPoints, points, delta, delay_adding_finger_ms,
      delay_releasing_finger_ms, 15, kSteps);
  bounds += gfx::Vector2d(50, 50);
  EXPECT_EQ(bounds.ToString(), target->bounds().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, TwoFingerDragDelayAddFinger) {
  std::unique_ptr<aura::Window> target(CreateWindow(HTCAPTION));
  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(),
                                     target.get());

  const int kSteps = 10;
  const int kTouchPoints = 2;
  gfx::Point points[kTouchPoints] = {
      gfx::Point(5, 5),   // Within caption.
      gfx::Point(55, 5),  // Within caption.
  };
  gfx::Vector2d delta[kTouchPoints] = {
      gfx::Vector2d(50, 50), gfx::Vector2d(50, 50),
  };
  int delay_adding_finger_ms[kTouchPoints] = {0, 90};
  int delay_releasing_finger_ms[kTouchPoints] = {150, 150};

  gfx::Rect bounds = target->bounds();
  // Swipe right and down starting with one fingers. Add another finger at 90ms
  // and continue dragging. The drag should continue without interrupt.
  generator.GestureMultiFingerScrollWithDelays(
      kTouchPoints, points, delta, delay_adding_finger_ms,
      delay_releasing_finger_ms, 15, kSteps);
  bounds += gfx::Vector2d(50, 50);
  EXPECT_EQ(bounds.ToString(), target->bounds().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, TwoFingerDragDelayReleaseFinger) {
  std::unique_ptr<aura::Window> target(CreateWindow(HTCAPTION));
  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(),
                                     target.get());

  const int kSteps = 10;
  const int kTouchPoints = 2;
  gfx::Point points[kTouchPoints] = {
      gfx::Point(5, 5),   // Within caption.
      gfx::Point(55, 5),  // Within caption.
  };
  gfx::Vector2d delta[kTouchPoints] = {
      gfx::Vector2d(50, 50), gfx::Vector2d(50, 50),
  };
  int delay_adding_finger_ms[kTouchPoints] = {0, 0};
  int delay_releasing_finger_ms[kTouchPoints] = {150, 90};

  gfx::Rect bounds = target->bounds();
  // Swipe right and down starting with two fingers. Remove one finger at 90ms
  // and continue dragging. The drag should continue without interrupt.
  generator.GestureMultiFingerScrollWithDelays(
      kTouchPoints, points, delta, delay_adding_finger_ms,
      delay_releasing_finger_ms, 15, kSteps);
  bounds += gfx::Vector2d(50, 50);
  EXPECT_EQ(bounds.ToString(), target->bounds().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest,
       TwoFingerDragDelayAdd2ndAndRelease2ndFinger) {
  std::unique_ptr<aura::Window> target(CreateWindow(HTCAPTION));
  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(),
                                     target.get());

  const int kSteps = 10;
  const int kTouchPoints = 2;
  gfx::Point points[kTouchPoints] = {
      gfx::Point(5, 5),   // Within caption.
      gfx::Point(55, 5),  // Within caption.
  };
  gfx::Vector2d delta[kTouchPoints] = {
      gfx::Vector2d(50, 50), gfx::Vector2d(50, 50),
  };
  int delay_adding_finger_ms[kTouchPoints] = {0, 30};
  int delay_releasing_finger_ms[kTouchPoints] = {150, 120};

  gfx::Rect bounds = target->bounds();
  // Swipe right and down starting with one fingers. Add second finger at 30ms,
  // continue dragging, release second finger at 120ms and continue dragging.
  // The drag should continue without interrupt.
  generator.GestureMultiFingerScrollWithDelays(
      kTouchPoints, points, delta, delay_adding_finger_ms,
      delay_releasing_finger_ms, 15, kSteps);
  bounds += gfx::Vector2d(50, 50);
  EXPECT_EQ(bounds.ToString(), target->bounds().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest,
       TwoFingerDragDelayAdd2ndAndRelease1stFinger) {
  std::unique_ptr<aura::Window> target(CreateWindow(HTCAPTION));
  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(),
                                     target.get());

  const int kSteps = 10;
  const int kTouchPoints = 2;
  gfx::Point points[kTouchPoints] = {
      gfx::Point(5, 5),   // Within caption.
      gfx::Point(55, 5),  // Within caption.
  };
  gfx::Vector2d delta[kTouchPoints] = {
      gfx::Vector2d(50, 50), gfx::Vector2d(50, 50),
  };
  int delay_adding_finger_ms[kTouchPoints] = {0, 30};
  int delay_releasing_finger_ms[kTouchPoints] = {120, 150};

  gfx::Rect bounds = target->bounds();
  // Swipe right and down starting with one fingers. Add second finger at 30ms,
  // continue dragging, release first finger at 120ms and continue dragging.
  // The drag should continue without interrupt.
  generator.GestureMultiFingerScrollWithDelays(
      kTouchPoints, points, delta, delay_adding_finger_ms,
      delay_releasing_finger_ms, 15, kSteps);
  bounds += gfx::Vector2d(50, 50);
  EXPECT_EQ(bounds.ToString(), target->bounds().ToString());
}

TEST_F(ToplevelWindowEventHandlerTest, GestureDragToRestore) {
  std::unique_ptr<aura::Window> window(CreateTestWindowInShellWithDelegate(
      new TestWindowDelegate(HTCAPTION), 0, gfx::Rect(10, 20, 30, 40)));
  window->Show();
  WindowState* window_state = WindowState::Get(window.get());
  window_state->Activate();

  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(),
                                     window.get());
  gfx::Rect old_bounds = window->bounds();
  gfx::Point location, end;
  end = location = window->GetBoundsInRootWindow().CenterPoint();
  end.Offset(0, 100);
  generator.GestureScrollSequence(location, end,
                                  base::TimeDelta::FromMilliseconds(5), 10);
  base::RunLoop().RunUntilIdle();
  EXPECT_NE(old_bounds.ToString(), window->bounds().ToString());
  EXPECT_TRUE(window_state->IsMinimized());
  EXPECT_TRUE(window_state->unminimize_to_restore_bounds());
  EXPECT_EQ(old_bounds.ToString(),
            window_state->GetRestoreBoundsInScreen().ToString());
}

// Tests that EasyResizeWindowTargeter expands the hit-test area when a
// top-level window can be resized but not when the window is not resizable.
TEST_F(ToplevelWindowEventHandlerTest, EasyResizerUsedForTopLevel) {
  std::unique_ptr<aura::Window> w1(CreateTestWindowInShellWithDelegate(
      new TestWindowDelegate(HTCAPTION), -1, gfx::Rect(0, 0, 100, 100)));
  std::unique_ptr<aura::Window> w2(CreateTestWindowInShellWithDelegate(
      new TestWindowDelegate(HTCAPTION), -2, gfx::Rect(40, 40, 100, 100)));
  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(),
                                     gfx::Point(5, 5));

  generator.PressMoveAndReleaseTouchTo(gfx::Point(5, 5));
  EXPECT_TRUE(wm::IsActiveWindow(w1.get()));

  // Make |w1| resizable to allow touch events to go to it (and not |w2|) thanks
  // to EasyResizeWindowTargeter.
  w1->SetProperty(aura::client::kResizeBehaviorKey,
                  aura::client::kResizeBehaviorCanMaximize |
                      aura::client::kResizeBehaviorCanMinimize |
                      aura::client::kResizeBehaviorCanResize);
  // Clicking a point within |w2| but close to |w1| should not activate |w2|.
  const gfx::Point touch_point(105, 105);
  generator.MoveTouch(touch_point);
  generator.PressMoveAndReleaseTouchTo(touch_point);
  EXPECT_TRUE(wm::IsActiveWindow(w1.get()));

  // Make |w1| not resizable to allow touch events to go to |w2| even when close
  // to |w1|.
  w1->SetProperty(aura::client::kResizeBehaviorKey,
                  aura::client::kResizeBehaviorCanMaximize |
                      aura::client::kResizeBehaviorCanMinimize);
  // Clicking a point within |w2| should activate that window.
  generator.PressMoveAndReleaseTouchTo(touch_point);
  EXPECT_TRUE(wm::IsActiveWindow(w2.get()));
}

// Tests that EasyResizeWindowTargeter expands the hit-test area when a
// window is a transient child of a top-level window and is resizable.
TEST_F(ToplevelWindowEventHandlerTest, EasyResizerUsedForTransient) {
  std::unique_ptr<aura::Window> w1(CreateTestWindowInShellWithDelegate(
      new TestWindowDelegate(HTCAPTION), -1, gfx::Rect(0, 0, 100, 100)));
  std::unique_ptr<aura::Window> w11(CreateTestWindowInShellWithDelegate(
      new TestWindowDelegate(HTCAPTION), -11, gfx::Rect(20, 20, 50, 50)));
  ::wm::AddTransientChild(w1.get(), w11.get());
  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(),
                                     gfx::Point(10, 10));

  // Make |w11| non-resizable to avoid touch events inside its transient parent
  // |w1| from going to |w11| because of EasyResizeWindowTargeter.
  w11->SetProperty(aura::client::kResizeBehaviorKey,
                   aura::client::kResizeBehaviorCanMaximize |
                       aura::client::kResizeBehaviorCanMinimize);
  // Clicking a point within w1 should activate that window.
  generator.PressMoveAndReleaseTouchTo(gfx::Point(10, 10));
  EXPECT_TRUE(wm::IsActiveWindow(w1.get()));

  // Make |w11| resizable to allow touch events inside its transient parent
  // |w1| that are close to |w11| border to go to |w11| thanks to
  // EasyResizeWindowTargeter.
  w11->SetProperty(aura::client::kResizeBehaviorKey,
                   aura::client::kResizeBehaviorCanMaximize |
                       aura::client::kResizeBehaviorCanMinimize |
                       aura::client::kResizeBehaviorCanResize);
  // Clicking a point within |w1| but close to |w11| should activate |w11|.
  generator.PressMoveAndReleaseTouchTo(gfx::Point(10, 10));
  EXPECT_TRUE(wm::IsActiveWindow(w11.get()));
}

// Tests that an unresizable window cannot be dragged or snapped using gestures.
TEST_F(ToplevelWindowEventHandlerTest, GestureDragForUnresizableWindow) {
  std::unique_ptr<aura::Window> target(CreateWindow(HTCAPTION));
  WindowState* window_state = WindowState::Get(target.get());

  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(),
                                     target.get());
  gfx::Rect old_bounds = target->bounds();
  gfx::Point location(5, 5);

  target->SetProperty(aura::client::kResizeBehaviorKey,
                      aura::client::kResizeBehaviorNone);

  gfx::Point end = location;

  // Try to snap right. The window is not resizable. So it should not snap.
  end.Offset(100, 0);
  generator.GestureScrollSequence(location, end,
                                  base::TimeDelta::FromMilliseconds(5), 10);
  base::RunLoop().RunUntilIdle();

  // Verify that the window has moved after the gesture.
  gfx::Rect expected_bounds(old_bounds);
  expected_bounds.Offset(gfx::Vector2d(100, 0));
  EXPECT_EQ(expected_bounds.ToString(), target->bounds().ToString());

  // Verify that the window did not snap left.
  EXPECT_TRUE(window_state->IsNormalStateType());

  old_bounds = target->bounds();

  // Try to snap left. It should not snap.
  end = location = target->GetBoundsInRootWindow().CenterPoint();
  end.Offset(-100, 0);
  generator.GestureScrollSequence(location, end,
                                  base::TimeDelta::FromMilliseconds(5), 10);
  base::RunLoop().RunUntilIdle();

  // Verify that the window has moved after the gesture.
  expected_bounds = old_bounds;
  expected_bounds.Offset(gfx::Vector2d(-100, 0));
  EXPECT_EQ(expected_bounds.ToString(), target->bounds().ToString());

  // Verify that the window did not snap left.
  EXPECT_TRUE(window_state->IsNormalStateType());
}

// Tests that dragging multiple windows at the same time is not allowed.
TEST_F(ToplevelWindowEventHandlerTest, GestureDragMultipleWindows) {
  std::unique_ptr<aura::Window> target(CreateTestWindowInShellWithDelegate(
      new TestWindowDelegate(HTCAPTION), 0, gfx::Rect(0, 0, 100, 100)));
  std::unique_ptr<aura::Window> notmoved(CreateTestWindowInShellWithDelegate(
      new TestWindowDelegate(HTCAPTION), 1, gfx::Rect(100, 0, 100, 100)));

  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(),
                                     target.get());
  gfx::Point location(5, 5);

  // Send some touch events to start dragging |target|.
  generator.MoveTouch(location);
  generator.PressTouch();
  location.Offset(40, 5);
  generator.MoveTouch(location);

  // Try to drag |notmoved| window. This should not move the window.
  {
    gfx::Rect bounds = notmoved->bounds();
    ui::test::EventGenerator gen(Shell::GetPrimaryRootWindow(), notmoved.get());
    gfx::Point start = notmoved->bounds().origin() + gfx::Vector2d(10, 10);
    gfx::Point end = start + gfx::Vector2d(100, 10);
    gen.GestureScrollSequence(start, end, base::TimeDelta::FromMilliseconds(10),
                              10);
    EXPECT_EQ(bounds.ToString(), notmoved->bounds().ToString());
  }
}

// Verifies pressing escape resets the bounds to the original bounds.
TEST_F(ToplevelWindowEventHandlerTest, EscapeReverts) {
  std::unique_ptr<aura::Window> target(CreateWindow(HTBOTTOMRIGHT));
  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(),
                                     target.get());
  generator.PressLeftButton();
  generator.MoveMouseBy(10, 11);

  // Execute any scheduled draws so that pending mouse events are processed.
  base::RunLoop().RunUntilIdle();

  EXPECT_EQ("0,0 110x111", target->bounds().ToString());
  generator.PressKey(ui::VKEY_ESCAPE, 0);
  generator.ReleaseKey(ui::VKEY_ESCAPE, 0);
  EXPECT_EQ("0,0 100x100", target->bounds().ToString());
}

// Verifies window minimization/maximization completes drag.
TEST_F(ToplevelWindowEventHandlerTest, MinimizeMaximizeCompletes) {
  // Once window is minimized, window dragging completes.
  {
    std::unique_ptr<aura::Window> target(CreateWindow(HTCAPTION));
    target->Focus();
    ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(),
                                       target.get());
    generator.PressLeftButton();
    generator.MoveMouseBy(10, 11);
    base::RunLoop().RunUntilIdle();
    EXPECT_EQ("10,11 100x100", target->bounds().ToString());
    WindowState* window_state = WindowState::Get(target.get());
    window_state->Minimize();
    window_state->Restore();

    generator.PressLeftButton();
    generator.MoveMouseBy(10, 11);
    base::RunLoop().RunUntilIdle();
    EXPECT_EQ("10,11 100x100", target->bounds().ToString());
  }

  // Once window is maximized, window dragging completes.
  {
    std::unique_ptr<aura::Window> target(CreateWindow(HTCAPTION));
    target->Focus();
    ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(),
                                       target.get());
    generator.PressLeftButton();
    generator.MoveMouseBy(10, 11);
    base::RunLoop().RunUntilIdle();
    EXPECT_EQ("10,11 100x100", target->bounds().ToString());
    WindowState* window_state = WindowState::Get(target.get());
    window_state->Maximize();
    window_state->Restore();

    generator.PressLeftButton();
    generator.MoveMouseBy(10, 11);
    base::RunLoop().RunUntilIdle();
    EXPECT_EQ("10,11 100x100", target->bounds().ToString());
  }
}

// Verifies that a drag cannot be started via
// wm::WindowMoveClient::RunMoveLoop() while another drag is already
// in progress.
TEST_F(ToplevelWindowEventHandlerTest, RunMoveLoopFailsDuringInProgressDrag) {
  std::unique_ptr<aura::Window> window1(CreateWindow(HTCAPTION));
  EXPECT_EQ("0,0 100x100", window1->bounds().ToString());
  std::unique_ptr<aura::Window> window2(CreateWindow(HTCAPTION));

  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(),
                                     window1.get());
  window1->Focus();
  generator.PressLeftButton();
  generator.MoveMouseBy(10, 11);
  EXPECT_EQ("10,11 100x100", window1->bounds().ToString());

  ::wm::WindowMoveClient* move_client =
      ::wm::GetWindowMoveClient(window2->GetRootWindow());
  EXPECT_EQ(::wm::MOVE_CANCELED,
            move_client->RunMoveLoop(window2.get(), gfx::Vector2d(),
                                     ::wm::WINDOW_MOVE_SOURCE_MOUSE));

  generator.ReleaseLeftButton();
  EXPECT_EQ("10,11 100x100", window1->bounds().ToString());
}

class ToplevelWindowEventHandlerBackGestureTest : public AshTestBase {
 public:
  // Distance that swiping from left edge to let the affordance achieve
  // activated state.
  static constexpr int kSwipingDistanceForGoingBack = 80;

  ToplevelWindowEventHandlerBackGestureTest() = default;
  ~ToplevelWindowEventHandlerBackGestureTest() override = default;

  void SetUp() override {
    AshTestBase::SetUp();

    feature_list_.InitAndEnableFeature(
        ash::features::kSwipingFromLeftEdgeToGoBack);
    top_window_ = CreateAppWindow(gfx::Rect(), AppType::BROWSER);
    TabletModeControllerTestApi().EnterTabletMode();
  }

  void TearDown() override {
    top_window_.reset();
    AshTestBase::TearDown();
  }

  void RegisterBackPressAndRelease(ui::TestAcceleratorTarget* back_press,
                                   ui::TestAcceleratorTarget* back_release) {
    AcceleratorControllerImpl* controller =
        Shell::Get()->accelerator_controller();

    // Register an accelerator that looks for back presses.
    ui::Accelerator accelerator_back_press(ui::VKEY_BROWSER_BACK, ui::EF_NONE);
    accelerator_back_press.set_key_state(ui::Accelerator::KeyState::PRESSED);
    controller->Register({accelerator_back_press}, back_press);

    // Register an accelerator that looks for back releases.
    ui::Accelerator accelerator_back_release(ui::VKEY_BROWSER_BACK,
                                             ui::EF_NONE);
    accelerator_back_release.set_key_state(ui::Accelerator::KeyState::RELEASED);
    controller->Register({accelerator_back_release}, back_release);
  }

  // Send touch event with |type| to the toplevel window event handler.
  void SendTouchEvent(const gfx::Point& position, ui::EventType type) {
    ui::TouchEvent event = ui::TouchEvent(
        type, position, base::TimeTicks::Now(),
        ui::PointerDetails(ui::EventPointerType::POINTER_TYPE_TOUCH,
                           /*pointer_id=*/5, /*radius_x=*/5.0f,
                           /*radius_y=*/5.0, /*force=*/1.0f));
    ui::Event::DispatcherApi(&event).set_target(top_window_.get());
    ash::Shell::Get()->toplevel_window_event_handler()->OnTouchEvent(&event);
  }

  aura::Window* top_window() { return top_window_.get(); }

 private:
  base::test::ScopedFeatureList feature_list_;
  std::unique_ptr<aura::Window> top_window_;

  DISALLOW_COPY_AND_ASSIGN(ToplevelWindowEventHandlerBackGestureTest);
};

TEST_F(ToplevelWindowEventHandlerBackGestureTest, SwipingFromLeftEdgeToGoBack) {
  ui::TestAcceleratorTarget target_back_press, target_back_release;
  RegisterBackPressAndRelease(&target_back_press, &target_back_release);

  // Tests that swiping from the left less than |kSwipingDistanceForGoingBack|
  // should not go to previous page.
  ui::test::EventGenerator* generator = GetEventGenerator();
  const gfx::Point start(0, 100);
  generator->GestureScrollSequence(
      start, gfx::Point(kSwipingDistanceForGoingBack - 10, 100),
      base::TimeDelta::FromMilliseconds(100), 3);
  EXPECT_EQ(0, target_back_press.accelerator_count());
  EXPECT_EQ(0, target_back_release.accelerator_count());

  // Tests that swiping from the left more than |kSwipingDistanceForGoingBack|
  // should go to previous page.
  generator->GestureScrollSequence(
      start, gfx::Point(kSwipingDistanceForGoingBack + 10, 100),
      base::TimeDelta::FromMilliseconds(100), 3);
  EXPECT_EQ(1, target_back_press.accelerator_count());
  EXPECT_EQ(1, target_back_release.accelerator_count());
}

TEST_F(ToplevelWindowEventHandlerBackGestureTest, FlingFromLeftEdgeToGoBack) {
  ui::TestAcceleratorTarget target_back_press, target_back_release;
  RegisterBackPressAndRelease(&target_back_press, &target_back_release);

  // Tests that fling from the left with velocity smaller than
  // |kFlingVelocityForGoingBack| should not go to previous page.
  // Drag further than |touch_slop| in GestureDetector to trigger scroll
  // sequence. Note, |touch_slop| equals to 15.05, which is the value of
  // |max_touch_move_in_pixels_for_click_| + |kSlopEpsilon|. Generate the scroll
  // sequence with short duration and only one step for FLING scroll gestures.
  // X-velocity here will be 800 dips/seconds.
  ui::test::EventGenerator* generator = GetEventGenerator();
  generator->GestureScrollSequence(gfx::Point(0, 0), gfx::Point(16, 0),
                                   base::TimeDelta::FromMilliseconds(20),
                                   /*steps=*/1);
  EXPECT_EQ(0, target_back_press.accelerator_count());
  EXPECT_EQ(0, target_back_release.accelerator_count());

  // Tests that fling from the left with velocity larger than
  // |kFlingVelocityForGoingBack| should go to previous page. X-velocity here
  // will be 1600 dips/seconds.
  generator->GestureScrollSequence(gfx::Point(0, 0), gfx::Point(16, 0),
                                   base::TimeDelta::FromMilliseconds(1),
                                   /*steps=*/1);
  EXPECT_EQ(1, target_back_press.accelerator_count());
  EXPECT_EQ(1, target_back_release.accelerator_count());

  // Tests that fling from the left with velocity smaller than
  // |kFlingVelocityForGoingBack| but dragged further enough to trigger
  // activated affordance should still go back to previous page. X-velocity here
  // will be 800 dips/seconds and drag distance is 160, which is larger than
  // |kSwipingDistanceForGoingBack|.
  generator->GestureScrollSequence(gfx::Point(0, 0), gfx::Point(160, 0),
                                   base::TimeDelta::FromMilliseconds(200),
                                   /*steps=*/1);
  EXPECT_EQ(2, target_back_press.accelerator_count());
  EXPECT_EQ(2, target_back_release.accelerator_count());
}

TEST_F(ToplevelWindowEventHandlerBackGestureTest, GoBackInOverviewMode) {
  ui::TestAcceleratorTarget target_back_press, target_back_release;
  RegisterBackPressAndRelease(&target_back_press, &target_back_release);

  ash_test_helper()->test_shell_delegate()->SetCanGoBack(false);
  ASSERT_FALSE(WindowState::Get(top_window())->IsMinimized());
  ASSERT_TRUE(TabletModeWindowManager::ShouldMinimizeTopWindowOnBack());
  GetEventGenerator()->GestureScrollSequence(
      gfx::Point(0, 100), gfx::Point(kSwipingDistanceForGoingBack + 10, 100),
      base::TimeDelta::FromMilliseconds(100), 3);
  // Should trigger window minimize instead of go back.
  EXPECT_EQ(0, target_back_release.accelerator_count());
  EXPECT_TRUE(WindowState::Get(top_window())->IsMinimized());

  WindowState::Get(top_window())->Unminimize();
  ASSERT_FALSE(WindowState::Get(top_window())->IsMinimized());
  auto* shell = Shell::Get();
  shell->overview_controller()->StartOverview();
  ASSERT_TRUE(shell->overview_controller()->InOverviewSession());
  GetEventGenerator()->GestureScrollSequence(
      gfx::Point(0, 100), gfx::Point(kSwipingDistanceForGoingBack + 10, 100),
      base::TimeDelta::FromMilliseconds(100), 3);
  // Should trigger go back instead of minimize the window since it is in
  // overview mode.
  EXPECT_EQ(1, target_back_release.accelerator_count());
}

TEST_F(ToplevelWindowEventHandlerBackGestureTest, DonotStartGoingBack) {
  ui::TestAcceleratorTarget target_back_press, target_back_release;
  RegisterBackPressAndRelease(&target_back_press, &target_back_release);

  auto* shell = Shell::Get();
  ui::test::EventGenerator* generator = GetEventGenerator();
  const gfx::Point start(0, 100);

  // Should not go back if it is not in ACTIVE session.
  ASSERT_FALSE(shell->overview_controller()->InOverviewSession());
  ASSERT_FALSE(shell->home_screen_controller()->IsHomeScreenVisible());
  GetSessionControllerClient()->SetSessionState(
      session_manager::SessionState::LOCKED);
  generator->GestureScrollSequence(
      start, gfx::Point(kSwipingDistanceForGoingBack + 10, 100),
      base::TimeDelta::FromMilliseconds(100), 3);
  EXPECT_EQ(0, target_back_press.accelerator_count());
  EXPECT_EQ(0, target_back_release.accelerator_count());

  // Should not go back if home screen is visible and in |kFullscreenAllApps|
  // state.
  GetSessionControllerClient()->SetSessionState(
      session_manager::SessionState::ACTIVE);
  shell->home_screen_controller()->GoHome(GetPrimaryDisplay().id());
  ASSERT_TRUE(shell->home_screen_controller()->IsHomeScreenVisible());
  GetAppListTestHelper()->CheckState(AppListViewState::kFullscreenAllApps);
  generator->GestureScrollSequence(
      start, gfx::Point(kSwipingDistanceForGoingBack + 10, 100),
      base::TimeDelta::FromMilliseconds(100), 3);
  EXPECT_EQ(0, target_back_press.accelerator_count());
  EXPECT_EQ(0, target_back_release.accelerator_count());

  // Should exit |kFullscreenSearch| to enter |kFullscreenAllApps| state while
  // home screen search result page is opened.
  generator->GestureTapAt(GetAppListTestHelper()
                              ->GetAppListView()
                              ->search_box_view()
                              ->GetBoundsInScreen()
                              .CenterPoint());
  GetAppListTestHelper()->CheckState(AppListViewState::kFullscreenSearch);
  generator->GestureScrollSequence(
      start, gfx::Point(kSwipingDistanceForGoingBack + 10, 100),
      base::TimeDelta::FromMilliseconds(100), 3);
  EXPECT_EQ(1, target_back_release.accelerator_count());
  GetAppListTestHelper()->CheckState(AppListViewState::kFullscreenAllApps);
}

TEST_F(ToplevelWindowEventHandlerBackGestureTest, CancelOnScreenRotation) {
  UpdateDisplay("807x407");
  int64_t display_id = display::Screen::GetScreen()->GetPrimaryDisplay().id();
  display::DisplayManager* display_manager = Shell::Get()->display_manager();
  display::test::ScopedSetInternalDisplayId set_internal(display_manager,
                                                         display_id);
  ScreenOrientationControllerTestApi test_api(
      Shell::Get()->screen_orientation_controller());
  ui::TestAcceleratorTarget target_back_press, target_back_release;
  RegisterBackPressAndRelease(&target_back_press, &target_back_release);

  // Set the screen orientation to LANDSCAPE_PRIMARY.
  test_api.SetDisplayRotation(display::Display::ROTATE_0,
                              display::Display::RotationSource::ACTIVE);
  EXPECT_EQ(test_api.GetCurrentOrientation(),
            OrientationLockType::kLandscapePrimary);

  gfx::Point start(0, 100);
  gfx::Point update_and_end(200, 100);
  SendTouchEvent(start, ui::ET_TOUCH_PRESSED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_MOVED);
  // Rotate the screen by 270 degree during drag.
  test_api.SetDisplayRotation(display::Display::ROTATE_270,
                              display::Display::RotationSource::ACTIVE);
  EXPECT_EQ(test_api.GetCurrentOrientation(),
            OrientationLockType::kPortraitPrimary);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_RELEASED);
  // Left edge swipe back should be cancelled due to screen rotation, so the
  // fling event with velocity larger than |kFlingVelocityForGoingBack| above
  // will not trigger actual going back.
  EXPECT_EQ(0, target_back_press.accelerator_count());
  EXPECT_EQ(0, target_back_release.accelerator_count());
}

// Tests back gesture while in split view mode.
TEST_F(ToplevelWindowEventHandlerBackGestureTest, DragFromSplitViewDivider) {
  std::unique_ptr<aura::Window> window1 = CreateTestWindow();
  std::unique_ptr<aura::Window> window2 = CreateTestWindow();
  ui::TestAcceleratorTarget target_back_press, target_back_release;
  gfx::Rect display_bounds =
      screen_util::GetDisplayWorkAreaBoundsInScreenForActiveDeskContainer(
          window1.get());
  RegisterBackPressAndRelease(&target_back_press, &target_back_release);

  auto* split_view_controller =
      SplitViewController::Get(Shell::GetPrimaryRootWindow());
  split_view_controller->SnapWindow(window1.get(), SplitViewController::LEFT);
  split_view_controller->SnapWindow(window2.get(), SplitViewController::RIGHT);
  ASSERT_TRUE(split_view_controller->InSplitViewMode());
  ASSERT_EQ(SplitViewController::State::kBothSnapped,
            split_view_controller->state());

  gfx::Rect divider_bounds =
      split_view_controller->split_view_divider()->GetDividerBoundsInScreen(
          false);
  ui::test::EventGenerator* generator = GetEventGenerator();
  // Drag from the splitview divider's non-resizable area with larger than
  // |kSwipingDistanceForGoingBack| distance should trigger back gesture. The
  // snapped window should go to previous page and divider's position will not
  // be changed.
  gfx::Point start(divider_bounds.x(), 10);
  gfx::Point end(start.x() + kSwipingDistanceForGoingBack + 10, 10);
  EXPECT_GT(split_view_controller->divider_position(),
            0.33f * display_bounds.width());
  EXPECT_LE(split_view_controller->divider_position(),
            0.5f * display_bounds.width());
  generator->GestureScrollSequence(start, end,
                                   base::TimeDelta::FromMilliseconds(100), 3);
  EXPECT_EQ(SplitViewController::State::kBothSnapped,
            split_view_controller->state());
  EXPECT_EQ(1, target_back_press.accelerator_count());
  EXPECT_EQ(1, target_back_release.accelerator_count());
  EXPECT_GT(split_view_controller->divider_position(),
            0.33f * display_bounds.width());
  EXPECT_LE(split_view_controller->divider_position(),
            0.5f * display_bounds.width());

  // Drag from the divider's resizable area should trigger splitview resizing.
  // Divider's position will be changed and back gesture should not be
  // triggered.
  start = divider_bounds.CenterPoint();
  end = gfx::Point(0.67f * display_bounds.width(), start.y());
  generator->GestureScrollSequence(start, end,
                                   base::TimeDelta::FromMilliseconds(100), 3);
  EXPECT_EQ(1, target_back_press.accelerator_count());
  EXPECT_EQ(1, target_back_release.accelerator_count());
  EXPECT_GT(split_view_controller->divider_position(),
            0.5f * display_bounds.width());
  EXPECT_LE(split_view_controller->divider_position(),
            0.67f * display_bounds.width());
  split_view_controller->EndSplitView();
}

// Tests that in different screen orientations should always activate the
// snapped window in splitview that is underneath the finger. And should be the
// snapped window that is underneath to go back to the previous page.
TEST_F(ToplevelWindowEventHandlerBackGestureTest, BackInSplitViewMode) {
  int64_t display_id = display::Screen::GetScreen()->GetPrimaryDisplay().id();
  display::DisplayManager* display_manager = Shell::Get()->display_manager();
  display::test::ScopedSetInternalDisplayId set_internal(display_manager,
                                                         display_id);
  ScreenOrientationControllerTestApi test_api(
      Shell::Get()->screen_orientation_controller());
  ui::TestAcceleratorTarget target_back_press, target_back_release;
  RegisterBackPressAndRelease(&target_back_press, &target_back_release);

  std::unique_ptr<aura::Window> left_window = CreateTestWindow();
  std::unique_ptr<aura::Window> right_window = CreateTestWindow();
  auto* split_view_controller =
      SplitViewController::Get(Shell::GetPrimaryRootWindow());
  split_view_controller->SnapWindow(left_window.get(),
                                    SplitViewController::LEFT);
  split_view_controller->SnapWindow(right_window.get(),
                                    SplitViewController::RIGHT);

  // Set the screen orientation to LANDSCAPE_PRIMARY.
  test_api.SetDisplayRotation(display::Display::ROTATE_0,
                              display::Display::RotationSource::ACTIVE);
  EXPECT_EQ(test_api.GetCurrentOrientation(),
            OrientationLockType::kLandscapePrimary);

  ASSERT_EQ(right_window.get(), window_util::GetActiveWindow());
  gfx::Point start(0, 10);
  gfx::Point update_and_end(kSwipingDistanceForGoingBack + 10, 10);
  SendTouchEvent(start, ui::ET_TOUCH_PRESSED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_MOVED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_RELEASED);
  // Swiping from the left of the display in LandscapePrimary further than
  // |kSwipingDistanceForGoingBack| should activate the physically left snapped
  // window, which is |left_window| and it should go back to the previous page.
  EXPECT_EQ(left_window.get(), window_util::GetActiveWindow());
  EXPECT_EQ(1, target_back_press.accelerator_count());
  EXPECT_EQ(1, target_back_release.accelerator_count());

  gfx::Rect divider_bounds =
      split_view_controller->split_view_divider()->GetDividerBoundsInScreen(
          false);
  start = gfx::Point(divider_bounds.x(), 10);
  update_and_end =
      gfx::Point(divider_bounds.x() + kSwipingDistanceForGoingBack + 10, 10);
  SendTouchEvent(start, ui::ET_TOUCH_PRESSED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_MOVED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_RELEASED);
  // Swiping from the split view divider in LandscapePrimary further than
  // |kSwipingDistanceForGoingBack| should activate the physically right snapped
  // window, which is |right_window| and it should go back to the previous page.
  EXPECT_EQ(right_window.get(), window_util::GetActiveWindow());
  EXPECT_EQ(2, target_back_press.accelerator_count());
  EXPECT_EQ(2, target_back_release.accelerator_count());

  // Rotate the screen by 180 degree.
  test_api.SetDisplayRotation(display::Display::ROTATE_180,
                              display::Display::RotationSource::ACTIVE);
  EXPECT_EQ(test_api.GetCurrentOrientation(),
            OrientationLockType::kLandscapeSecondary);

  SendTouchEvent(start, ui::ET_TOUCH_PRESSED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_MOVED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_RELEASED);
  // Swiping from the split view divider in LandscapeSecondary further than
  // |kSwipingDistanceForGoingBack| should activate the physically right snapped
  // window, which is |left_window| and it should go back to the previous page.
  EXPECT_EQ(left_window.get(), window_util::GetActiveWindow());
  EXPECT_EQ(3, target_back_press.accelerator_count());
  EXPECT_EQ(3, target_back_release.accelerator_count());

  start = gfx::Point(0, 10);
  update_and_end = gfx::Point(kSwipingDistanceForGoingBack + 10, 10);
  SendTouchEvent(start, ui::ET_TOUCH_PRESSED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_MOVED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_RELEASED);
  // Swiping from the left of the display in LandscapeSecondary further than
  // |kSwipingDistanceForGoingBack| should activate the physically left snapped
  // window, which is |right_window| and it should go back to the previous page.
  EXPECT_EQ(right_window.get(), window_util::GetActiveWindow());
  EXPECT_EQ(4, target_back_press.accelerator_count());
  EXPECT_EQ(4, target_back_release.accelerator_count());

  // Rotate the screen by 270 degree.
  test_api.SetDisplayRotation(display::Display::ROTATE_270,
                              display::Display::RotationSource::ACTIVE);
  EXPECT_EQ(test_api.GetCurrentOrientation(),
            OrientationLockType::kPortraitPrimary);

  SendTouchEvent(start, ui::ET_TOUCH_PRESSED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_MOVED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_RELEASED);
  // Swiping from the left of the top half of the display in PortraitPrimary
  // further than |kSwipingDistanceForGoingBack| should activate the physically
  // top snapped window, which is |right_window|, and it should go back to the
  // previous page.
  EXPECT_EQ(left_window.get(), window_util::GetActiveWindow());
  EXPECT_EQ(5, target_back_press.accelerator_count());
  EXPECT_EQ(5, target_back_release.accelerator_count());

  divider_bounds =
      split_view_controller->split_view_divider()->GetDividerBoundsInScreen(
          false);
  start = gfx::Point(0, divider_bounds.bottom() + 10);
  update_and_end = gfx::Point(kSwipingDistanceForGoingBack + 10, start.y());
  SendTouchEvent(start, ui::ET_TOUCH_PRESSED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_MOVED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_RELEASED);
  // Swiping from the left of the bottom half of the display in PortraitPrimary
  // further than |kSwipingDistanceForGoingBack| should activate the physically
  // bottom snapped window, which is |right_window|, and it should go back to
  // the previous page.
  EXPECT_EQ(right_window.get(), window_util::GetActiveWindow());
  EXPECT_EQ(6, target_back_press.accelerator_count());
  EXPECT_EQ(6, target_back_release.accelerator_count());

  // Rotate the screen by 90 degree.
  test_api.SetDisplayRotation(display::Display::ROTATE_90,
                              display::Display::RotationSource::ACTIVE);
  EXPECT_EQ(test_api.GetCurrentOrientation(),
            OrientationLockType::kPortraitSecondary);

  SendTouchEvent(start, ui::ET_TOUCH_PRESSED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_MOVED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_RELEASED);
  // Swiping from the left of the bottom half of the display in
  // PortraitSecondary further than |kSwipingDistanceForGoingBack| should
  // activate the physically bottom snapped window, which is |left_window|, and
  // it should go back to the previous page.
  EXPECT_EQ(left_window.get(), window_util::GetActiveWindow());
  EXPECT_EQ(7, target_back_press.accelerator_count());
  EXPECT_EQ(7, target_back_release.accelerator_count());

  start = gfx::Point(0, 10);
  update_and_end = gfx::Point(kSwipingDistanceForGoingBack + 10, 10);
  SendTouchEvent(start, ui::ET_TOUCH_PRESSED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_MOVED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_RELEASED);
  // Swiping from the left of the top half of the display in PortraitSecondary
  // further than |kSwipingDistanceForGoingBack| should activate the physically
  // top snapped window, which is |right_window| and it should go back to the
  // previous page.
  EXPECT_EQ(right_window.get(), window_util::GetActiveWindow());
  EXPECT_EQ(8, target_back_press.accelerator_count());
  EXPECT_EQ(8, target_back_release.accelerator_count());
}

namespace {

void SendMouseReleaseAndReleaseCapture(ui::test::EventGenerator* generator,
                                       aura::Window* window) {
  generator->ReleaseLeftButton();
  window->ReleaseCapture();
}

}  // namespace

// Test that a drag is successful even if ET_MOUSE_CAPTURE_CHANGED is sent
// immediately after the mouse release. views::Widget has this behavior.
TEST_F(ToplevelWindowEventHandlerTest, CaptureLossAfterMouseRelease) {
  std::unique_ptr<aura::Window> window(CreateWindow(HTNOWHERE));
  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(),
                                     window.get());
  generator.PressLeftButton();
  window->SetCapture();

  ::wm::WindowMoveClient* move_client =
      ::wm::GetWindowMoveClient(window->GetRootWindow());
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(&SendMouseReleaseAndReleaseCapture,
                                base::Unretained(&generator),
                                base::Unretained(window.get())));
  EXPECT_EQ(::wm::MOVE_SUCCESSFUL,
            move_client->RunMoveLoop(window.get(), gfx::Vector2d(),
                                     ::wm::WINDOW_MOVE_SOURCE_MOUSE));
}

namespace {

// Checks that |window| has capture and releases capture.
void CheckHasCaptureAndReleaseCapture(aura::Window* window) {
  ASSERT_TRUE(window->HasCapture());
  window->ReleaseCapture();
}

}  // namespace

// Test that releasing capture completes an in-progress gesture drag.
TEST_F(ToplevelWindowEventHandlerTest, GestureDragCaptureLoss) {
  std::unique_ptr<aura::Window> window(CreateWindow(HTNOWHERE));
  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(),
                                     window.get());

  ::wm::WindowMoveClient* move_client =
      ::wm::GetWindowMoveClient(window->GetRootWindow());
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(&CheckHasCaptureAndReleaseCapture,
                                base::Unretained(window.get())));
  EXPECT_EQ(::wm::MOVE_SUCCESSFUL,
            move_client->RunMoveLoop(window.get(), gfx::Vector2d(),
                                     ::wm::WINDOW_MOVE_SOURCE_TOUCH));
}

// Tests that dragging a snapped window to another display updates the
// window's bounds correctly.
TEST_F(ToplevelWindowEventHandlerTest, DragSnappedWindowToExternalDisplay) {
  UpdateDisplay("940x550,940x550");
  int64_t primary_id = display::Screen::GetScreen()->GetPrimaryDisplay().id();
  int64_t secondary_id = display_manager()->GetSecondaryDisplay().id();
  display::DisplayLayoutBuilder builder(primary_id);
  builder.SetSecondaryPlacement(secondary_id, display::DisplayPlacement::TOP,
                                0);
  display_manager()->SetLayoutForCurrentDisplays(builder.Build());

  const gfx::Size initial_window_size(330, 230);
  std::unique_ptr<aura::Window> w1(CreateTestWindowInShellWithDelegateAndType(
      new TestWindowDelegate(HTCAPTION), aura::client::WINDOW_TYPE_NORMAL, 0,
      gfx::Rect(initial_window_size)));

  // Snap the window to the right.
  WindowState* window_state = WindowState::Get(w1.get());
  ASSERT_TRUE(window_state->CanSnap());
  const WMEvent event(WM_EVENT_CYCLE_SNAP_RIGHT);
  window_state->OnWMEvent(&event);
  ASSERT_TRUE(window_state->IsSnapped());

  // Drag the window to the secondary display.
  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(), w1.get());
  generator.DragMouseTo(472, -462);

  // Expect the window is no longer snapped and its size was restored to the
  // initial size.
  EXPECT_FALSE(window_state->IsSnapped());
  EXPECT_EQ(initial_window_size.ToString(), w1->bounds().size().ToString());

  // The window is now fully contained in the secondary display.
  EXPECT_TRUE(display_manager()->GetSecondaryDisplay().bounds().Contains(
      w1->GetBoundsInScreen()));
}

TEST_F(ToplevelWindowEventHandlerTest, MoveDoesntEnterResizeLoop) {
  std::unique_ptr<aura::Window> w1(CreateWindow(HTCAPTION));
  ResizeLoopWindowObserver window_observer(w1.get());
  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(), w1.get());
  // A click on the caption does not trigger the resize loop.
  generator.PressLeftButton();
  EXPECT_FALSE(window_observer.in_resize_loop());

  // A move in the caption does not trigger the resize loop either.
  generator.MoveMouseBy(100, 100);
  EXPECT_FALSE(window_observer.in_resize_loop());
  w1->RemoveObserver(&window_observer);
}

TEST_F(ToplevelWindowEventHandlerTest, EnterResizeLoopOnResize) {
  std::unique_ptr<aura::Window> w1(CreateWindow(HTGROWBOX));
  ResizeLoopWindowObserver window_observer(w1.get());
  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow(), w1.get());
  // The resize loop is entered once a possible resize is detected.
  generator.PressLeftButton();
  EXPECT_TRUE(window_observer.in_resize_loop());

  // Should remain in the resize loop while dragging.
  generator.MoveMouseBy(100, 100);
  EXPECT_TRUE(window_observer.in_resize_loop());

  // Releasing the button should end the loop.
  generator.ReleaseLeftButton();
  EXPECT_FALSE(window_observer.in_resize_loop());
}

// Provides common setup and convenience for a handful of tests.
class ToplevelWindowEventHandlerDragTest : public AshTestBase {
 public:
  ToplevelWindowEventHandlerDragTest() = default;
  ~ToplevelWindowEventHandlerDragTest() override = default;

  void SetUp() override {
    AshTestBase::SetUp();

    dragged_window_ = CreateTestWindow();
    non_dragged_window_ = CreateTestWindow();
    dragged_window_->SetProperty(aura::client::kAppType,
                                 static_cast<int>(AppType::CHROME_APP));
  }

  void TearDown() override {
    non_dragged_window_.reset();
    dragged_window_.reset();
    AshTestBase::TearDown();
  }

 protected:
  // Send gesture event with |type| to the toplevel window event handler.
  void SendGestureEvent(const gfx::Point& position,
                        int scroll_x,
                        int scroll_y,
                        ui::EventType type) {
    ui::GestureEvent event = ui::GestureEvent(
        position.x(), position.y(), ui::EF_NONE, base::TimeTicks::Now(),
        ui::GestureEventDetails(type, scroll_x, scroll_y));
    ui::Event::DispatcherApi(&event).set_target(dragged_window_.get());
    ash::Shell::Get()->toplevel_window_event_handler()->OnGestureEvent(&event);
  }

  std::unique_ptr<aura::Window> dragged_window_;
  std::unique_ptr<aura::Window> non_dragged_window_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ToplevelWindowEventHandlerDragTest);
};

// Tests that tap the window in overview grid during window drag should end
// the overview mode.
TEST_F(ToplevelWindowEventHandlerDragTest,
       TapWindowInOverviewGridDuringWindowDrag) {
  TabletModeControllerTestApi().EnterTabletMode();

  SendGestureEvent(gfx::Point(100, 0), 0, 5, ui::ET_GESTURE_SCROLL_BEGIN);
  // Drag the window to the right corner to avoid overlap with
  // |non_dragged_window_| in overview grid.
  SendGestureEvent(gfx::Point(600, 500), 600, 500,
                   ui::ET_GESTURE_SCROLL_UPDATE);
  EXPECT_TRUE(WindowState::Get(dragged_window_.get())->is_dragged());

  OverviewController* overview_controller = Shell::Get()->overview_controller();
  EXPECT_TRUE(overview_controller->InOverviewSession());
  EXPECT_TRUE(overview_controller->overview_session()->IsWindowInOverview(
      non_dragged_window_.get()));

  OverviewGrid* current_grid =
      overview_controller->overview_session()->GetGridWithRootWindow(
          non_dragged_window_->GetRootWindow());
  OverviewItem* item =
      current_grid->GetOverviewItemContaining(non_dragged_window_.get());
  GetEventGenerator()->GestureTapAt(
      gfx::ToRoundedPoint(item->GetTransformedBounds().CenterPoint()));

  // Overview mode is no longer active and |non_dragged_window_| is not in the
  // overview grid after tapping it in overview grid.
  EXPECT_FALSE(overview_controller->InOverviewSession());
  EXPECT_FALSE(overview_controller->overview_session());
}

// In tablet mode, the window's resizability shouldn't be taken into account
// when dragging from the top. Regression test for https://crbug.com/1444132
TEST_F(ToplevelWindowEventHandlerDragTest,
       NonResizableWindowsCanBeDraggedInTabletMode) {
  TabletModeControllerTestApi().EnterTabletMode();

  dragged_window_->SetProperty(aura::client::kResizeBehaviorKey,
                               aura::client::kResizeBehaviorNone);

  SendGestureEvent(gfx::Point(100, 0), 0, 5, ui::ET_GESTURE_SCROLL_BEGIN);
  SendGestureEvent(gfx::Point(600, 500), 600, 500,
                   ui::ET_GESTURE_SCROLL_UPDATE);
  EXPECT_TRUE(WindowState::Get(dragged_window_.get())->is_dragged());

  OverviewController* overview_controller = Shell::Get()->overview_controller();
  EXPECT_TRUE(overview_controller->InOverviewSession());
  EXPECT_TRUE(overview_controller->overview_session()->IsWindowInOverview(
      non_dragged_window_.get()));
}

// Contrary to tablet mode, in non-tablet mode, non resizable windows cannot be
// dragged.
TEST_F(ToplevelWindowEventHandlerDragTest,
       NonResizableWindowsCannotBeDraggedInClamshellMode) {
  ASSERT_FALSE(TabletModeControllerTestApi().IsTabletModeStarted());

  dragged_window_->SetProperty(aura::client::kResizeBehaviorKey,
                               aura::client::kResizeBehaviorNone);

  SendGestureEvent(gfx::Point(0, 0), 0, 5, ui::ET_GESTURE_SCROLL_BEGIN);
  SendGestureEvent(gfx::Point(700, 500), 700, 500,
                   ui::ET_GESTURE_SCROLL_UPDATE);
  EXPECT_FALSE(WindowState::Get(dragged_window_.get())->is_dragged());

  OverviewController* overview_controller = Shell::Get()->overview_controller();
  EXPECT_FALSE(overview_controller->InOverviewSession());
}

// Tests that the window drag will be reverted if the screen is being rotated.
TEST_F(ToplevelWindowEventHandlerDragTest, DisplayConfigurationChangeTest) {
  TabletModeControllerTestApi().EnterTabletMode();

  int64_t display_id = display::Screen::GetScreen()->GetPrimaryDisplay().id();
  display::DisplayManager* display_manager = Shell::Get()->display_manager();
  display::test::ScopedSetInternalDisplayId set_internal(display_manager,
                                                         display_id);
  ScreenOrientationControllerTestApi test_api(
      Shell::Get()->screen_orientation_controller());
  // Set the screen orientation to LANDSCAPE_PRIMARY.
  test_api.SetDisplayRotation(display::Display::ROTATE_0,
                              display::Display::RotationSource::ACTIVE);
  EXPECT_EQ(test_api.GetCurrentOrientation(),
            OrientationLockType::kLandscapePrimary);

  ASSERT_TRUE(TabletModeControllerTestApi().IsTabletModeStarted());

  SendGestureEvent(gfx::Point(100, 0), 0, 5, ui::ET_GESTURE_SCROLL_BEGIN);
  // Drag the window to the right corner to avoid overlap with
  // |non_dragged_window_| in overview grid.
  SendGestureEvent(gfx::Point(600, 500), 600, 500,
                   ui::ET_GESTURE_SCROLL_UPDATE);
  EXPECT_TRUE(WindowState::Get(dragged_window_.get())->is_dragged());

  OverviewController* overview_controller = Shell::Get()->overview_controller();
  EXPECT_TRUE(overview_controller->InOverviewSession());
  EXPECT_TRUE(overview_controller->overview_session()->IsWindowInOverview(
      non_dragged_window_.get()));

  // Rotate the screen during drag.
  test_api.SetDisplayRotation(display::Display::ROTATE_270,
                              display::Display::RotationSource::ACTIVE);
  EXPECT_EQ(test_api.GetCurrentOrientation(),
            OrientationLockType::kPortraitPrimary);
  EXPECT_TRUE(WindowState::Get(dragged_window_.get())->IsMaximized());
  EXPECT_FALSE(overview_controller->InOverviewSession());
  EXPECT_FALSE(WindowState::Get(dragged_window_.get())->is_dragged());
}

// Showing the resize shadows when the mouse is over the window edges is
// tested in resize_shadow_and_cursor_test.cc

}  // namespace ash
