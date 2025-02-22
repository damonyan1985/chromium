// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/wm/tablet_mode/tablet_mode_window_manager.h"

#include <memory>

#include "ash/public/cpp/app_types.h"
#include "ash/public/cpp/ash_features.h"
#include "ash/public/cpp/ash_switches.h"
#include "ash/public/cpp/shell_window_ids.h"
#include "ash/public/cpp/window_properties.h"
#include "ash/root_window_controller.h"
#include "ash/scoped_animation_disabler.h"
#include "ash/screen_util.h"
#include "ash/session/session_controller_impl.h"
#include "ash/shell.h"
#include "ash/shell_delegate.h"
#include "ash/wm/desks/desks_util.h"
#include "ash/wm/mru_window_tracker.h"
#include "ash/wm/overview/overview_controller.h"
#include "ash/wm/overview/overview_session.h"
#include "ash/wm/overview/overview_utils.h"
#include "ash/wm/splitview/split_view_controller.h"
#include "ash/wm/splitview/split_view_utils.h"
#include "ash/wm/tablet_mode/scoped_skip_user_session_blocked_check.h"
#include "ash/wm/tablet_mode/tablet_mode_event_handler.h"
#include "ash/wm/tablet_mode/tablet_mode_window_state.h"
#include "ash/wm/window_state.h"
#include "ash/wm/wm_event.h"
#include "ash/wm/workspace/backdrop_controller.h"
#include "ash/wm/workspace/workspace_layout_manager.h"
#include "ash/wm/workspace_controller.h"
#include "base/command_line.h"
#include "base/stl_util.h"
#include "ui/aura/client/aura_constants.h"
#include "ui/display/screen.h"

