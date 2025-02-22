// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Include test fixture.
GEN_INCLUDE(['../testing/chromevox_next_e2e_test_base.js']);
GEN_INCLUDE(['../testing/mock_feedback.js']);

/**
 * Test fixture for ChromeVox LanguageSwitching.
 * @constructor
 * @extends {ChromeVoxE2ETest}
 */
function ChromeVoxLanguageSwitchingTest() {
  ChromeVoxNextE2ETest.call(this);
}

ChromeVoxLanguageSwitchingTest.prototype = {
  __proto__: ChromeVoxNextE2ETest.prototype,

  /** @override */
  testGenCppIncludes: function() {
    GEN(`
// The following includes are copy-pasted from chromevox_e2e_test_base.js.
#include "ash/accessibility/accessibility_delegate.h"
#include "ash/shell.h"
#include "base/bind.h"
#include "base/callback.h"
#include "chrome/browser/chromeos/accessibility/accessibility_manager.h"
#include "chrome/common/extensions/extension_constants.h"
#include "extensions/common/extension_l10n_util.h"

// The following includes are necessary for this test file.
#include "base/command_line.h"
#include "ui/accessibility/accessibility_switches.h"
#include "ui/base/ui_base_switches.h"
    `);
  },

  /** @override */
  testGenPreamble: function() {
    GEN(`
  base::CommandLine::ForCurrentProcess()->AppendSwitch(
    ::switches::kEnableExperimentalAccessibilityLanguageDetection);
  base::CommandLine::ForCurrentProcess()->AppendSwitch(
    ::switches::kEnableExperimentalAccessibilityChromeVoxLanguageSwitching);
  base::CommandLine::ForCurrentProcess()->AppendSwitch(
    ::switches::kEnableExperimentalAccessibilityChromeVoxSubNodeLanguageSwitching);
  base::CommandLine::ForCurrentProcess()->AppendSwitchASCII(::switches::kLang, "en-US");

  // Copy-pasted from chromevox_e2e_test_base.js.
  auto allow = extension_l10n_util::AllowGzippedMessagesAllowedForTest();
  base::Closure load_cb =
    base::Bind(&chromeos::AccessibilityManager::EnableSpokenFeedback,
        base::Unretained(chromeos::AccessibilityManager::Get()),
        true);
  WaitForExtension(extension_misc::kChromeVoxExtensionId, load_cb);
    `);
  },

  /** @override */
  setUp: function() {
    window.doCmd = this.doCmd;
    // Mock this api to return a predefined set of voices.
    chrome.tts.getVoices = function(callback) {
      callback([
        {'lang': 'en-US'}, {'lang': 'fr-CA'}, {'lang': 'es-ES'},
        {'lang': 'it-IT'}, {'lang': 'ja-JP'}, {'lang': 'ko-KR'},
        {'lang': 'zh-TW'}, {'lang': 'ast'}
      ]);
    };

    this.setAvailableVoices();
  },

  /**
   * @return {!MockFeedback}
   */
  createMockFeedback: function() {
    var mockFeedback =
        new MockFeedback(this.newCallback(), this.newCallback.bind(this));

    mockFeedback.install();
    return mockFeedback;
  },

  /**
   * Create a function which performs the command |cmd|.
   * @param {string} cmd
   * @return {function(): void}
   */
  doCmd: function(cmd) {
    return function() {
      CommandHandler.onCommand(cmd);
    };
  },

  /**
   * Calls mock version of chrome.tts.getVoices() to populate
   * LanguageSwitching's available voice list with a specific set of voices.
   */
  setAvailableVoices: function() {
    chrome.tts.getVoices(function(voices) {
      LanguageSwitching.availableVoices_ = voices;
    });
  },

  // Test documents //


  // The purpose of this doc is to test functionality with three-letter language
  // codes. Asturian has a language code of 'ast'. It is a language spoken
  // in Principality of Asturias, Spain.
  asturianAndJapaneseDoc: `
    <meta charset="utf-8">
    <p lang="ja">ど</p>
    <p lang="ast">
      Pretend that this text is Asturian. Testing three-letter language code logic.
    </p>
  `,

  buttonAndLinkDoc: `
    <body lang="es">
      <p>This is a paragraph, written in English.</p>
      <button type="submit">This is a button, written in English.</button>
      <a href="https://www.google.com">Este es un enlace.</a>
    </body>
  `,

  englishAndFrenchUnlabeledDoc: `
    <p>
      This entire object should be read in English, even the following French passage:
      salut mon ami! Ca va? Bien, et toi? It's hard to differentiate between latin-based languages.
    </p>
  `,

  englishAndKoreanUnlabeledDoc: `
    <meta charset="utf-8">
    <p>This text is written in English. 차에 한하여 중임할 수. This text is also written in English.</p>
  `,

  japaneseAndChineseUnlabeledDoc: `
    <meta charset="utf-8">
    <p id="text">
      天気はいいですね. 右万諭全中結社原済権人点掲年難出面者会追
    </p>
  `,

  japaneseAndEnglishUnlabeledDoc: `
    <meta charset="utf-8">
    <p>Hello, my name is 太田あきひろ. It's a pleasure to meet you. どうぞよろしくお願いします.</p>
  `,

  japaneseAndKoreanUnlabeledDoc: `
    <meta charset="utf-8">
    <p lang="ko">
      私は. 법률이 정하는 바에 의하여 대법관이 아닌 법관을 둘 수 있다
    </p>
  `,

  japaneseCharacterUnlabeledDoc: `
    <meta charset="utf-8">
    <p>ど</p>
  `,

  multipleLanguagesLabeledDoc: `
    <p lang="es">Hola.</p>
    <p lang="en">Hello.</p>
    <p lang="fr">Salut.</p>
    <span lang="it">Ciao amico.</span>
  `,

  japaneseAndInvalidLanguagesLabeledDoc: `
    <meta charset="utf-8">
    <p lang="ja">どうぞよろしくお願いします</p>
    <p lang="invalid-code">Test</p>
    <p lang="hello">Yikes</p>
  `,

  nestedLanguagesLabeledDoc: `
    <p id="breakfast" lang="en">In the morning, I sometimes eat breakfast.</p>
    <p id="lunch" lang="fr">Dans l'apres-midi, je dejeune.</p>
    <p id="greeting" lang="en">
      Hello it's a pleasure to meet you.
      <span lang="fr">Comment ca va?</span>Switching back to English.
      <span lang="es">Hola.</span>Goodbye.
    </p>
  `,

  vietnameseAndUrduLabeledDoc: `
    <p lang="vi">Vietnamese text.</p>
    <p lang="ur">Urdu text.</p>
  `,
};

