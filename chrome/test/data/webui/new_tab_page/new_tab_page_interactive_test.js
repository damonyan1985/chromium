// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/** @fileoverview Test suite for the WebUI new tab page page. */

GEN_INCLUDE(['//chrome/test/data/webui/polymer_interactive_ui_test.js']);
GEN('#include "services/network/public/cpp/features.h"');

class NewTabPageInteractiveTest extends PolymerInteractiveUITest {
  /** @override */
  get browsePreload() {
    throw 'this is abstract and should be overriden by subclasses';
  }

  /** @override */
  get extraLibraries() {
    return [
      '//third_party/mocha/mocha.js',
      '//chrome/test/data/webui/mocha_adapter.js',
    ];
  }

  /** @override */
  get featureList() {
    return {enabled: ['network::features::kOutOfBlinkCors']};
  }
}

// eslint-disable-next-line no-var
var NewTabPageMostVisitedFocusTest = class extends NewTabPageInteractiveTest {
  /** @override */
  get browsePreload() {
    return 'chrome://new-tab-page/test_loader.html?module=new_tab_page/most_visited_focus_test.js';
  }
};

TEST_F('NewTabPageMostVisitedFocusTest', 'All', function() {
  mocha.run();
});
