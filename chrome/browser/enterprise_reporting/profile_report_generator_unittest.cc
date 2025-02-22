// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/enterprise_reporting/profile_report_generator.h"

#include "base/strings/string16.h"
#include "base/strings/utf_string_conversions.h"
#include "base/util/values/values_util.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/signin/identity_test_environment_profile_adaptor.h"
#include "chrome/common/extensions/extension_constants.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/testing_browser_process.h"
#include "chrome/test/base/testing_profile_manager.h"
#include "components/account_id/account_id.h"
#include "components/policy/core/common/mock_policy_service.h"
#include "components/policy/core/common/policy_map.h"
#include "components/signin/public/identity_manager/identity_test_environment.h"
#include "content/public/test/browser_task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace enterprise_reporting {
namespace {

constexpr char kProfile[] = "Profile";
constexpr char kIdleProfile[] = "IdleProfile";
constexpr char kExtensionId[] = "abcdefghijklmnopabcdefghijklmnop";
constexpr int kFakeTime = 123456;

}  // namespace

class ProfileReportGeneratorTest : public ::testing::Test {
 public:
  ProfileReportGeneratorTest()
      : profile_manager_(TestingBrowserProcess::GetGlobal()) {}
  ~ProfileReportGeneratorTest() override = default;

  void SetUp() override {
    ASSERT_TRUE(profile_manager_.SetUp());
    InitMockPolicyService();
    InitPolicyMap();

    profile_ = profile_manager_.CreateTestingProfile(
        kProfile, {}, base::UTF8ToUTF16(kProfile), 0, {},
        IdentityTestEnvironmentProfileAdaptor::
            GetIdentityTestEnvironmentFactories(),
        base::nullopt, std::move(policy_service_));
  }

  void InitMockPolicyService() {
    policy_service_ = std::make_unique<policy::MockPolicyService>();

    ON_CALL(*policy_service_.get(),
            GetPolicies(::testing::Eq(policy::PolicyNamespace(
                policy::POLICY_DOMAIN_CHROME, std::string()))))
        .WillByDefault(::testing::ReturnRef(policy_map_));
  }

  void InitPolicyMap() {
    policy_map_.Set("kPolicyName1", policy::POLICY_LEVEL_MANDATORY,
                    policy::POLICY_SCOPE_USER, policy::POLICY_SOURCE_CLOUD,
                    std::make_unique<base::Value>(std::vector<base::Value>()),
                    nullptr);
    policy_map_.Set("kPolicyName2", policy::POLICY_LEVEL_RECOMMENDED,
                    policy::POLICY_SCOPE_MACHINE, policy::POLICY_SOURCE_MERGED,
                    std::make_unique<base::Value>(true), nullptr);
  }

  std::unique_ptr<em::ChromeUserProfileInfo> GenerateReport(
      const base::FilePath& path,
      const std::string& name) {
    std::unique_ptr<em::ChromeUserProfileInfo> report =
        generator_.MaybeGenerate(path, name);
    return report;
  }

  std::unique_ptr<em::ChromeUserProfileInfo> GenerateReport() {
    auto report =
        GenerateReport(profile()->GetPath(), profile()->GetProfileUserName());
    EXPECT_TRUE(report);
    EXPECT_EQ(profile()->GetProfileUserName(), report->name());
    EXPECT_EQ(profile()->GetPath().AsUTF8Unsafe(), report->id());
    EXPECT_TRUE(report->is_full_report());

    return report;
  }

  void SetExtensionToPendingList(const std::vector<std::string>& ids) {
    std::unique_ptr<base::Value> id_values =
        std::make_unique<base::Value>(base::Value::Type::DICTIONARY);
    for (const auto& id : ids) {
      base::Value request_data(base::Value::Type::DICTIONARY);
      request_data.SetKey(
          extension_misc::kExtensionRequestTimestamp,
          ::util::TimeToValue(base::Time::FromJavaTime(kFakeTime)));
      id_values->SetKey(id, std::move(request_data));
    }
    profile()->GetTestingPrefService()->SetUserPref(
        prefs::kCloudExtensionRequestIds, std::move(id_values));
  }

  TestingProfile* profile() { return profile_; }
  TestingProfileManager* profile_manager() { return &profile_manager_; }

  ProfileReportGenerator generator_;

 private:
  content::BrowserTaskEnvironment task_environment_;
  TestingProfileManager profile_manager_;
  TestingProfile* profile_;

  std::unique_ptr<policy::MockPolicyService> policy_service_;
  policy::PolicyMap policy_map_;

  DISALLOW_COPY_AND_ASSIGN(ProfileReportGeneratorTest);
};

TEST_F(ProfileReportGeneratorTest, ProfileNotActivated) {
  const base::FilePath profile_path =
      profile_manager()->profiles_dir().AppendASCII(kIdleProfile);
  profile_manager()->profile_attributes_storage()->AddProfile(
      profile_path, base::ASCIIToUTF16(kIdleProfile), std::string(),
      base::string16(), false, 0, std::string(), EmptyAccountId());
  std::unique_ptr<em::ChromeUserProfileInfo> response =
      generator_.MaybeGenerate(profile_path, kIdleProfile);
  ASSERT_FALSE(response.get());
}

TEST_F(ProfileReportGeneratorTest, UnsignedInProfile) {
  auto report = GenerateReport();
  EXPECT_FALSE(report->has_chrome_signed_in_user());
}

TEST_F(ProfileReportGeneratorTest, SignedInProfile) {
  IdentityTestEnvironmentProfileAdaptor identity_test_env_adaptor(profile());
  auto expected_info =
      identity_test_env_adaptor.identity_test_env()->SetPrimaryAccount(
          "test@mail.com");
  auto report = GenerateReport();
  EXPECT_TRUE(report->has_chrome_signed_in_user());
  EXPECT_EQ(expected_info.email, report->chrome_signed_in_user().email());
  EXPECT_EQ(expected_info.gaia,
            report->chrome_signed_in_user().obfudscated_gaia_id());
}

TEST_F(ProfileReportGeneratorTest, PoliciesDisabled) {
  // Users' profile info is collected by default.
  std::unique_ptr<em::ChromeUserProfileInfo> report = GenerateReport();
  EXPECT_EQ(2, report->chrome_policies_size());

  // Stop to collect profile info after |set_policies_enabled| is set as false.
  generator_.set_policies_enabled(false);
  report = GenerateReport();
  EXPECT_EQ(0, report->chrome_policies_size());

  // Start to collect profile info again after |set_policies_enabled| is set as
  // true.
  generator_.set_policies_enabled(true);
  report = GenerateReport();
  EXPECT_EQ(2, report->chrome_policies_size());
}

TEST_F(ProfileReportGeneratorTest, PendingRequest) {
  generator_.set_extension_request_enabled(true);
  std::vector<std::string> ids = {kExtensionId};
  SetExtensionToPendingList(ids);

  auto report = GenerateReport();
  ASSERT_EQ(1, report->extension_requests_size());
  EXPECT_EQ(kExtensionId, report->extension_requests(0).id());
  EXPECT_EQ(kFakeTime, report->extension_requests(0).request_timestamp());
}

TEST_F(ProfileReportGeneratorTest, NoPendingRequestWhenItsDisabled) {
  generator_.set_extension_request_enabled(false);
  std::vector<std::string> ids = {kExtensionId};
  SetExtensionToPendingList(ids);

  auto report = GenerateReport();
  EXPECT_EQ(0, report->extension_requests_size());
}

}  // namespace enterprise_reporting
