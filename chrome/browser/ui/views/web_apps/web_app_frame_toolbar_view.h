// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_WEB_APPS_WEB_APP_FRAME_TOOLBAR_VIEW_H_
#define CHROME_BROWSER_UI_VIEWS_WEB_APPS_WEB_APP_FRAME_TOOLBAR_VIEW_H_

#include <utility>
#include <vector>

#include "base/macros.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/ui/views/frame/toolbar_button_provider.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/views/accessible_pane_view.h"

namespace views {
class View;
class Widget;
}  // namespace views

class BrowserView;
class ContentSettingImageView;

#if defined(OS_MACOSX)
constexpr int kWebAppMenuMargin = 7;
#endif

// A container for web app buttons in the title bar.
class WebAppFrameToolbarView : public views::AccessiblePaneView,
                               public ToolbarButtonProvider {
 public:
  static const char kViewClassName[];

  // Timing parameters for the origin fade animation.
  // These control how long it takes for the origin text and menu button
  // highlight to fade in, pause then fade out.
  static constexpr base::TimeDelta kOriginFadeInDuration =
      base::TimeDelta::FromMilliseconds(800);
  static constexpr base::TimeDelta kOriginPauseDuration =
      base::TimeDelta::FromMilliseconds(2500);
  static constexpr base::TimeDelta kOriginFadeOutDuration =
      base::TimeDelta::FromMilliseconds(800);

  // The total duration of the origin fade animation.
  static base::TimeDelta OriginTotalDuration();

  // |active_color| and |inactive_color| indicate the colors to use
  // for button icons when the window is focused and blurred respectively.
  WebAppFrameToolbarView(views::Widget* widget,
                         BrowserView* browser_view,
                         SkColor active_color,
                         SkColor inactive_color);
  ~WebAppFrameToolbarView() override;

  void UpdateStatusIconsVisibility();

  void UpdateCaptionColors();

  // Sets the container to paints its buttons the active/inactive color.
  void SetPaintAsActive(bool active);

  // Sets own bounds equal to the available space and returns the bounds of the
  // remaining inner space as a pair of (leading x, trailing x).
  std::pair<int, int> LayoutInContainer(int leading_x,
                                        int trailing_x,
                                        int y,
                                        int available_height);

  SkColor active_color_for_testing() const { return active_color_; }

  // ToolbarButtonProvider:
  BrowserActionsContainer* GetBrowserActionsContainer() override;
  ToolbarActionView* GetToolbarActionViewForId(const std::string& id) override;
  views::View* GetDefaultExtensionDialogAnchorView() override;
  PageActionIconView* GetPageActionIconView(PageActionIconType type) override;
  AppMenuButton* GetAppMenuButton() override;
  gfx::Rect GetFindBarBoundingBox(int contents_bottom) const override;
  void FocusToolbar() override;
  views::AccessiblePaneView* GetAsAccessiblePaneView() override;
  views::View* GetAnchorView(PageActionIconType type) override;
  void ZoomChangedForActiveTab(bool can_show_bubble) override;
  AvatarToolbarButton* GetAvatarToolbarButton() override;
  ToolbarButton* GetBackButton() override;
  ReloadButton* GetReloadButton() override;

  static void DisableAnimationForTesting();
  views::View* GetLeftContainerForTesting();
  views::View* GetRightContainerForTesting();
  views::View* GetPageActionIconContainerForTesting();

 protected:
  // views::AccessiblePaneView:
  const char* GetClassName() const override;
  void ChildPreferredSizeChanged(views::View* child) override;

 private:
  friend class WebAppNonClientFrameViewAshTest;
  friend class ImmersiveModeControllerAshWebAppBrowserTest;
  friend class WebAppAshInteractiveUITest;

  // Duration to wait before starting the opening animation.
  static constexpr base::TimeDelta kTitlebarAnimationDelay =
      base::TimeDelta::FromMilliseconds(750);

  class ContentSettingsContainer;

  views::View* GetContentSettingContainerForTesting();

  const std::vector<ContentSettingImageView*>&
  GetContentSettingViewsForTesting() const;

  SkColor GetCaptionColor() const;
  void UpdateChildrenColor();

  // The containing browser view.
  BrowserView* const browser_view_;

  // Button and text colors.
  bool paint_as_active_ = true;
  SkColor active_color_;
  SkColor inactive_color_;

  class NavigationButtonContainer;
  class ToolbarButtonContainer;

  // All remaining members are owned by the views hierarchy.

  // The navigation container is only created when display mode is minimal-ui.
  NavigationButtonContainer* left_container_ = nullptr;

  // Empty container used by the parent frame to layout additional elements.
  views::View* center_container_ = nullptr;

  ToolbarButtonContainer* right_container_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(WebAppFrameToolbarView);
};

#endif  // CHROME_BROWSER_UI_VIEWS_WEB_APPS_WEB_APP_FRAME_TOOLBAR_VIEW_H_
