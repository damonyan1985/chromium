// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/spellcheck/browser/spellcheck_platform.h"

#include <stddef.h>

#include "base/bind.h"
#include "base/run_loop.h"
#include "base/single_thread_task_runner.h"
#include "base/stl_util.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/task_environment.h"
#include "base/win/windows_version.h"
#include "components/spellcheck/common/spellcheck_features.h"
#include "components/spellcheck/common/spellcheck_result.h"
#include "content/public/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

class SpellcheckPlatformWinTest : public testing::Test {
 public:
  void RunUntilResultReceived() {
    if (callback_finished_)
      return;
    base::RunLoop run_loop;
    quit_ = run_loop.QuitClosure();
    run_loop.Run();

    // reset status
    callback_finished_ = false;
  }

  void SetLanguageCompletionCallback(bool result) {
    set_language_result_ = result;
    callback_finished_ = true;
    if (quit_)
      std::move(quit_).Run();
  }

  void TextCheckCompletionCallback(
      const std::vector<SpellCheckResult>& results) {
    callback_finished_ = true;
    spell_check_results_ = results;
    if (quit_)
      std::move(quit_).Run();
  }

  void GetSuggestionsCompletionCallback(
      const spellcheck::PerLanguageSuggestions& suggestions) {
    callback_finished_ = true;
    spell_check_suggestions_ = suggestions;
    if (quit_)
      std::move(quit_).Run();
  }

#if BUILDFLAG(USE_WINDOWS_PREFERRED_LANGUAGES_FOR_SPELLCHECK)
  void RetrieveSupportedWindowsPreferredLanguagesCallback(
      const std::vector<std::string>& preferred_languages) {
    callback_finished_ = true;
    preferred_languages_ = preferred_languages;
    for (const auto& preferred_language : preferred_languages_) {
      DLOG(INFO) << "RetrieveSupportedWindowsPreferredLanguagesCallback: "
                    "Dictionary supported for locale: "
                 << preferred_language;
    }
    if (quit_)
      std::move(quit_).Run();
  }
#endif  // BUILDFLAG(USE_WINDOWS_PREFERRED_LANGUAGES_FOR_SPELLCHECK

  // TODO(crbug.com/1035044) Make these methods actual tests. See the
  //                         task_environment_ comment below.
  void RequestTextCheckTests() {
    static const struct {
      const char* text_to_check;
      const char* expected_suggestion;
    } kTestCases[] = {
        {"absense", "absence"},    {"becomeing", "becoming"},
        {"cieling", "ceiling"},    {"definate", "definite"},
        {"eigth", "eight"},        {"exellent", "excellent"},
        {"finaly", "finally"},     {"garantee", "guarantee"},
        {"humerous", "humorous"},  {"imediately", "immediately"},
        {"jellous", "jealous"},    {"knowlege", "knowledge"},
        {"lenght", "length"},      {"manuever", "maneuver"},
        {"naturaly", "naturally"}, {"ommision", "omission"},
    };

    for (size_t i = 0; i < base::size(kTestCases); ++i) {
      const auto& test_case = kTestCases[i];
      const base::string16 word(base::ASCIIToUTF16(test_case.text_to_check));

      // Check if the suggested words occur.
      spellcheck_platform::RequestTextCheck(
          1, word,
          base::BindOnce(
              &SpellcheckPlatformWinTest::TextCheckCompletionCallback,
              base::Unretained(this)));
      RunUntilResultReceived();

      ASSERT_EQ(1u, spell_check_results_.size())
          << "RequestTextCheckTests case " << i << ": Wrong number of results";

      const std::vector<base::string16>& suggestions =
          spell_check_results_.front().replacements;
      const base::string16 suggested_word(
          base::ASCIIToUTF16(test_case.expected_suggestion));
      auto position =
          std::find_if(suggestions.begin(), suggestions.end(),
                       [&](const base::string16& suggestion) {
                         return suggestion.compare(suggested_word) == 0;
                       });

      ASSERT_NE(suggestions.end(), position)
          << "RequestTextCheckTests case " << i
          << ": Expected suggestion not found";
    }
  }

#if BUILDFLAG(USE_WINDOWS_PREFERRED_LANGUAGES_FOR_SPELLCHECK)
  void RetrieveSupportedWindowsPreferredLanguagesTests() {
    spellcheck_platform::RetrieveSupportedWindowsPreferredLanguages(
        base::BindOnce(&SpellcheckPlatformWinTest::
                           RetrieveSupportedWindowsPreferredLanguagesCallback,
                       base::Unretained(this)));

    RunUntilResultReceived();

    ASSERT_LE(1u, preferred_languages_.size());
    ASSERT_NE(preferred_languages_.end(),
              std::find(preferred_languages_.begin(),
                        preferred_languages_.end(), "en-US"));
  }
#endif  // BUILDFLAG(USE_WINDOWS_PREFERRED_LANGUAGES_FOR_SPELLCHECK

#if BUILDFLAG(USE_WIN_HYBRID_SPELLCHECKER)
  void HybridRequestTextCheckTests() {
    static const struct {
      const char* text_to_check;
      std::vector<SpellCheckResult> fake_renderer_results;
      bool fill_suggestions;
      size_t expected_result_count;
    } kHybridTestCases[] = {
        // Should find no mistakes.
        {"This has no spelling mistakes", {}, false, 0u},
        // Should find all 3 mistakes.
        {"Tihs has some speling mitsakes", {}, false, 3u},
        // Should find all 3 mistakes and return some spelling suggestions.
        {"Tihs has some speling mitsakes", {}, true, 3u},
        // Should find no mistakes because all words are correct on the browser
        // side, so mistakes from the renderer should be ignored.
        {"This has no spelling mistakes",
         {
             SpellCheckResult(SpellCheckResult::SPELLING, 5, 3),
             SpellCheckResult(SpellCheckResult::SPELLING, 9, 2),
         },
         false,
         0u},
        // Should find no mistakes because all words were marked correct by
        // either
        // the renderer or the browser.
        {"Tihs has some speling mitsakes",
         {
             SpellCheckResult(SpellCheckResult::SPELLING, 5, 3),
             SpellCheckResult(SpellCheckResult::SPELLING, 9, 2),
         },
         false,
         0u},
        // Should find a single mistake, "speling", because that's the only word
        // marked as misspelled by both the renderer and the browser.
        {"Tihs has some speling mitsakes",
         {
             SpellCheckResult(SpellCheckResult::SPELLING, 5, 3),
             SpellCheckResult(SpellCheckResult::SPELLING, 14, 7),
         },
         false,
         1u},
        // Should find all 3 mistakes because they were marked as misspelled by
        // both the renderer and the browser.
        {"Tihs has some speling mitsakes",
         {
             SpellCheckResult(SpellCheckResult::SPELLING, 0, 4),
             SpellCheckResult(SpellCheckResult::SPELLING, 14, 7),
             SpellCheckResult(SpellCheckResult::SPELLING, 22, 8),
         },
         false,
         3u},
        // Should find 3 mistakes. The 2 extra renderer mistakes should be
        // ignored
        // because the browser didn't mark them as misspelled.
        {"Tihs has some speling mitsakes",
         {
             SpellCheckResult(SpellCheckResult::SPELLING, 0, 4),
             SpellCheckResult(SpellCheckResult::SPELLING, 5, 3),
             SpellCheckResult(SpellCheckResult::SPELLING, 9, 2),
             SpellCheckResult(SpellCheckResult::SPELLING, 14, 7),
             SpellCheckResult(SpellCheckResult::SPELLING, 22, 8),
         },
         false,
         3u},
    };

    for (size_t i = 0; i < base::size(kHybridTestCases); ++i) {
      const auto& test_case = kHybridTestCases[i];
      const base::string16 text(base::ASCIIToUTF16(test_case.text_to_check));

      // Check if the suggested words occur.
      spellcheck_platform::RequestTextCheck(
          1, text, test_case.fake_renderer_results, test_case.fill_suggestions,
          base::BindOnce(
              &SpellcheckPlatformWinTest::TextCheckCompletionCallback,
              base::Unretained(this)));
      RunUntilResultReceived();

      ASSERT_EQ(test_case.expected_result_count, spell_check_results_.size())
          << "HybridRequestTextCheckTests case " << i
          << ": Wrong number of results";

      if (spell_check_results_.size() > 0u) {
        ASSERT_EQ(spell_check_results_[0].replacements.size() > 0,
                  test_case.fill_suggestions)
            << "HybridRequestTextCheckTests case " << i
            << ": Wrong number of suggestions";
      }
    }
  }
#endif  // BUILDFLAG(USE_WIN_HYBRID_SPELLCHECKER)

