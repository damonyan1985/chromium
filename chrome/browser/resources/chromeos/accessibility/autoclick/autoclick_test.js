// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

GEN_INCLUDE(['../chromevox/testing/callback_helper.js']);
GEN_INCLUDE(['mock_accessibility_private.js']);

/**
 * Base class for browser tests for automatic clicks extension.
 * @constructor
 */
function AutoclickE2ETest() {
  this.callbackHelper_ = new CallbackHelper(this);
  this.mockAccessibilityPrivate = MockAccessibilityPrivate;
  chrome.accessibilityPrivate = this.mockAccessibilityPrivate;

  // Re-initialize Autoclick with mock AccessibilityPrivate API.
  autoclick = new Autoclick(false /* do not blink focus rings */);
}

AutoclickE2ETest.prototype = {
  __proto__: testing.Test.prototype,

  /**
   * @override
   * No UI in the background context.
   */
  runAccessibilityChecks: false,

  /** @override */
  isAsync: true,

  /** @override */
  browsePreload: null,

  /** @override */
  testGenCppIncludes: function() {
    GEN(`
#include "ash/accessibility/accessibility_delegate.h"
#include "ash/shell.h"
#include "base/bind.h"
#include "base/callback.h"
#include "chrome/browser/chromeos/accessibility/accessibility_manager.h"
#include "chrome/common/extensions/extension_constants.h"
    `);
  },

  /** @override */
  testGenPreamble: function() {
    GEN(`
  base::Closure load_cb =
      base::Bind(&chromeos::AccessibilityManager::EnableAutoclick,
          base::Unretained(chromeos::AccessibilityManager::Get()),
          true);
  chromeos::AccessibilityManager::Get()->EnableAutoclick(true);
  WaitForExtension(extension_misc::kAutoclickExtensionId, load_cb);
    `);
  },

  /**
   * Creates a callback that optionally calls {@code opt_callback} when
   * called.  If this method is called one or more times, then
   * {@code testDone()} will be called when all callbacks have been called.
   * @param {Function=} opt_callback Wrapped callback that will have its this
   *        reference bound to the test fixture.
   * @return {Function}
   */
  newCallback: function(opt_callback) {
    return this.callbackHelper_.wrap(opt_callback);
  },

  /**
   * From chromevox_next_e2e_test_base.js
   * Gets the desktop from the automation API and Launches a new tab with
   * the given document, and runs |callback| with the desktop when a load
   * complete fires on the created tab.
   * Arranges to call |testDone()| after |callback| returns.
   * NOTE: Callbacks created inside |callback| must be wrapped with
   * |this.newCallback| if passed to asynchonous calls.  Otherwise, the test
   * will be finished prematurely.
   * @param {string} url Url to load and wait for.
   * @param {function(chrome.automation.AutomationNode)} callback Called with
   *     the desktop node once the document is ready.
   */
  runWithLoadedTree: function(url, callback) {
    callback = this.newCallback(callback);
    chrome.automation.getDesktop(function(desktopRootNode) {
      var createParams = {active: true, url: url};
      chrome.tabs.create(createParams, function(unused_tab) {
        chrome.automation.getTree(function(returnedRootNode) {
          rootNode = returnedRootNode;
          if (rootNode.docLoaded) {
            callback && callback(desktopRootNode);
            callback = null;
            return;
          }
          rootNode.addEventListener('loadComplete', function(evt) {
            if (evt.target.root.url != url) {
              return;
            }
            callback && callback(desktopRootNode);
            callback = null;
          });
        });
      });
    }.bind(this));
  },

  /**
   * Asserts that two rects are the same.
   * @param {!chrome.accessibilityPrivate.ScreenRect} first
   * @param {!chrome.accessibilityPrivate.ScreenRect} second
   */
  assertSameRect: function(first, second) {
    assertEquals(first.left, second.left);
    assertEquals(first.top, second.top);
    assertEquals(first.width, second.width);
    assertEquals(first.height, second.height);
  },
};

TEST_F('AutoclickE2ETest', 'HighlightsRootWebAreaIfNotScrollable', function() {
  this.runWithLoadedTree(
      'data:text/html;charset=utf-8,<p>Cats rock!</p>', function(desktop) {
        let node = desktop.find(
            {role: 'staticText', attributes: {name: 'Cats rock!'}});
        this.mockAccessibilityPrivate.callFindScrollableBoundsForPoint(
            // Offset slightly into the node to ensure the hittest happens
            // within the node.
            node.location.left + 1, node.location.top + 1,
            this.newCallback(() => {
              let expected = node.root.location;
              this.assertSameRect(
                  this.mockAccessibilityPrivate.getScrollableBounds(),
                  expected);
              this.assertSameRect(
                  this.mockAccessibilityPrivate.getFocusRings()[0], expected);
            }));
      });
});

TEST_F('AutoclickE2ETest', 'HighlightsScrollableDiv', function() {
  this.runWithLoadedTree(
      'data:text/html;charset=utf-8,' +
          '<div style="width:100px;height:100px;overflow:scroll">' +
          '<div style="margin:50px">cats rock! this text wraps and overflows!' +
          '</div></div>',
      function(desktop) {
        let node = desktop.find({
          role: 'staticText',
          attributes: {name: 'cats rock! this text wraps and overflows!'}
        });
        this.mockAccessibilityPrivate.callFindScrollableBoundsForPoint(
            // Offset slightly into the node to ensure the hittest happens
            // within the node.
            node.location.left + 1, node.location.top + 1,
            this.newCallback(() => {
              // The outer div, which is the parent of the parent of the
              // text, is scrollable.
              assertTrue(node.parent.parent.scrollable);
              let expected = node.parent.parent.location;
              this.assertSameRect(
                  this.mockAccessibilityPrivate.getScrollableBounds(),
                  expected);
              this.assertSameRect(
                  this.mockAccessibilityPrivate.getFocusRings()[0], expected);
            }));
      });
});

// TODO(crbug.com/978163): Add tests for when the scrollable area is scrolled
// all the way up or down, left or right. Add tests for nested scrollable areas.
// Add tests for root types like toolbar, dialog, and window to ensure
// we don't break boundaries when searching for scroll bars.