// Overview:
// The naming scheme of the language switching tests is as follows:
// <switching_behavior>_<test_document>_Test.
// <switching_behavior>: Whether the test is testing node-level or
// sub-node-level switching. <test_document>: The name of the document the test
// uses. Example: NodeLevelSwitching_MultipleLanguagesLabeledDoc_Test Each group
// of tests test the two switching behaviors with various labelings.


TEST_F(
    'ChromeVoxLanguageSwitchingTest',
    'SubNodeLevelSwitching_MultipleLanguagesLabeledDoc_Test', function() {
      var mockFeedback = this.createMockFeedback();
      this.runWithLoadedTree(this.multipleLanguagesLabeledDoc, function() {
        // Turn on language switching.
        localStorage['languageSwitching'] = 'true';
        this.setAvailableVoices();
        mockFeedback.call(doCmd('jumpToTop'))
            .expectSpeechWithLanguage('es', 'español: Hola.');
        mockFeedback.call(doCmd('nextLine'))
            .expectSpeechWithLanguage('en', 'English: Hello.');
        mockFeedback.call(doCmd('nextLine'))
            .expectSpeechWithLanguage('fr', 'français: Salut.');
        mockFeedback.call(doCmd('nextLine'))
            .expectSpeechWithLanguage('it', 'italiano: Ciao amico.');
        mockFeedback.replay();
      });
    });

TEST_F(
    'ChromeVoxLanguageSwitchingTest',
    'NodeLevelSwitching_MultipleLanguagesLabeledDoc_Test', function() {
      var mockFeedback = this.createMockFeedback();
      this.runWithLoadedTree(this.multipleLanguagesLabeledDoc, function() {
        localStorage['languageSwitching'] = 'true';
        this.setAvailableVoices();
        // Disable sub-node-level switching.
        LanguageSwitching.sub_node_switching_enabled_ = false;
        mockFeedback.call(doCmd('jumpToTop'))
            .expectSpeechWithLanguage('es', 'español: Hola.');
        mockFeedback.call(doCmd('nextLine'))
            .expectSpeechWithLanguage('en', 'English: Hello.');
        mockFeedback.call(doCmd('nextLine'))
            .expectSpeechWithLanguage('fr', 'français: Salut.');
        mockFeedback.call(doCmd('nextLine'))
            .expectSpeechWithLanguage('it', 'italiano: Ciao amico.');
        mockFeedback.replay();
      });
    });