 protected:
  bool callback_finished_ = false;

  bool set_language_result_;
  std::vector<SpellCheckResult> spell_check_results_;
  spellcheck::PerLanguageSuggestions spell_check_suggestions_;
#if BUILDFLAG(USE_WINDOWS_PREFERRED_LANGUAGES_FOR_SPELLCHECK)
  std::vector<std::string> preferred_languages_;
#endif  // BUILDFLAG(USE_WINDOWS_PREFERRED_LANGUAGES_FOR_SPELLCHECK
  base::OnceClosure quit_;

  // The WindowsSpellChecker class is instantiated with static storage using
  // base::NoDestructor (see GetWindowsSpellChecker) and creates its own task
  // runner. The thread pool is destroyed together with TaskEnvironment when the
  // test fixture object is destroyed. Therefore without some elaborate
  // test-only code added to the WindowsSpellChecker class or a means to keep
  // the TaskEnvironment alive (which would set off leak detection), easiest
  // approach for now is to add all test coverage for Windows spellchecking to
  // a single test.
  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::MainThreadType::UI};
};

// TODO(crbug.com/1035044) Split this test into multiple tests instead of
//                         individual methods. See the task_environment_
//                         comment in the SpellcheckPlatformWinTest class.
TEST_F(SpellcheckPlatformWinTest, SpellCheckAsyncMethods) {
  if (!spellcheck::WindowsVersionSupportsSpellchecker()) {
    return;
  }

  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(spellcheck::kWinUseBrowserSpellChecker);

  spellcheck_platform::SetLanguage(
      "en-US",
      base::BindOnce(&SpellcheckPlatformWinTest::SetLanguageCompletionCallback,
                     base::Unretained(this)));

  RunUntilResultReceived();

  ASSERT_TRUE(set_language_result_);

  RequestTextCheckTests();

#if BUILDFLAG(USE_WINDOWS_PREFERRED_LANGUAGES_FOR_SPELLCHECK)
  RetrieveSupportedWindowsPreferredLanguagesTests();
#endif  // BUILDFLAG(USE_WINDOWS_PREFERRED_LANGUAGES_FOR_SPELLCHECK

#if BUILDFLAG(USE_WIN_HYBRID_SPELLCHECKER)
  HybridRequestTextCheckTests();
#endif  // BUILDFLAG(USE_WIN_HYBRID_SPELLCHECKER)
}

}  // namespace
