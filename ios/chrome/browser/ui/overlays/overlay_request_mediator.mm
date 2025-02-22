// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/ui/overlays/overlay_request_mediator.h"

#include "base/bind.h"
#include "base/logging.h"
#include "ios/chrome/browser/overlays/public/overlay_callback_manager.h"
#include "ios/chrome/browser/overlays/public/overlay_request.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

@interface OverlayRequestMediator ()
// Redefine property as readwrite.
@property(nonatomic, readwrite) OverlayRequest* request;
@end

@implementation OverlayRequestMediator

- (instancetype)initWithRequest:(OverlayRequest*)request {
  if (self = [super init]) {
    _request = request;
    _request->GetCallbackManager()->AddCompletionCallback(
        [self requestCompletionCallback]);
  }
  return self;
}

#pragma mark - Private

// Returns an OverlayCompletionCallback to reset the request pointer upon
// destruction to prevent it from being used in the event of overlay UI user
// interaction events that occur during dismissal after a request has been
// cancelled.
- (OverlayCompletionCallback)requestCompletionCallback {
  __weak __typeof(self) weakSelf = self;
  return base::BindOnce(^(OverlayResponse*) {
    weakSelf.request = nullptr;
  });
}

@end