TEST_F(
    'ChromeVoxLanguageSwitchingTest',
    'SubNodeLevelSwitching_NestedLanguagesLabeledDoc_Test', function() {
      var mockFeedback = this.createMockFeedback();
      this.runWithLoadedTree(this.nestedLanguagesLabeledDoc, function() {
        localStorage['languageSwitching'] = 'true';
        this.setAvailableVoices();
        // We should be able to switch languages when each component is labeled
        // with a language.
        mockFeedback
            .call(doCmd('jumpToTop'))
            // LanguageSwitching.currentLanguage_ is initialized to 'en'. Do not
            // prepend 'English' because language does not switch.
            .expectSpeechWithLanguage(
                'en', 'In the morning, I sometimes eat breakfast.');
        mockFeedback.call(doCmd('nextLine'))
            .expectSpeechWithLanguage(
                'fr', 'français: Dans l\'apres-midi, je dejeune.');
        mockFeedback.call(doCmd('nextLine'))
            .expectSpeechWithLanguage(
                'en', 'English: Hello it\'s a pleasure to meet you. ');
        mockFeedback.call(doCmd('nextLine'))
            .expectSpeechWithLanguage('fr', 'français: Comment ca va?');
        mockFeedback.call(doCmd('nextLine'))
            .expectSpeechWithLanguage(
                'en', 'English: Switching back to English. ');
        mockFeedback.call(doCmd('nextLine'))
            .expectSpeechWithLanguage('es', 'español: Hola.');
        mockFeedback.call(doCmd('nextLine'))
            .expectSpeechWithLanguage('en', 'English: Goodbye.');
        mockFeedback.replay();
      });
    });

TEST_F(
    'ChromeVoxLanguageSwitchingTest',
    'NodeLevelSwitching_NestedLanguagesLabeledDoc_Test', function() {
      var mockFeedback = this.createMockFeedback();
      this.runWithLoadedTree(this.nestedLanguagesLabeledDoc, function() {
        localStorage['languageSwitching'] = 'true';
        this.setAvailableVoices();
        // Disable sub-node-switching.
        LanguageSwitching.sub_node_switching_enabled_ = false;
        mockFeedback.call(doCmd('jumpToTop'))
            .expectSpeechWithLanguage(
                'en', 'In the morning, I sometimes eat breakfast.');
        mockFeedback.call(doCmd('nextLine'))
            .expectSpeechWithLanguage(
                'fr', 'français: Dans l\'apres-midi, je dejeune.');
        mockFeedback.call(doCmd('nextLine'))
            .expectSpeechWithLanguage(
                'en', 'English: Hello it\'s a pleasure to meet you. ');
        mockFeedback.call(doCmd('nextLine'))
            .expectSpeechWithLanguage('fr', 'français: Comment ca va?');
        mockFeedback.call(doCmd('nextLine'))
            .expectSpeechWithLanguage(
                'en', 'English: Switching back to English. ');
        mockFeedback.call(doCmd('nextLine'))
            .expectSpeechWithLanguage('es', 'español: Hola.');
        mockFeedback.call(doCmd('nextLine'))
            .expectSpeechWithLanguage('en', 'English: Goodbye.');
        mockFeedback.replay();
      });
    });


TEST_F(
    'ChromeVoxLanguageSwitchingTest',
    'SubNodeLevelSwitching_ButtonAndLinkDoc_Test', function() {
      var mockFeedback = this.createMockFeedback();
      this.runWithLoadedTree(this.buttonAndLinkDoc, function(root) {
        localStorage['languageSwitching'] = 'true';
        this.setAvailableVoices();
        mockFeedback
            .call(doCmd('jumpToTop'))
            // Sub-node language detection is able to label this as 'en' and
            // overwrite the author-provided language of 'es'.
            // LanguageSwitching.currentLanguage_ is initialized to 'en'. Do not
            // prepend 'English' because language does not switch.
            .expectSpeechWithLanguage(
                'en', 'This is a paragraph, written in English.')
            .call(doCmd('nextObject'))
            // CLD3 is able to determine, with high confidence, that this is
            // English text.
            .expectSpeechWithLanguage(
                'en', 'This is a button, written in English.')
            .expectSpeechWithLanguage(
                undefined, 'Button', 'Press Search+Space to activate.')
            .call(doCmd('nextObject'))
            .expectSpeechWithLanguage('es', 'español: Este es un enlace.')
            .expectSpeechWithLanguage(undefined, 'Link');
        mockFeedback.replay();
      });
    });

