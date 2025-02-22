// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * Test fixture for word_utils.js.
 * @constructor
 * @extends {testing.Test}
 */
function SelectToSpeakWordUtilsUnitTest() {
  testing.Test.call(this);
}

SelectToSpeakWordUtilsUnitTest.prototype = {
  __proto__: testing.Test.prototype,

  /** @override */
  extraLibraries: [
    'test_support.js',
    'paragraph_utils.js',
    'word_utils.js',
  ]
};

TEST_F(
    'SelectToSpeakWordUtilsUnitTest', 'getNextWordStartWithoutWordStarts',
    function() {
      let node = {node: {}};
      assertEquals(0, WordUtils.getNextWordStart('kitty cat', 0, node));
      assertEquals(1, WordUtils.getNextWordStart(' kitty cat', 0, node));
      assertEquals(6, WordUtils.getNextWordStart('kitty cat', 5, node));
      assertEquals(6, WordUtils.getNextWordStart('kitty cat', 6, node));
      assertEquals(7, WordUtils.getNextWordStart('kitty "cat"', 5, node));
    });

TEST_F(
    'SelectToSpeakWordUtilsUnitTest', 'getNextWordEndWithoutWordEnds',
    function() {
      let node = {node: {}};
      assertEquals(5, WordUtils.getNextWordEnd('kitty cat', 0, node));
      assertEquals(6, WordUtils.getNextWordEnd(' kitty cat', 0, node));
      assertEquals(9, WordUtils.getNextWordEnd('kitty cat', 6, node));
      assertEquals(9, WordUtils.getNextWordEnd('kitty cat', 7, node));
    });

TEST_F('SelectToSpeakWordUtilsUnitTest', 'getNextWordStart', function() {
  let inlineText = {wordStarts: [0, 6], name: 'kitty cat'};
  let staticText = {children: [inlineText], name: 'kitty cat'};
  let node = {node: staticText, startChar: 0, hasInlineText: true};
  assertEquals(0, WordUtils.getNextWordStart('kitty cat', 0, node));
  assertEquals(6, WordUtils.getNextWordStart('kitty cat', 5, node));
  assertEquals(6, WordUtils.getNextWordStart('kitty cat', 6, node));

  node.startChar = 10;
  assertEquals(10, WordUtils.getNextWordStart('once upon kitty cat', 9, node));
  assertEquals(16, WordUtils.getNextWordStart('once upon kitty cat', 15, node));

  // Should return the default if the inlineText children are missing.
  staticText.children = [];
  assertEquals(
      10, WordUtils.getNextWordStart('once upon a kitty cat', 10, node));
});

TEST_F(
    'SelectToSpeakWordUtilsUnitTest', 'getNextWordStartMultipleChildren',
    function() {
      let inlineText1 = {
        wordStarts: [0, 6],
        name: 'kitty cat ',
        indexInParent: 0
      };
      let inlineText2 = {wordStarts: [0, 3], name: 'is cute', indexInParent: 1};
      let staticText = {
        children: [inlineText1, inlineText2],
        name: 'kitty cat is cute'
      };
      inlineText1.parent = staticText;
      inlineText2.parent = staticText;
      let node = {node: staticText, startChar: 0, hasInlineText: true};
      assertEquals(
          10, WordUtils.getNextWordStart('kitty cat is cute', 7, node));
      assertEquals(
          13, WordUtils.getNextWordStart('kitty cat is cute', 11, node));
    });

TEST_F('SelectToSpeakWordUtilsUnitTest', 'getNextWordEnd', function() {
  let inlineText = {wordEnds: [5, 9], name: 'kitty cat'};
  let staticText = {children: [inlineText], name: 'kitty cat'};
  let node = {node: staticText, startChar: 0, hasInlineText: true};
  assertEquals(5, WordUtils.getNextWordEnd('kitty cat', 0, node));
  assertEquals(5, WordUtils.getNextWordEnd('kitty cat', 4, node));
  assertEquals(9, WordUtils.getNextWordEnd('kitty cat', 5, node));
  assertEquals(9, WordUtils.getNextWordEnd('kitty cat', 6, node));

  node.startChar = 10;
  assertEquals(15, WordUtils.getNextWordEnd('once upon kitty cat', 9, node));
  assertEquals(19, WordUtils.getNextWordEnd('once upon kitty cat', 17, node));

  // Should return the default if the inlineText children are missing.
  staticText.children = [];
  assertEquals(5, WordUtils.getNextWordEnd('kitty cat', 4, node));
});

TEST_F(
    'SelectToSpeakWordUtilsUnitTest', 'getNextWordEndMultipleChildren',
    function() {
      let inlineText1 = {
        wordEnds: [5, 9],
        name: 'kitty cat ',
        indexInParent: 0
      };
      let inlineText2 = {wordEnds: [2, 7], name: 'is cute', indexInParent: 1};
      let staticText = {
        children: [inlineText1, inlineText2],
        name: 'kitty cat is cute'
      };
      inlineText1.parent = staticText;
      inlineText2.parent = staticText;
      let node = {node: staticText, startChar: 0, hasInlineText: true};
      assertEquals(12, WordUtils.getNextWordEnd('kitty cat is cute', 10, node));
      assertEquals(17, WordUtils.getNextWordEnd('kitty cat is cute', 13, node));
    });