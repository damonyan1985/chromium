// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/cocoa/first_run_dialog_controller.h"

#include "base/mac/scoped_nsobject.h"
#include "base/mac/sdk_forward_declarations.h"
#include "chrome/browser/ui/cocoa/key_equivalent_constants.h"
#include "chrome/browser/ui/cocoa/l10n_util.h"
#include "chrome/grit/chromium_strings.h"
#include "chrome/grit/generated_resources.h"
#include "ui/base/cocoa/controls/button_utils.h"
#include "ui/base/cocoa/controls/textfield_utils.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/l10n/l10n_util_mac.h"

namespace {

// Return the internationalized message |message_id|, with the product name
// substituted in for $1.
NSString* NSStringWithProductName(int message_id) {
  return l10n_util::GetNSStringF(message_id,
                                 l10n_util::GetStringUTF16(IDS_PRODUCT_NAME));
}

void MoveViewsVertically(NSArray* views, CGFloat distance) {
  for (NSView* view : views) {
    NSRect frame = view.frame;
    frame.origin.y += distance;
    [view setFrame:frame];
  }
}

// Center |view| vertically within its own superview, so its horizontal
// centerline is the same as its superview's horizontal centerline.
void CenterVertically(NSView* view) {
  NSView* superview = view.superview;
  NSRect frame = view.frame;
  NSRect superframe = superview.frame;
  frame.origin.y = (NSHeight(superframe) - NSHeight(frame)) / 2.0;
  [view setFrame:frame];
}

}  // namespace

@implementation FirstRunDialogViewController {
  // These are owned by the NSView hierarchy:
  NSButton* _defaultBrowserCheckbox;
  NSButton* _statsCheckbox;

  // This is owned by NSViewController:
  NSView* _view;

  BOOL _statsCheckboxInitiallyChecked;
  BOOL _defaultBrowserCheckboxVisible;
}

- (instancetype)initWithStatsCheckboxInitiallyChecked:(BOOL)checked
                        defaultBrowserCheckboxVisible:(BOOL)visible {
  if ((self = [super init])) {
    _statsCheckboxInitiallyChecked = checked;
    _defaultBrowserCheckboxVisible = visible;
  }
  return self;
}