TEST_F(
    'ChromeVoxLanguageSwitchingTest',
    'NodeLevelSwitching_ButtonAndLinkDoc_Test', function() {
      var mockFeedback = this.createMockFeedback();
      this.runWithLoadedTree(this.buttonAndLinkDoc, function(root) {
        localStorage['languageSwitching'] = 'true';
        this.setAvailableVoices();
        // Disable sub-node-switching.
        LanguageSwitching.sub_node_switching_enabled_ = false;
        mockFeedback
            .call(doCmd('jumpToTop'))
            // Sub-node language detection is disabled, so we are not able to
            // detect + switch to English on any of these nodes. Instead, we use
            // the author-provided language of 'es'.
            .expectSpeechWithLanguage(
                'es', 'español: This is a paragraph, written in English.')
            .call(doCmd('nextObject'))
            .expectSpeechWithLanguage(
                'es', 'This is a button, written in English.')
            .expectSpeechWithLanguage(
                undefined, 'Button', 'Press Search+Space to activate.')
            .call(doCmd('nextObject'))
            .expectSpeechWithLanguage('es', 'Este es un enlace.')
            .expectSpeechWithLanguage(undefined, 'Link');
        mockFeedback.replay();
      });
    });


TEST_F(
    'ChromeVoxLanguageSwitchingTest',
    'SubNodeLevelSwitching_JapaneseAndEnglishUnlabeledDoc_Test', function() {
      var mockFeedback = this.createMockFeedback();
      this.runWithLoadedTree(
          this.japaneseAndEnglishUnlabeledDoc, function(root) {
            localStorage['languageSwitching'] = 'true';
            this.setAvailableVoices();
            // We are able to separate out English and Japanese because they use
            // different scripts.
            mockFeedback
                .call(doCmd('jumpToTop'))
                // LanguageSwitching.currentLanguage_ is initialized to 'en'. Do
                // not prepend 'English' because language does not switch.
                .expectSpeechWithLanguage('en', 'Hello, my name is ')
                .expectSpeechWithLanguage('ja', '日本語: 太田あきひろ. ')
                // Expect 'en-us' because sub-node language of 'en' doesn't come
                // with high enough probability. We fall back on node-level
                // detected language, which is 'en-us'.
                .expectSpeechWithLanguage(
                    'en-us', 'English: It\'s a pleasure to meet you. ')
                .expectSpeechWithLanguage(
                    'ja', '日本語: どうぞよろしくお願いします.');
            mockFeedback.replay();
          });
    });

TEST_F(
    'ChromeVoxLanguageSwitchingTest',
    'NodeLevelSwitching_JapaneseAndEnglishUnlabeledDoc_Test', function() {
      var mockFeedback = this.createMockFeedback();
      this.runWithLoadedTree(
          this.japaneseAndEnglishUnlabeledDoc, function(root) {
            localStorage['languageSwitching'] = 'true';
            this.setAvailableVoices();
            // Disable sub-node-switching.
            LanguageSwitching.sub_node_switching_enabled_ = false;
            mockFeedback
                .call(doCmd('jumpToTop'))
                // Expect the node's contents to be read in one language
                // (English) because sub-node switching has been disabled. Since
                // node-level detection does not run on small runs of text, like
                // the one in this test, we are falling back on the UI language
                // of the browser, which is en-US. Please see testGenPreamble
                // for more details.
                .expectSpeechWithLanguage(
                    'en-us',
                    'Hello, my name is 太田あきひろ. It\'s a pleasure to meet' +
                        ' you. どうぞよろしくお願いします.');
            mockFeedback.replay();
          });
    });


