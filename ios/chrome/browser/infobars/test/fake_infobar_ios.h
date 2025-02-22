// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IOS_CHROME_BROWSER_INFOBARS_TEST_FAKE_INFOBAR_IOS_H_
#define IOS_CHROME_BROWSER_INFOBARS_TEST_FAKE_INFOBAR_IOS_H_

#include "ios/chrome/browser/infobars/infobar_ios.h"

@class FakeInfobarUIDelegate;
class FakeInfobarDelegate;

// Fake version of InfoBarIOS set up with fake delegates to use in tests.
class FakeInfobarIOS : public InfoBarIOS {
 public:
  FakeInfobarIOS();
  ~FakeInfobarIOS() override;

  // The fake UI delegate.
  FakeInfobarUIDelegate* fake_ui_delegate() const { return fake_ui_delegate_; }
  // The fake InfoBarDelegate.
  FakeInfobarDelegate* fake_delegate() const { return fake_delegate_; }

 private:
  FakeInfobarUIDelegate* fake_ui_delegate_ = nil;
  FakeInfobarDelegate* fake_delegate_ = nullptr;
};

#endif  // IOS_CHROME_BROWSER_INFOBARS_TEST_FAKE_INFOBAR_IOS_H_