namespace ash {

namespace {

// This function is called to check if window[i] is eligible to be carried over
// to split view mode during clamshell <-> tablet mode transition or multi-user
// switch transition. Returns true if windows[i] exists, is on |root_window|,
// and can snap in split view on |root_window|.
bool IsCarryOverCandidateForSplitView(
    const MruWindowTracker::WindowList& windows,
    size_t i,
    aura::Window* root_window) {
  return windows.size() > i && windows[i]->GetRootWindow() == root_window &&
         SplitViewController::Get(root_window)->CanSnapWindow(windows[i]);
}

// Returns the windows that are going to be carried over to splitview during
// clamshell <-> tablet transition or multi user switch transition.
// TODO(xdai): Return eligible windows regardless of window zorders.
base::flat_map<aura::Window*, WindowStateType>
GetCarryOverWindowsInSplitView() {
  base::flat_map<aura::Window*, WindowStateType> windows;
  // Check the states of the topmost two non-overview windows to see if they are
  // eligible to be carried over to splitscreen. A window must meet
  // IsCarryOverCandidateForSplitView() to be carried over to splitscreen.
  MruWindowTracker::WindowList mru_windows =
      Shell::Get()->mru_window_tracker()->BuildWindowForCycleList(kActiveDesk);
  mru_windows.erase(
      std::remove_if(mru_windows.begin(), mru_windows.end(),
                     [](aura::Window* window) {
                       return window->GetProperty(kIsShowingInOverviewKey);
                     }),
      mru_windows.end());
  aura::Window* root_window = Shell::GetPrimaryRootWindow();
  if (IsCarryOverCandidateForSplitView(mru_windows, 0u, root_window)) {
    if (WindowState::Get(mru_windows[0])->GetStateType() ==
        WindowStateType::kLeftSnapped) {
      windows.emplace(mru_windows[0], WindowStateType::kLeftSnapped);
      if (IsCarryOverCandidateForSplitView(mru_windows, 1u, root_window) &&
          WindowState::Get(mru_windows[1])->GetStateType() ==
              WindowStateType::kRightSnapped) {
        windows.emplace(mru_windows[1], WindowStateType::kRightSnapped);
      }
    } else if (WindowState::Get(mru_windows[0])->GetStateType() ==
               WindowStateType::kRightSnapped) {
      windows.emplace(mru_windows[0], WindowStateType::kRightSnapped);
      if (IsCarryOverCandidateForSplitView(mru_windows, 1u, root_window) &&
          WindowState::Get(mru_windows[1])->GetStateType() ==
              WindowStateType::kLeftSnapped) {
        windows.emplace(mru_windows[1], WindowStateType::kLeftSnapped);
      }
    }
  }
  return windows;
}

// Calculates the divider position of the splitscreen based on the snapped
// window(s)'s positions. We'll try to keep the current snapped window(s)'
// bounds as much as possible.
int CalculateCarryOverDividerPostion(
    base::flat_map<aura::Window*, WindowStateType> windows_in_splitview) {
  aura::Window* left_window = nullptr;
  aura::Window* right_window = nullptr;
  for (auto& iter : windows_in_splitview) {
    if (iter.second == WindowStateType::kLeftSnapped)
      left_window = iter.first;
    else if (iter.second == WindowStateType::kRightSnapped)
      right_window = iter.first;
  }
  if (!left_window && !right_window)
    return -1;

  gfx::Rect work_area =
      display::Screen::GetScreen()
          ->GetDisplayNearestWindow(left_window ? left_window : right_window)
          .work_area();
  gfx::Rect left_window_bounds =
      left_window ? left_window->GetBoundsInScreen() : gfx::Rect();
  gfx::Rect right_window_bounds =
      right_window ? right_window->GetBoundsInScreen() : gfx::Rect();

  if (SplitViewController::IsLayoutHorizontal()) {
    if (SplitViewController::IsLayoutRightSideUp()) {
      return left_window ? left_window_bounds.width()
                         : work_area.width() - right_window_bounds.width();
    } else {
      return left_window ? work_area.width() - left_window_bounds.width()
                         : right_window_bounds.width();
    }
  } else {
    if (SplitViewController::IsLayoutRightSideUp()) {
      return left_window ? left_window_bounds.height()
                         : work_area.height() - right_window_bounds.height();
    } else {
      return left_window ? work_area.height() - left_window_bounds.height()
                         : right_window_bounds.height();
    }
  }
}

// Snap the carry over windows into splitview mode at |divider_position|.
void DoSplitViewTransition(
    base::flat_map<aura::Window*, WindowStateType> windows,
    int divider_position) {
  if (windows.empty())
    return;

  SplitViewController* split_view_controller =
      SplitViewController::Get(Shell::GetPrimaryRootWindow());
  // If split view mode is already active, use its own divider position.
  if (!split_view_controller->InSplitViewMode())
    split_view_controller->InitDividerPositionForTransition(divider_position);

  for (auto& iter : windows) {
    split_view_controller->SnapWindow(
        iter.first, iter.second == WindowStateType::kLeftSnapped
                        ? SplitViewController::LEFT
                        : SplitViewController::RIGHT);
  }

  // For clamshell split view mode, end splitview mode if we're in single
  // split mode or both snapped mode (in both cases overview is not active).
  // TODO(xdai): Refactoring SplitViewController to make SplitViewController to
  // handle this case.
  if (split_view_controller->InClamshellSplitViewMode() &&
      !Shell::Get()->overview_controller()->InOverviewSession()) {
    split_view_controller->EndSplitView();
  }
}

void UpdateDeskContainersBackdrops() {
  for (auto* root : Shell::GetAllRootWindows()) {
    for (auto* desk_container : desks_util::GetDesksContainers(root)) {
      WorkspaceController* controller = GetWorkspaceController(desk_container);
      WorkspaceLayoutManager* layout_manager = controller->layout_manager();
      BackdropController* backdrop_controller =
          layout_manager->backdrop_controller();
      backdrop_controller->UpdateBackdrop();
    }
  }
}

}  // namespace

// Class which tells tablet mode controller to observe a given window for UMA
// logging purposes. Created before the window animations start. When this goes
// out of scope and the given window is not actually animating, tells tablet
// mode controller to stop observing.
class ScopedObserveWindowAnimation {
 public:
  ScopedObserveWindowAnimation(aura::Window* window,
                               TabletModeWindowManager* manager,
                               bool exiting_tablet_mode)
      : window_(window),
        manager_(manager),
        exiting_tablet_mode_(exiting_tablet_mode) {
    if (Shell::Get()->tablet_mode_controller() && window_) {
      Shell::Get()->tablet_mode_controller()->MaybeObserveBoundsAnimation(
          window_);
    }
  }
  ~ScopedObserveWindowAnimation() {
    // May be null on shutdown.
    if (!Shell::Get()->tablet_mode_controller())
      return;

    if (!window_)
      return;

    // Stops observing if |window_| is not animating, or if it is not tracked by
    // TabletModeWindowManager. When this object is destroyed while exiting
    // tablet mode, |window_| is no longer tracked, so skip that check.
    if (window_->layer()->GetAnimator()->is_animating() &&
        (exiting_tablet_mode_ || manager_->IsTrackingWindow(window_))) {
      return;
    }

    Shell::Get()->tablet_mode_controller()->StopObservingAnimation(
        /*record_stats=*/false, /*delete_screenshot=*/true);
  }