TEST_F(
    'ChromeVoxLanguageSwitchingTest',
    'SubNodeLevelSwitching_EnglishAndKoreanUnlabeledDoc_Test', function() {
      var mockFeedback = this.createMockFeedback();
      this.runWithLoadedTree(this.englishAndKoreanUnlabeledDoc, function(root) {
        localStorage['languageSwitching'] = 'true';
        this.setAvailableVoices();
        // We are able to separate out English and Korean because they use
        // different scripts.
        mockFeedback
            .call(doCmd('jumpToTop'))
            // LanguageSwitching.currentLanguage_ is initialized to 'en'. Do not
            // prepend 'English' because language does not switch.
            .expectSpeechWithLanguage('en', 'This text is written in English. ')
            .expectSpeechWithLanguage('ko', '한국어: 차에 한하여 중임할 수. ')
            // Expect 'en-us' because sub-node language of 'en' doesn't come
            // with high enough probability. We fall back on node-level detected
            // language, which is 'en-us'.
            .expectSpeechWithLanguage(
                'en', 'English: This text is also written in English.');
        mockFeedback.replay();
      });
    });

TEST_F(
    'ChromeVoxLanguageSwitchingTest',
    'NodeLevelSwitching_EnglishAndKoreanUnlabeledDoc_Test', function() {
      var mockFeedback = this.createMockFeedback();
      this.runWithLoadedTree(this.englishAndKoreanUnlabeledDoc, function(root) {
        localStorage['languageSwitching'] = 'true';
        this.setAvailableVoices();
        // Disable sub-node-switching
        LanguageSwitching.sub_node_switching_enabled_ = false;
        mockFeedback.call(doCmd('jumpToTop'))
            .expectSpeechWithLanguage(
                'en-us',
                'This text is written in English. 차에 한하여 중임할 수.' +
                    ' This text is also written in English.');
        mockFeedback.replay();
      });
    });


TEST_F(
    'ChromeVoxLanguageSwitchingTest',
    'SubNodeLevelSwitching_EnglishAndFrenchUnlabeledDoc_Test', function() {
      var mockFeedback = this.createMockFeedback();
      this.runWithLoadedTree(this.englishAndFrenchUnlabeledDoc, function(root) {
        localStorage['languageSwitching'] = 'true';
        this.setAvailableVoices();
        // Unable to separate out English and French when unlabeled.
        mockFeedback
            .call(doCmd('jumpToTop'))
            // LanguageSwitching.currentLanguage_ is initialized to 'en'. Do not
            // prepend 'English' because language does not switch.
            .expectSpeechWithLanguage(
                'en',
                'This entire object should be read in English, even' +
                    ' the following French passage: salut mon ami! Ca va? Bien, et toi? It\'s hard to' +
                    ' differentiate between latin-based languages.');
        mockFeedback.replay();
      });
    });

TEST_F(
    'ChromeVoxLanguageSwitchingTest',
    'NodeLevelSwitching_EnglishAndFrenchUnlabeledDoc_Test', function() {
      var mockFeedback = this.createMockFeedback();
      this.runWithLoadedTree(this.englishAndFrenchUnlabeledDoc, function(root) {
        localStorage['languageSwitching'] = 'true';
        this.setAvailableVoices();
        LanguageSwitching.sub_node_switching_enabled_ = false;
        mockFeedback.call(doCmd('jumpToTop'))
            .expectSpeechWithLanguage(
                'en',
                'This entire object should be read in English, even' +
                    ' the following French passage: salut mon ami! Ca va? Bien, et toi? It\'s hard to' +
                    ' differentiate between latin-based languages.');
        mockFeedback.replay();
      });
    });


TEST_F(
    'ChromeVoxLanguageSwitchingTest',
    'SubNodeLevelSwitching_JapaneseCharacterUnlabeledDoc_Test', function() {
      var mockFeedback = this.createMockFeedback();
      this.runWithLoadedTree(
          this.japaneseCharacterUnlabeledDoc, function(root) {
            localStorage['languageSwitching'] = 'true';
            this.setAvailableVoices();
            // We are able to detect and switch at the character level if the
            // character is unique to a certian script. In this case, 'ど' only
            // appears in Japanese, and therefore we can confidently switch
            // languages.
            mockFeedback.call(doCmd('jumpToTop'))
                .expectSpeechWithLanguage('ja', '日本語: ど');
            mockFeedback.replay();
          });
    });

