// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'chrome://resources/mojo/mojo/public/js/mojo_bindings_lite.js';
import 'chrome://resources/mojo/url/mojom/url.mojom-lite.js';
import 'chrome://new-tab-page/skcolor.mojom-lite.js';
import 'chrome://new-tab-page/new_tab_page.mojom-lite.js';

import {TestBrowserProxy} from 'chrome://test/test_browser_proxy.m.js';

export class TestProxy {
  constructor() {
    /** @type {newTabPage.mojom.PageCallbackRouter} */
    this.callbackRouter = new newTabPage.mojom.PageCallbackRouter();

    /** @type {!newTabPage.mojom.PageRemote} */
    this.callbackRouterRemote =
        this.callbackRouter.$.bindNewPipeAndPassRemote();

    /** @type {newTabPage.mojom.PageHandlerInterface} */
    this.handler = new FakePageHandler(this.callbackRouterRemote);
  }
}

/** @implements {newTabPage.mojom.PageHandlerInterface} */
class FakePageHandler {
  /** @param {newTabPage.mojom.PageInterface} */
  constructor(callbackRouterRemote) {
    /** @private {TestBrowserProxy} */
    this.callTracker_ = new TestBrowserProxy([
      'addMostVisitedTile',
      'deleteMostVisitedTile',
      'reorderMostVisitedTile',
      'restoreMostVisitedDefaults',
      'undoMostVisitedTileAction',
      'updateMostVisitedTile',
      'getCustomizeInfo',
      'applyDefaultTheme',
      'applyAutogeneratedTheme',
      'applyChromeTheme',
    ]);
  }

  /**
   * @param {string} methodName
   * @return {!Promise}
   */
  whenCalled(methodName) {
    return this.callTracker_.whenCalled(methodName);
  }

  /**
   * @param {string} methodName
   * @param {*} value
   */
  setResultFor(methodName, value) {
    this.callTracker_.setResultFor(methodName, value);
  }

  /** @override */
  addMostVisitedTile(url, title) {
    this.callTracker_.methodCalled('addMostVisitedTile', [url, title]);
    return {success: true};
  }

  /** @override */
  deleteMostVisitedTile(url) {
    this.callTracker_.methodCalled('deleteMostVisitedTile', url);
    return {success: true};
  }

  /** @override */
  reorderMostVisitedTile(url, newPos) {
    this.callTracker_.methodCalled('reorderMostVisitedTile', [url, newPos]);
  }

  /** @override */
  restoreMostVisitedDefaults() {
    this.callTracker_.methodCalled('restoreMostVisitedDefaults');
  }

  /** @override */
  undoMostVisitedTileAction() {
    this.callTracker_.methodCalled('undoMostVisitedTileAction');
  }

  /** @override */
  updateMostVisitedTile(url, newUrl, newTitle) {
    this.callTracker_.methodCalled(
        'updateMostVisitedTile', [url, newUrl, newTitle]);
    return {success: true};
  }

  /** @override */
  getCustomizeInfo() {
    this.callTracker_.methodCalled('getCustomizeInfo');
    return this.callTracker_.getResultFor('getCustomizeInfo', Promise.resolve({
      info: {
        currentTheme: {type: newTabPage.mojom.ThemeType.DEFAULT},
        chromeThemes: [],
      },
    }));
  }

  /** @override */
  applyDefaultTheme() {
    this.callTracker_.methodCalled('applyDefaultTheme');
  }

  /** @override */
  applyAutogeneratedTheme(frameColor) {
    this.callTracker_.methodCalled('applyAutogeneratedTheme', frameColor);
  }

  /** @override */
  applyChromeTheme(id) {
    this.callTracker_.methodCalled('applyChromeTheme', id);
  }
}

/**
 * @param {!HTMLElement} element
 * @param {string} key
 */
export function keydown(element, key) {
  element.dispatchEvent(new KeyboardEvent('keydown', {key: key}));
}

/**
 * Asserts the computed style value for an element.
 * @param {!HTMLElement} element The element.
 * @param {string} name The name of the style to assert.
 * @param {string} expected The expected style value.
 */
export function assertStyle(element, name, expected) {
  const actual = window.getComputedStyle(element).getPropertyValue(name).trim();
  assertEquals(expected, actual);
}
