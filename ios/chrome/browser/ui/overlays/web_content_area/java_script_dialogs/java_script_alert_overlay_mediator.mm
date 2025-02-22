// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/ui/overlays/web_content_area/java_script_dialogs/java_script_alert_overlay_mediator.h"

#include "base/strings/sys_string_conversions.h"
#include "components/strings/grit/components_strings.h"
#import "ios/chrome/browser/overlays/public/overlay_request.h"
#import "ios/chrome/browser/overlays/public/web_content_area/java_script_alert_overlay.h"
#import "ios/chrome/browser/ui/alert_view/alert_action.h"
#import "ios/chrome/browser/ui/alert_view/alert_consumer.h"
#import "ios/chrome/browser/ui/dialogs/dialog_constants.h"
#import "ios/chrome/browser/ui/overlays/common/alerts/alert_overlay_mediator+alert_consumer_support.h"
#import "ios/chrome/browser/ui/overlays/web_content_area/java_script_dialogs/java_script_dialog_blocking_action.h"
#import "ios/chrome/browser/ui/overlays/web_content_area/java_script_dialogs/java_script_overlay_mediator_util.h"
#include "ios/chrome/grit/ios_strings.h"
#include "ui/base/l10n/l10n_util.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

@interface JavaScriptAlertOverlayMediator ()
// The congig from the request passed on initialization.
@property(nonatomic, readonly) JavaScriptAlertOverlayRequestConfig* config;
@end

@implementation JavaScriptAlertOverlayMediator

- (instancetype)initWithRequest:(OverlayRequest*)request {
  if (self = [super initWithRequest:request]) {
    // Verify that the request is configured for JavaScript alerts.
    DCHECK(request->GetConfig<JavaScriptAlertOverlayRequestConfig>());
  }
  return self;
}

#pragma mark - Accessors

- (JavaScriptAlertOverlayRequestConfig*)config {
  return self.request
             ? self.request->GetConfig<JavaScriptAlertOverlayRequestConfig>()
             : nullptr;
}

@end

@implementation JavaScriptAlertOverlayMediator (AlertConsumerSupport)

- (NSString*)alertTitle {
  return GetJavaScriptDialogTitle(self.config->source(),
                                  self.config->message());
}

- (NSString*)alertMessage {
  return GetJavaScriptDialogMessage(self.config->source(),
                                    self.config->message());
}

- (NSArray<AlertAction*>*)alertActions {
  __weak __typeof__(self) weakSelf = self;
  NSMutableArray<AlertAction*>* actions = [@[ [AlertAction
      actionWithTitle:l10n_util::GetNSString(IDS_OK)
                style:UIAlertActionStyleDefault
              handler:^(AlertAction* action) {
                [weakSelf.delegate stopOverlayForMediator:weakSelf];
              }] ] mutableCopy];
  AlertAction* blockingAction =
      GetBlockingAlertAction(self, self.config->source());
  if (blockingAction)
    [actions addObject:blockingAction];
  return actions;
}

- (NSString*)alertAccessibilityIdentifier {
  return kJavaScriptDialogAccessibilityIdentifier;
}

@end