 private:
  aura::Window* window_;
  TabletModeWindowManager* manager_;
  bool exiting_tablet_mode_;
  DISALLOW_COPY_AND_ASSIGN(ScopedObserveWindowAnimation);
};

TabletModeWindowManager::TabletModeWindowManager() = default;

TabletModeWindowManager::~TabletModeWindowManager() = default;

// static
aura::Window* TabletModeWindowManager::GetTopWindow() {
  MruWindowTracker::WindowList windows =
      Shell::Get()->mru_window_tracker()->BuildWindowForCycleList(kActiveDesk);

  return windows.empty() ? nullptr : windows[0];
}

// static
bool TabletModeWindowManager::ShouldMinimizeTopWindowOnBack() {
  if (!features::IsSwipingFromLeftEdgeToGoBackEnabled())
    return false;

  Shell* shell = Shell::Get();
  if (!shell->tablet_mode_controller()->InTabletMode())
    return false;

  aura::Window* window = GetTopWindow();
  if (!window)
    return false;

  // Do not minimize the window if it is in overview. This can avoid unnecessary
  // window minimize animation.
  OverviewController* overview_controller = Shell::Get()->overview_controller();
  if (overview_controller->InOverviewSession() &&
      overview_controller->overview_session()->IsWindowInOverview(window)) {
    return false;
  }

  const int app_type = window->GetProperty(aura::client::kAppType);
  if (app_type != static_cast<int>(AppType::BROWSER) &&
      app_type != static_cast<int>(AppType::CHROME_APP)) {
    return false;
  }

  WindowState* window_state = WindowState::Get(window);
  if (!window_state || !window_state->CanMinimize() ||
      window_state->IsMinimized()) {
    return false;
  }

  // Minimize the window if it is at the bottom page.
  return !shell->shell_delegate()->CanGoBack(window);
}

void TabletModeWindowManager::Init() {
  {
    ScopedObserveWindowAnimation scoped_observe(GetTopWindow(), this,
                                                /*exiting_tablet_mode=*/false);
    ArrangeWindowsForTabletMode();
  }
  AddWindowCreationObservers();
  display::Screen::GetScreen()->AddObserver(this);
  SplitViewController::Get(Shell::GetPrimaryRootWindow())->AddObserver(this);
  Shell::Get()->session_controller()->AddObserver(this);
  Shell::Get()->overview_controller()->AddObserver(this);
  accounts_since_entering_tablet_.insert(
      Shell::Get()->session_controller()->GetActiveAccountId());
  event_handler_ = std::make_unique<TabletModeEventHandler>();
}

void TabletModeWindowManager::Shutdown() {
  SplitViewController* split_view_controller =
      SplitViewController::Get(Shell::GetPrimaryRootWindow());
  base::flat_map<aura::Window*, WindowStateType> carryover_windows_in_splitview;
  const bool was_in_overview =
      Shell::Get()->overview_controller()->InOverviewSession();
  // If clamshell split view mode is not enabled, still keep the old behavior:
  // End overview if overview is active and restore all windows' window states
  // to their previous window states.
  if (!IsClamshellSplitViewModeEnabled()) {
    Shell::Get()->overview_controller()->EndOverview();
  } else {
    // If clamshell split view mode is enabled, there are 4 cases when exiting
    // tablet mode:
    // 1) overview is active but split view is inactive: keep overview active in
    //    clamshell mode.
    // 2) overview and splitview are both active: keep overview and splitview
    // both
    //    active in clamshell mode, unless if it's single split state, splitview
    //    and overview will both be ended.
    // 3) overview is inactive but split view is active (two snapped windows):
    //    split view is no longer active. But the two snapped windows will still
    //    keep snapped in clamshell mode.
    // 4) overview and splitview are both inactive: keep the current behavior,
    //    i.e., restore all windows to its window state before entering tablet
    //    mode.

    // TODO(xdai): Instead of caching snapped windows and their state here, we
    // should try to see if it can be done in the WindowState::State impl.
    carryover_windows_in_splitview = GetCarryOverWindowsInSplitView();

    // For case 2 and 3: End splitview mode for two snapped windows case or
    // single split case to match the clamshell split view behavior. (there is
    // no both snapped state or single split state in clamshell split view). The
    // windows will still be kept snapped though.
    if (split_view_controller->InSplitViewMode()) {
      OverviewController* overview_controller =
          Shell::Get()->overview_controller();
      if (!overview_controller->InOverviewSession() ||
          overview_controller->overview_session()->IsEmpty()) {
        split_view_controller->EndSplitView(
            SplitViewController::EndReason::kExitTabletMode);
        overview_controller->EndOverview();
      }
    }
  }

  for (aura::Window* window : added_windows_)
    window->RemoveObserver(this);
  added_windows_.clear();
  split_view_controller->RemoveObserver(this);
  Shell::Get()->session_controller()->RemoveObserver(this);
  Shell::Get()->overview_controller()->RemoveObserver(this);
  display::Screen::GetScreen()->RemoveObserver(this);
  RemoveWindowCreationObservers();

  ScopedObserveWindowAnimation scoped_observe(GetTopWindow(), this,
                                              /*exiting_tablet_mode=*/true);
  ArrangeWindowsForClamshellMode(carryover_windows_in_splitview,
                                 was_in_overview);
}

int TabletModeWindowManager::GetNumberOfManagedWindows() {
  return window_state_map_.size();
}

bool TabletModeWindowManager::IsTrackingWindow(aura::Window* window) {
  return base::Contains(window_state_map_, window);
}

void TabletModeWindowManager::AddWindow(aura::Window* window) {
  // Only add the window if it is a direct dependent of a container window
  // and not yet tracked.
  if (IsTrackingWindow(window) || !IsContainerWindow(window->parent())) {
    return;
  }

  TrackWindow(window);
}

void TabletModeWindowManager::WindowStateDestroyed(aura::Window* window) {
  // We come here because the tablet window state object was destroyed. It was
  // destroyed either because ForgetWindow() was called, or because its
  // associated window was destroyed. In both cases, the window must has removed
  // TabletModeWindowManager as an observer.
  DCHECK(!window->HasObserver(this));

  // The window state object might have been removed in OnWindowDestroying().
  auto it = window_state_map_.find(window);
  if (it != window_state_map_.end())
    window_state_map_.erase(it);
}

void TabletModeWindowManager::SetIgnoreWmEventsForExit() {
  is_exiting_ = true;
  for (auto& pair : window_state_map_)
    pair.second->set_ignore_wm_events(true);
}

void TabletModeWindowManager::StopWindowAnimations() {
  for (auto& pair : window_state_map_)
    pair.first->layer()->GetAnimator()->StopAnimating();
}

void TabletModeWindowManager::OnOverviewModeEndingAnimationComplete(
    bool canceled) {
  if (canceled)
    return;

  SplitViewController* split_view_controller =
      SplitViewController::Get(Shell::GetPrimaryRootWindow());

  // Maximize all snapped windows upon exiting overview mode except snapped
  // windows in splitview mode. Note the snapped window might not be tracked in
  // our |window_state_map_|.
  // Leave snapped windows on inactive desks unchanged.
  const MruWindowTracker::WindowList windows =
      Shell::Get()->mru_window_tracker()->BuildWindowListIgnoreModal(
          kActiveDesk);
  for (auto* window : windows) {
    if (split_view_controller->left_window() != window &&
        split_view_controller->right_window() != window) {
      MaximizeIfSnapped(window);
    }
  }
}

void TabletModeWindowManager::OnSplitViewStateChanged(
    SplitViewController::State previous_state,
    SplitViewController::State state) {
  // All TabletModeWindowState will ignore further WMEvents, but we still have
  // to manually prevent sending maximizing events to ClientControlledState ARC
  // windows e.g. ARC apps.
  if (is_exiting_)
    return;

  if (state != SplitViewController::State::kNoSnap)
    return;
  switch (
      SplitViewController::Get(Shell::GetPrimaryRootWindow())->end_reason()) {
    case SplitViewController::EndReason::kNormal:
    case SplitViewController::EndReason::kUnsnappableWindowActivated:
      break;
    case SplitViewController::EndReason::kHomeLauncherPressed:
    case SplitViewController::EndReason::kActiveUserChanged:
    case SplitViewController::EndReason::kWindowDragStarted:
    case SplitViewController::EndReason::kExitTabletMode:
    case SplitViewController::EndReason::kDesksChange:
      // For the case of kHomeLauncherPressed, the home launcher will minimize
      // the snapped windows after ending splitview, so avoid maximizing them
      // here. For the case of kActiveUserChanged, the snapped windows will be
      // used to restore the splitview layout when switching back, and it is
      // already too late to maximize them anyway (the for loop below would
      // iterate over windows in the newly activated user session).
      return;
  }

  // Maximize all snapped windows upon exiting split view mode. Note the snapped
  // window might not be tracked in our |window_state_map_|.
  // Leave snapped windows on inactive desks unchanged.
  const MruWindowTracker::WindowList windows =
      Shell::Get()->mru_window_tracker()->BuildWindowListIgnoreModal(
          kActiveDesk);
  for (auto* window : windows)
    MaximizeIfSnapped(window);
}

void TabletModeWindowManager::OnWindowDestroying(aura::Window* window) {
  if (IsContainerWindow(window)) {
    // container window can be removed on display destruction.
    window->RemoveObserver(this);
    observed_container_windows_.erase(window);
  } else if (base::Contains(added_windows_, window)) {
    // Added window was destroyed before being shown.
    added_windows_.erase(window);
    window->RemoveObserver(this);
  } else {
    // If a known window gets destroyed we need to remove all knowledge about
    // it.
    ForgetWindow(window, /*destroyed=*/true);
  }
}

void TabletModeWindowManager::OnWindowHierarchyChanged(
    const HierarchyChangeParams& params) {
  // A window can get removed and then re-added by a drag and drop operation.
  if (params.new_parent && IsContainerWindow(params.new_parent) &&
      !base::Contains(window_state_map_, params.target)) {
    // Don't register the window if the window is invisible. Instead,
    // wait until it becomes visible because the client may update the
    // flag to control if the window should be added.
    if (!params.target->IsVisible()) {
      if (!base::Contains(added_windows_, params.target)) {
        added_windows_.insert(params.target);
        params.target->AddObserver(this);
      }
      return;
    }
    TrackWindow(params.target);
    // When the state got added, the "WM_EVENT_ADDED_TO_WORKSPACE" event got
    // already sent and we have to notify our state again.
    if (base::Contains(window_state_map_, params.target)) {
      WMEvent event(WM_EVENT_ADDED_TO_WORKSPACE);
      WindowState::Get(params.target)->OnWMEvent(&event);
    }
  }
}

void TabletModeWindowManager::OnWindowPropertyChanged(aura::Window* window,
                                                      const void* key,
                                                      intptr_t old) {
  // Stop managing |window| if it is moved to have a non-normal z-order.
  if (key == aura::client::kZOrderingKey &&
      window->GetProperty(aura::client::kZOrderingKey) !=
          ui::ZOrderLevel::kNormal) {
    ForgetWindow(window, false /* destroyed */);
  }
}

void TabletModeWindowManager::OnWindowBoundsChanged(
    aura::Window* window,
    const gfx::Rect& old_bounds,
    const gfx::Rect& new_bounds,
    ui::PropertyChangeReason reason) {
  if (!IsContainerWindow(window))
    return;

  auto* session = Shell::Get()->overview_controller()->overview_session();
  if (session)
    session->SuspendReposition();

  // Reposition all non maximizeable windows.
  for (auto& pair : window_state_map_) {
    pair.second->UpdateWindowPosition(WindowState::Get(pair.first),
                                      /*animate=*/false);
  }
  if (session)
    session->ResumeReposition();
}

void TabletModeWindowManager::OnWindowVisibilityChanged(aura::Window* window,
                                                        bool visible) {
  // Skip if it's already managed.
  if (IsTrackingWindow(window))
    return;

  if (IsContainerWindow(window->parent()) &&
      base::Contains(added_windows_, window) && visible) {
    added_windows_.erase(window);
    window->RemoveObserver(this);
    TrackWindow(window);
    // When the state got added, the "WM_EVENT_ADDED_TO_WORKSPACE" event got
    // already sent and we have to notify our state again.
    if (IsTrackingWindow(window)) {
      WMEvent event(WM_EVENT_ADDED_TO_WORKSPACE);
      WindowState::Get(window)->OnWMEvent(&event);
    }
  }
}

void TabletModeWindowManager::OnDisplayAdded(const display::Display& display) {
  DisplayConfigurationChanged();
}

void TabletModeWindowManager::OnDisplayRemoved(
    const display::Display& display) {
  DisplayConfigurationChanged();
}

void TabletModeWindowManager::OnActiveUserSessionChanged(
    const AccountId& account_id) {
  SplitViewController* split_view_controller =
      SplitViewController::Get(Shell::GetPrimaryRootWindow());

  // There is only one SplitViewController object for all user sessions, but
  // functionally, each user session independently can be in split view or not.
  // Here, a new user session has just been switched to, and if split view mode
  // is active then it was for the previous user session.
  // SplitViewController::EndSplitView() will perform some cleanup, including
  // setting |SplitViewController::left_window_| and
  // |SplitViewController::right_window_| to null, but the aura::Window objects
  // will be left unchanged to facilitate switching back.
  split_view_controller->EndSplitView(
      SplitViewController::EndReason::kActiveUserChanged);

  // If a user session is now active for the first time since clamshell mode,
  // then do the logic for carrying over snapped windows. Else recreate the
  // split view layout from the last time the current user session was active.
  bool refresh_snapped_windows = false;
  if (accounts_since_entering_tablet_.count(account_id) == 0u) {
    base::flat_map<aura::Window*, WindowStateType> windows_in_splitview =
        GetCarryOverWindowsInSplitView();
    int divider_position =
        CalculateCarryOverDividerPostion(windows_in_splitview);
    DoSplitViewTransition(windows_in_splitview, divider_position);
    accounts_since_entering_tablet_.insert(account_id);
  } else {
    refresh_snapped_windows = true;
  }

  MaybeRestoreSplitView(refresh_snapped_windows);
}

WindowStateType TabletModeWindowManager::GetDesktopWindowStateType(
    aura::Window* window) const {
  auto iter = window_state_map_.find(window);
  return iter == window_state_map_.end()
             ? WindowState::Get(window)->GetStateType()
             : iter->second->old_state()->GetType();
}

void TabletModeWindowManager::ArrangeWindowsForTabletMode() {
  // If clamshell split view mode is not enabled, still keep the old behavior:
  // end overview if it's active. And carry over snapped windows to
  // splitscreen if possible.
  if (!IsClamshellSplitViewModeEnabled())
    Shell::Get()->overview_controller()->EndOverview();

  // If clamshell splitview mode is enabled, there are 3 cases when entering
  // tablet mode:
  // 1) overview is active but split view is inactive: keep overview active in
  //    tablet mode.
  // 2) overview and splitview are both active (splitview can only be active
  //    when overview is active in clamshell mode): keep overview and splitview
  //    both active in tablet mode.
  // 3) overview is inactive: keep the current behavior, i.e.,
  //    a. if the top window is a snapped window, put it in splitview
  //    b. if the second top window is also a snapped window and snapped to
  //       the other side, put it in split view as well. Otherwise, open
  //       overview on the other side of the screen
  //    c. if the top window is not a snapped window, maximize all windows
  //       when entering tablet mode.

  // |activatable_windows| includes all windows to be tracked, and that includes
  // windows on the lock screen via |scoped_skip_user_session_blocked_check|.
  ScopedSkipUserSessionBlockedCheck scoped_skip_user_session_blocked_check;
  MruWindowTracker::WindowList activatable_windows =
      Shell::Get()->mru_window_tracker()->BuildWindowListIgnoreModal(kAllDesks);

  // Determine which windows are to be carried over to splitview from clamshell
  // mode to tablet mode.
  base::flat_map<aura::Window*, WindowStateType> windows_in_splitview =
      GetCarryOverWindowsInSplitView();
  int divider_position = CalculateCarryOverDividerPostion(windows_in_splitview);

  // If split view is not appropriate, then maximize all windows and bail out.
  if (windows_in_splitview.empty()) {
    for (auto* window : activatable_windows)
      TrackWindow(window, /*entering_tablet_mode=*/true);
    return;
  }

  // Carry over the state types of the windows that shall be in split view.
  // Maximize all other windows. Do not animate any window bounds updates.
  for (auto* window : activatable_windows) {
    bool snap = false;
    for (auto& iter : windows_in_splitview) {
      if (window == iter.first) {
        snap = true;
        break;
      }
    }
    TrackWindow(window, /*entering_tablet_mode=*/true, snap,
                /*animate_bounds_on_attach=*/false);
  }

  // Do split view mode transition.
  DoSplitViewTransition(windows_in_splitview, divider_position);
}

void TabletModeWindowManager::ArrangeWindowsForClamshellMode(
    base::flat_map<aura::Window*, WindowStateType> windows_in_splitview,
    bool was_in_overview) {
  int divider_position = CalculateCarryOverDividerPostion(windows_in_splitview);

  while (window_state_map_.size()) {
    aura::Window* window = window_state_map_.begin()->first;
    ForgetWindow(window, /*destroyed=*/false, was_in_overview);
  }

  if (IsClamshellSplitViewModeEnabled()) {
    // Arriving here the window state has changed to its clamshell window state.
    // Since we need to keep the windows that were in splitview still be snapped
    // in clamshell mode, change its window state to the corresponding snapped
    // window state.
    DoSplitViewTransition(windows_in_splitview, divider_position);
  }
}

void TabletModeWindowManager::TrackWindow(aura::Window* window,
                                          bool entering_tablet_mode,
                                          bool snap,
                                          bool animate_bounds_on_attach) {
  if (!ShouldHandleWindow(window))
    return;

  DCHECK(!IsTrackingWindow(window));
  window->AddObserver(this);

  // Create and remember a tablet mode state which will attach itself to the
  // provided state object.
  window_state_map_.emplace(
      window,
      new TabletModeWindowState(window, this, snap, animate_bounds_on_attach,
                                entering_tablet_mode));
}

void TabletModeWindowManager::ForgetWindow(aura::Window* window,
                                           bool destroyed,
                                           bool was_in_overview) {
  added_windows_.erase(window);
  window->RemoveObserver(this);

  WindowToState::iterator it = window_state_map_.find(window);
  // A window may not be registered yet if the observer was
  // registered in OnWindowHierarchyChanged.
  if (it == window_state_map_.end())
    return;

  if (destroyed) {
    // If the window is to-be-destroyed, remove it from |window_state_map_|
    // immidietely. Otherwise it's possible to send a WMEvent to the to-be-
    // destroyed window.  Note we should not restore its old previous window
    // state object here since it will send unnecessary window state change
    // events. The tablet window state object and the old window state object
    // will be both deleted when the window is destroyed.
    window_state_map_.erase(it);
  } else {
    // By telling the state object to revert, it will switch back the old
    // State object and destroy itself, calling WindowStateDestroyed().
    it->second->LeaveTabletMode(WindowState::Get(it->first), was_in_overview);
    DCHECK(!IsTrackingWindow(window));
  }
}

bool TabletModeWindowManager::ShouldHandleWindow(aura::Window* window) {
  DCHECK(window);

  // Windows that don't have normal z-ordering should be free-floating and thus
  // not managed by us.
  if (window->GetProperty(aura::client::kZOrderingKey) !=
      ui::ZOrderLevel::kNormal) {
    return false;
  }

  // If the changing bounds in the maximized/fullscreen is allowed, then
  // let the client manage it even in tablet mode.
  if (!WindowState::Get(window) ||
      WindowState::Get(window)->allow_set_bounds_direct()) {
    return false;
  }

  return window->type() == aura::client::WINDOW_TYPE_NORMAL;
}

void TabletModeWindowManager::AddWindowCreationObservers() {
  DCHECK(observed_container_windows_.empty());
  // Observe window activations/creations in the default containers on all root
  // windows.
  for (aura::Window* root : Shell::GetAllRootWindows()) {
    for (auto* desk_container : desks_util::GetDesksContainers(root)) {
      DCHECK(!base::Contains(observed_container_windows_, desk_container));
      desk_container->AddObserver(this);
      observed_container_windows_.insert(desk_container);
    }
  }
}

void TabletModeWindowManager::RemoveWindowCreationObservers() {
  for (aura::Window* window : observed_container_windows_)
    window->RemoveObserver(this);
  observed_container_windows_.clear();
}

void TabletModeWindowManager::DisplayConfigurationChanged() {
  RemoveWindowCreationObservers();
  AddWindowCreationObservers();
  UpdateDeskContainersBackdrops();
}

bool TabletModeWindowManager::IsContainerWindow(aura::Window* window) {
  return base::Contains(observed_container_windows_, window);
}

}  // namespace ash