TEST_F(
    'ChromeVoxLanguageSwitchingTest',
    'NodeLevelSwitching_JapaneseCharacterUnlabeledDoc_Test', function() {
      var mockFeedback = this.createMockFeedback();
      this.runWithLoadedTree(
          this.japaneseCharacterUnlabeledDoc, function(root) {
            localStorage['languageSwitching'] = 'true';
            this.setAvailableVoices();
            LanguageSwitching.sub_node_switching_enabled_ = false;
            mockFeedback.call(doCmd('jumpToTop'))
                .expectSpeechWithLanguage('en-us', 'ど');
            mockFeedback.replay();
          });
    });


TEST_F(
    'ChromeVoxLanguageSwitchingTest',
    'SubNodeLevelSwitching_JapaneseAndChineseUnlabeledDoc_Test', function() {
      var mockFeedback = this.createMockFeedback();
      this.runWithLoadedTree(this.japaneseAndChineseUnlabeledDoc, function(root) {
        localStorage['languageSwitching'] = 'true';
        this.setAvailableVoices();
        // Unable to separate out Japanese and Chinese if unlabeled.
        mockFeedback.call(doCmd('jumpToTop'))
            .expectSpeechWithLanguage(
                'ja',
                '日本語: 天気はいいですね. 右万諭全中結社原済権人点掲年難出面者会追');
        mockFeedback.replay();
      });
    });

TEST_F(
    'ChromeVoxLanguageSwitchingTest',
    'NodeLevelSwitching_JapaneseAndChineseUnlabeledDoc_Test', function() {
      var mockFeedback = this.createMockFeedback();
      this.runWithLoadedTree(this.japaneseAndChineseUnlabeledDoc, function(root) {
        localStorage['languageSwitching'] = 'true';
        this.setAvailableVoices();
        LanguageSwitching.sub_node_switching_enabled_ = false;
        mockFeedback.call(doCmd('jumpToTop'))
            .expectSpeechWithLanguage(
                'en-us',
                '天気はいいですね. 右万諭全中結社原済権人点掲年難出面者会追');
        mockFeedback.replay();
      });
    });

TEST_F(
    'ChromeVoxLanguageSwitchingTest',
    'NodeLevelSwitching_JapaneseAndChineseLabeledDoc_Test', function() {
      var mockFeedback = this.createMockFeedback();
      // Only difference between doc used in this test and
      // this.japaneseAndChineseUnlabeledDoc is the lang="zh" attribute.
      this.runWithLoadedTree(
          `
        <meta charset="utf-8">
        <p lang="zh">
          天気はいいですね. 右万諭全中結社原済権人点掲年難出面者会追
        </p>
    `,
          function(root) {
            localStorage['languageSwitching'] = 'true';
            this.setAvailableVoices();
            LanguageSwitching.sub_node_switching_enabled_ = false;
            mockFeedback.call(doCmd('jumpToTop'))
                .expectSpeechWithLanguage(
                    'zh',
                    '中文: 天気はいいですね. 右万諭全中結社原済権人点掲年難出面者会追');
            mockFeedback.replay();
          });
    });


TEST_F(
    'ChromeVoxLanguageSwitchingTest',
    'SubNodeLevelSwitching_JapaneseAndKoreanUnlabeledDoc_Test', function() {
      var mockFeedback = this.createMockFeedback();
      this.runWithLoadedTree(this.japaneseAndKoreanUnlabeledDoc, function(root) {
        localStorage['languageSwitching'] = 'true';
        this.setAvailableVoices();
        // Unable to separate out Japanese and Korean if unlabeled.
        mockFeedback.call(doCmd('jumpToTop'))
            .expectSpeechWithLanguage(
                'ko',
                '한국어: 私は. 법률이 정하는 바에 의하여 대법관이 아닌 법관을 둘 수 있다');
        mockFeedback.replay();
      });
    });

TEST_F(
    'ChromeVoxLanguageSwitchingTest',
    'NodeLevelSwitching_JapaneseAndKoreanUnlabeledDoc_Test', function() {
      var mockFeedback = this.createMockFeedback();
      this.runWithLoadedTree(this.japaneseAndKoreanUnlabeledDoc, function(root) {
        localStorage['languageSwitching'] = 'true';
        this.setAvailableVoices();
        LanguageSwitching.sub_node_switching_enabled_ = false;
        // Node-level language detection runs and assigns language of 'ko' to
        // the node.
        mockFeedback.call(doCmd('jumpToTop'))
            .expectSpeechWithLanguage(
                'ko',
                '한국어: 私は. 법률이 정하는 바에 의하여 대법관이 아닌 법관을 둘 수' +
                    ' 있다');
        mockFeedback.replay();
      });
    });