- (void)loadView {
  BOOL isDarkMode = NO;
  if (@available(macOS 10.14, *)) {
    NSAppearanceName appearance =
        [[NSApp effectiveAppearance] bestMatchFromAppearancesWithNames:@[
          NSAppearanceNameAqua, NSAppearanceNameDarkAqua
        ]];
    isDarkMode = [appearance isEqual:NSAppearanceNameDarkAqua];
  }
  NSColor* topBoxColor = isDarkMode
                             ? [NSColor colorWithCalibratedRed:0x32 / 255.0
                                                         green:0x36 / 255.0
                                                          blue:0x39 / 255.0
                                                         alpha:1.0]
                             : [NSColor whiteColor];

  NSBox* topBox =
      [[[NSBox alloc] initWithFrame:NSMakeRect(0, 137, 480, 52)] autorelease];
  [topBox setFillColor:topBoxColor];
  [topBox setBoxType:NSBoxCustom];
  [topBox setBorderType:NSNoBorder];
  [topBox setContentViewMargins:NSZeroSize];

  NSTextField* completionLabel = [TextFieldUtils
      labelWithString:NSStringWithProductName(
                          IDS_FIRSTRUN_DLG_MAC_COMPLETE_INSTALLATION_LABEL)];
  [completionLabel setFrame:NSMakeRect(13, 25, 390, 17)];

  _defaultBrowserCheckbox = [ButtonUtils
      checkboxWithTitle:l10n_util::GetNSString(
                            IDS_FIRSTRUN_DLG_MAC_SET_DEFAULT_BROWSER_LABEL)];
  [_defaultBrowserCheckbox setFrame:NSMakeRect(45, 107, 389, 18)];
  [_defaultBrowserCheckbox setState:NSOnState];
  if (!_defaultBrowserCheckboxVisible)
    [_defaultBrowserCheckbox setHidden:YES];

  _statsCheckbox = [ButtonUtils
      checkboxWithTitle:
          NSStringWithProductName(
              IDS_FIRSTRUN_DLG_MAC_OPTIONS_SEND_USAGE_STATS_LABEL)];
  [_statsCheckbox setFrame:NSMakeRect(45, 82, 389, 19)];
  if (_statsCheckboxInitiallyChecked)
    [_statsCheckbox setState:NSOnState];

  NSButton* startChromeButton =
      [ButtonUtils buttonWithTitle:NSStringWithProductName(
                                       IDS_FIRSTRUN_DLG_MAC_START_CHROME_BUTTON)
                            action:@selector(ok:)
                            target:self];
  [startChromeButton setFrame:NSMakeRect(161, 12, 306, 32)];
  [startChromeButton setKeyEquivalent:kKeyEquivalentReturn];

  NSBox* topSeparator =
      [[[NSBox alloc] initWithFrame:NSMakeRect(0, 136, 480, 1)] autorelease];
  [topSeparator setBoxType:NSBoxSeparator];

  NSBox* bottomSeparator =
      [[[NSBox alloc] initWithFrame:NSMakeRect(0, 55, 480, 5)] autorelease];
  [bottomSeparator setBoxType:NSBoxSeparator];

  [topBox addSubview:completionLabel];
  CenterVertically(completionLabel);

  base::scoped_nsobject<NSView> content_view(
      [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 480, 190)]);
  self.view = content_view.get();
  [self.view addSubview:topBox];
  [self.view addSubview:topSeparator];
  [self.view addSubview:_defaultBrowserCheckbox];
  [self.view addSubview:_statsCheckbox];
  [self.view addSubview:bottomSeparator];
  [self.view addSubview:startChromeButton];

  // Now that the content view is constructed, fix the layout. The first step is
  // to reflow the browser and stats checkbox texts, which can be quite lengthy
  // in some locales. They may wrap onto additional lines, and in doing so cause
  // the rest of the dialog to need to be rearranged.
  {
    CGFloat delta = cocoa_l10n_util::VerticallyReflowGroup(
        @[ _defaultBrowserCheckbox, _statsCheckbox ]);
    if (delta) {
      // If reflowing the checkboxes produced a height delta, move the
      // checkboxes and the items above them in the content view upward, then
      // grow the content view to match. This has the effect of moving
      // everything visually-below the checkboxes downwards and expanding the
      // window, leaving the vertical space the checkboxes need for their text.
      MoveViewsVertically(
          @[ _defaultBrowserCheckbox, _statsCheckbox, topSeparator, topBox ],
          delta);
      NSRect frame = [self.view frame];
      frame.size.height += delta;
      [self.view setAutoresizesSubviews:NO];
      [self.view setFrame:frame];
      [self.view setAutoresizesSubviews:YES];
    }
  }

  // The "Start Chrome" button needs to be sized to fit the localized string
  // inside it, but it should still be at the right-most edge of the dialog, so
  // any width added or subtracted by |sizeToFit| is added to its x coord, which
  // keeps its right edge where it was.
  CGFloat oldWidth = NSWidth([startChromeButton frame]);
  [startChromeButton sizeToFit];
  NSRect frame = [startChromeButton frame];
  frame.origin.x += oldWidth - NSWidth([startChromeButton frame]);
  [startChromeButton setFrame:frame];

  // Lastly, if the default browser checkbox is actually invisible, move the
  // views above it downward so that there's not a big open space in the content
  // view, and resize the content view itself so there isn't extra space.
  if (!_defaultBrowserCheckboxVisible) {
    CGFloat delta = NSHeight([_defaultBrowserCheckbox frame]);
    MoveViewsVertically(@[ topBox, topSeparator ], -delta);
    NSRect frame = [self.view frame];
    frame.size.height -= delta;
    [self.view setAutoresizesSubviews:NO];
    [self.view setFrame:frame];
    [self.view setAutoresizesSubviews:YES];
  }
}

- (NSString*)windowTitle {
  return NSStringWithProductName(IDS_FIRSTRUN_DLG_MAC_WINDOW_TITLE);
}

- (BOOL)isStatsReportingEnabled {
  return [_statsCheckbox state] == NSOnState;
}

- (BOOL)isMakeDefaultBrowserEnabled {
  return [_defaultBrowserCheckbox state] == NSOnState;
}

- (void)ok:(id)sender {
  [[[self view] window] close];
  [NSApp stopModal];
}

@end
