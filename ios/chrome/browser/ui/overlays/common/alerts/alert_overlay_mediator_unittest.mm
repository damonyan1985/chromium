// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/ui/overlays/common/alerts/alert_overlay_mediator.h"

#include "ios/chrome/browser/overlays/public/overlay_request.h"
#include "ios/chrome/browser/overlays/test/fake_overlay_user_data.h"
#import "ios/chrome/browser/ui/alert_view/alert_action.h"
#import "ios/chrome/browser/ui/alert_view/test/fake_alert_consumer.h"
#import "ios/chrome/browser/ui/elements/text_field_configuration.h"
#import "ios/chrome/browser/ui/overlays/common/alerts/alert_overlay_mediator+alert_consumer_support.h"
#import "ios/chrome/browser/ui/overlays/common/alerts/test/alert_overlay_mediator_test.h"
#include "testing/gtest_mac.h"
#include "testing/platform_test.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

// Fake subclass of AlertOverlayMediator for tests.
@interface FakeAlertOverlayMediator : AlertOverlayMediator
// Define readwrite versions of subclassing properties.
@property(nonatomic, readwrite) NSString* alertTitle;
@property(nonatomic, readwrite) NSString* alertMessage;
@property(nonatomic, readwrite)
    NSArray<TextFieldConfiguration*>* alertTextFieldConfigurations;
@property(nonatomic, readwrite) NSArray<AlertAction*>* alertActions;
@property(nonatomic, readwrite) NSString* alertAccessibilityIdentifier;
@end

@implementation FakeAlertOverlayMediator
@end

// Tests that the AlertOverlayMediator's subclassing properties are correctly
// applied to the consumer.
TEST_F(AlertOverlayMediatorTest, SetUpConsumer) {
  std::unique_ptr<OverlayRequest> request =
      OverlayRequest::CreateWithConfig<FakeOverlayUserData>(nullptr);
  FakeAlertOverlayMediator* mediator =
      [[FakeAlertOverlayMediator alloc] initWithRequest:request.get()];
  mediator.alertTitle = @"Title";
  mediator.alertMessage = @"Message";
  mediator.alertTextFieldConfigurations =
      @[ [[TextFieldConfiguration alloc] initWithText:@"Text"
                                          placeholder:@"placeholder"
                              accessibilityIdentifier:@"identifier"
                                      secureTextEntry:NO] ];
  mediator.alertActions =
      @[ [AlertAction actionWithTitle:@"Title"
                                style:UIAlertActionStyleDefault
                              handler:nil] ];
  mediator.alertAccessibilityIdentifier = @"identifier";

  SetMediator(mediator);
  EXPECT_NSEQ(mediator.alertTitle, consumer().title);
  EXPECT_NSEQ(mediator.alertMessage, consumer().message);
  EXPECT_NSEQ(mediator.alertTextFieldConfigurations,
              consumer().textFieldConfigurations);
  EXPECT_NSEQ(mediator.alertActions, consumer().actions);
  EXPECT_NSEQ(mediator.alertAccessibilityIdentifier,
              consumer().alertAccessibilityIdentifier);
}