TEST_F(
    'ChromeVoxLanguageSwitchingTest',
    'SubNodeLevelSwitching_AsturianAndJapaneseDoc_Test', function() {
      var mockFeedback = this.createMockFeedback();
      this.runWithLoadedTree(this.asturianAndJapaneseDoc, function(root) {
        localStorage['languageSwitching'] = 'true';
        this.setAvailableVoices();
        mockFeedback.call(doCmd('jumpToTop'))
            .expectSpeechWithLanguage('ja', '日本語: ど')
            .call(doCmd('nextObject'))
            .expectSpeechWithLanguage(
                'ast',
                'asturianu: Pretend that this text is Asturian. Testing' +
                    ' three-letter language code logic.');
        mockFeedback.replay();
      });
    });

TEST_F(
    'ChromeVoxLanguageSwitchingTest',
    'NodeLevelSwitching_AsturianAndJapaneseDoc_Test', function() {
      var mockFeedback = this.createMockFeedback();
      this.runWithLoadedTree(this.asturianAndJapaneseDoc, function(root) {
        localStorage['languageSwitching'] = 'true';
        this.setAvailableVoices();
        LanguageSwitching.sub_node_level_switching_enabled_ = false;
        mockFeedback.call(doCmd('jumpToTop'))
            .expectSpeechWithLanguage('ja', '日本語: ど')
            .call(doCmd('nextObject'))
            .expectSpeechWithLanguage(
                'ast',
                'asturianu: Pretend that this text is Asturian. Testing' +
                    ' three-letter language code logic.');
        mockFeedback.replay();
      });
    });


// This does not need partner tests because no language switching behavior is
// tested.
TEST_F(
    'ChromeVoxLanguageSwitchingTest', 'LanguageSwitchingOffTest', function() {
      var mockFeedback = this.createMockFeedback();
      this.runWithLoadedTree(this.multipleLanguagesLabeledDoc, function(root) {
        localStorage['languageSwitching'] = 'false';
        this.setAvailableVoices();
        // Language should not be set if the language switching feature is off.
        mockFeedback.call(doCmd('jumpToTop'))
            .expectSpeechWithLanguage(undefined, 'Hola.')
            .call(doCmd('nextObject'))
            .expectSpeechWithLanguage(undefined, 'Hello.')
            .call(doCmd('nextObject'))
            .expectSpeechWithLanguage(undefined, 'Salut.')
            .call(doCmd('nextObject'))
            .expectSpeechWithLanguage(undefined, 'Ciao amico.');
        mockFeedback.replay();
      });
    });

TEST_F('ChromeVoxLanguageSwitchingTest', 'DefaultToUILanguageTest', function() {
  var mockFeedback = this.createMockFeedback();
  this.runWithLoadedTree(
      this.japaneseAndInvalidLanguagesLabeledDoc, function(root) {
        localStorage['languageSwitching'] = 'true';
        this.setAvailableVoices();
        LanguageSwitching.sub_node_level_switching_enabled_ = false;
        // Default to browser UI language, 'en-us', instead of defaulting to the
        // language we last switched to.
        mockFeedback.call(doCmd('jumpToTop'))
            .expectSpeechWithLanguage(
                'ja', '日本語: どうぞよろしくお願いします')
            .call(doCmd('nextObject'))
            .expectSpeechWithLanguage('en-us', 'English: Test')
            .call(doCmd('nextObject'))
            .expectSpeechWithLanguage('en-us', 'Yikes');
        mockFeedback.replay();
      });
});

TEST_F('ChromeVoxLanguageSwitchingTest', 'NoAvailableVoicesTest', function() {
  var mockFeedback = this.createMockFeedback();
  this.runWithLoadedTree(this.vietnameseAndUrduLabeledDoc, function(root) {
    localStorage['languageSwitching'] = 'true';
    this.setAvailableVoices();
    LanguageSwitching.sub_node_level_switching_enabled_ = false;
    mockFeedback.call(doCmd('jumpToTop'))
        .expectSpeechWithLanguage(
            'en-us', 'No voice available for language: Vietnamese')
        .call(doCmd('nextObject'))
        .expectSpeechWithLanguage(
            'en-us', 'No voice available for language: Urdu');
    mockFeedback.replay();
  });
});
