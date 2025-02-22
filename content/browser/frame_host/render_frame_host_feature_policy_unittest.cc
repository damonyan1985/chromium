// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <vector>

#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/navigation_simulator.h"
#include "content/public/test/test_renderer_host.h"
#include "content/test/test_render_frame_host.h"
#include "third_party/blink/public/common/feature_policy/feature_policy.h"
#include "third_party/blink/public/common/frame/frame_policy.h"
#include "third_party/blink/public/common/frame/sandbox_flags.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace content {

// Integration tests for feature policy setup and querying through a RFH. These
// tests are not meant to cover every edge case as the FeaturePolicy class
// itself is tested thoroughly in feature_policy_unittest.cc. Instead they are
// meant to ensure that integration with RenderFrameHost works correctly.
class RenderFrameHostFeaturePolicyTest
    : public content::RenderViewHostTestHarness {
 protected:
  static constexpr const char* kOrigin1 = "https://google.com";
  static constexpr const char* kOrigin2 = "https://maps.google.com";
  static constexpr const char* kOrigin3 = "https://example.com";
  static constexpr const char* kOrigin4 = "https://test.com";

  static const blink::mojom::FeaturePolicyFeature kDefaultEnabledFeature =
      blink::mojom::FeaturePolicyFeature::kDocumentWrite;
  static const blink::mojom::FeaturePolicyFeature kDefaultSelfFeature =
      blink::mojom::FeaturePolicyFeature::kGeolocation;
  static const blink::mojom::FeaturePolicyFeature kParameterizedFeature =
      blink::mojom::FeaturePolicyFeature::kOversizedImages;

  const blink::PolicyValue sample_double_value =
      blink::PolicyValue(2.5, blink::mojom::PolicyValueType::kDecDouble);
  const blink::PolicyValue min_double_value =
      blink::PolicyValue(2.0, blink::mojom::PolicyValueType::kDecDouble);
  const blink::PolicyValue sample_bool_value = blink::PolicyValue(true);

  RenderFrameHost* GetMainRFH(const char* origin) {
    RenderFrameHost* result = web_contents()->GetMainFrame();
    RenderFrameHostTester::For(result)->InitializeRenderFrameIfNeeded();
    SimulateNavigation(&result, GURL(origin));
    return result;
  }

  RenderFrameHost* AddChildRFH(RenderFrameHost* parent, const char* origin) {
    RenderFrameHost* result =
        RenderFrameHostTester::For(parent)->AppendChild("");
    RenderFrameHostTester::For(result)->InitializeRenderFrameIfNeeded();
    SimulateNavigation(&result, GURL(origin));
    return result;
  }

  // The header policy should only be set once on page load, so we refresh the
  // page to simulate that.
  void RefreshPageAndSetHeaderPolicy(
      RenderFrameHost** rfh,
      blink::mojom::FeaturePolicyFeature feature,
      const std::map<std::string, blink::PolicyValue>& values) {
    RenderFrameHost* current = *rfh;
    SimulateNavigation(&current, current->GetLastCommittedURL());
    static_cast<TestRenderFrameHost*>(current)->DidSetFramePolicyHeaders(
        blink::WebSandboxFlags::kNone, CreateFPHeader(feature, values));
    *rfh = current;
  }

  void SetContainerPolicy(
      RenderFrameHost* parent,
      RenderFrameHost* child,
      blink::mojom::FeaturePolicyFeature feature,
      const std::map<std::string, blink::PolicyValue>& values) {
    static_cast<TestRenderFrameHost*>(parent)->OnDidChangeFramePolicy(
        child->GetRoutingID(), {blink::WebSandboxFlags::kNone,
                                CreateFPHeader(feature, values),
                                {} /* required_document_policy */});
  }

  void SimulateNavigation(RenderFrameHost** rfh, const GURL& url) {
    auto navigation_simulator =
        NavigationSimulator::CreateRendererInitiated(url, *rfh);
    navigation_simulator->Commit();
    *rfh = navigation_simulator->GetFinalRenderFrameHost();
  }

 private:
  blink::ParsedFeaturePolicy CreateFPHeader(
      blink::mojom::FeaturePolicyFeature feature,
      const std::map<std::string, blink::PolicyValue>& values) {
    blink::ParsedFeaturePolicy result(1);
    result[0].feature = feature;
    if (feature == kParameterizedFeature) {
      result[0].fallback_value = min_double_value;
      result[0].opaque_value = min_double_value;
    }
    for (auto const& value : values)
      result[0].values.insert(std::pair<url::Origin, blink::PolicyValue>(
          url::Origin::Create(GURL(value.first)), value.second));
    return result;
  }
};

TEST_F(RenderFrameHostFeaturePolicyTest, DefaultPolicy) {
  RenderFrameHost* parent = GetMainRFH(kOrigin1);
  RenderFrameHost* child = AddChildRFH(parent, kOrigin2);

  EXPECT_TRUE(parent->IsFeatureEnabled(kDefaultEnabledFeature));
  EXPECT_TRUE(parent->IsFeatureEnabled(kDefaultSelfFeature));
  EXPECT_TRUE(parent->IsFeatureEnabled(kParameterizedFeature));
  EXPECT_TRUE(child->IsFeatureEnabled(kDefaultEnabledFeature));
  EXPECT_FALSE(child->IsFeatureEnabled(kDefaultSelfFeature));
  EXPECT_TRUE(child->IsFeatureEnabled(kParameterizedFeature));
}

TEST_F(RenderFrameHostFeaturePolicyTest, HeaderPolicy) {
  RenderFrameHost* parent = GetMainRFH(kOrigin1);

  // Enable the feature for the child in the parent frame.
  RefreshPageAndSetHeaderPolicy(&parent, kDefaultSelfFeature,
                                {{std::string(kOrigin1), sample_bool_value},
                                 {std::string(kOrigin2), sample_bool_value}});

  // Create the child.
  RenderFrameHost* child = AddChildRFH(parent, kOrigin2);

  EXPECT_TRUE(parent->IsFeatureEnabled(kDefaultSelfFeature));
  EXPECT_TRUE(child->IsFeatureEnabled(kDefaultSelfFeature));

  // Set an empty allowlist in the child to test that the policies combine
  // correctly.
  RefreshPageAndSetHeaderPolicy(&child, kDefaultSelfFeature,
                                std::map<std::string, blink::PolicyValue>());

  EXPECT_TRUE(parent->IsFeatureEnabled(kDefaultSelfFeature));
  EXPECT_FALSE(child->IsFeatureEnabled(kDefaultSelfFeature));

  // Re-enable the feature in the child.
  RefreshPageAndSetHeaderPolicy(&child, kDefaultSelfFeature,
                                {{std::string(kOrigin2), sample_bool_value}});
  EXPECT_TRUE(child->IsFeatureEnabled(kDefaultSelfFeature));

  // Navigate the child. Check that the feature is disabled.
  SimulateNavigation(&child, GURL(kOrigin3));
  EXPECT_FALSE(child->IsFeatureEnabled(kDefaultSelfFeature));
}

TEST_F(RenderFrameHostFeaturePolicyTest, ContainerPolicy) {
  RenderFrameHost* parent = GetMainRFH(kOrigin1);
  RenderFrameHost* child = AddChildRFH(parent, kOrigin2);

  // Set a container policy on origin 3 to give it the feature. It should not
  // be enabled because container policy will only take effect after navigation.
  SetContainerPolicy(parent, child, kDefaultSelfFeature,
                     {{std::string(kOrigin2), sample_bool_value},
                      {std::string(kOrigin3), sample_bool_value}});
  EXPECT_FALSE(child->IsFeatureEnabled(kDefaultSelfFeature));

  // Navigate the child so that the container policy takes effect.
  SimulateNavigation(&child, GURL(kOrigin3));
  EXPECT_TRUE(child->IsFeatureEnabled(kDefaultSelfFeature));

  // Navigate the child again, the feature should not be enabled.
  SimulateNavigation(&child, GURL(kOrigin4));
  EXPECT_FALSE(child->IsFeatureEnabled(kDefaultSelfFeature));
}

TEST_F(RenderFrameHostFeaturePolicyTest, HeaderAndContainerPolicy) {
  RenderFrameHost* parent = GetMainRFH(kOrigin1);

  // Set a header policy and container policy. Check that they both take effect.
  RefreshPageAndSetHeaderPolicy(&parent, kDefaultSelfFeature,
                                {{std::string(kOrigin1), sample_bool_value},
                                 {std::string(kOrigin2), sample_bool_value}});

  RenderFrameHost* child = AddChildRFH(parent, kOrigin2);
  SetContainerPolicy(parent, child, kDefaultSelfFeature,
                     {{std::string(kOrigin3), sample_bool_value}});

  // The feature should be enabled in kOrigin2, kOrigin3 but not kOrigin4.
  EXPECT_TRUE(child->IsFeatureEnabled(kDefaultSelfFeature));
  SimulateNavigation(&child, GURL(kOrigin3));
  EXPECT_TRUE(child->IsFeatureEnabled(kDefaultSelfFeature));
  SimulateNavigation(&child, GURL(kOrigin4));
  EXPECT_FALSE(child->IsFeatureEnabled(kDefaultSelfFeature));

  // Change the header policy to turn off the feature. It should be disabled in
  // all children.
  RefreshPageAndSetHeaderPolicy(&parent, kDefaultSelfFeature,
                                std::map<std::string, blink::PolicyValue>());
  child = AddChildRFH(parent, kOrigin2);
  SetContainerPolicy(parent, child, kDefaultSelfFeature,
                     {{std::string(kOrigin3), sample_bool_value}});

  SimulateNavigation(&child, GURL(kOrigin2));
  EXPECT_FALSE(child->IsFeatureEnabled(kDefaultSelfFeature));
  SimulateNavigation(&child, GURL(kOrigin3));
  EXPECT_FALSE(child->IsFeatureEnabled(kDefaultSelfFeature));
}

TEST_F(RenderFrameHostFeaturePolicyTest, ParameterizedHeaderPolicy) {
  RenderFrameHost* parent = GetMainRFH(kOrigin1);

  blink::PolicyValue large_double_value(
      3.5, blink::mojom::PolicyValueType::kDecDouble);
  // Enable the feature for the child in the parent frame.
  RefreshPageAndSetHeaderPolicy(&parent, kParameterizedFeature,
                                {{std::string(kOrigin1), sample_double_value},
                                 {std::string(kOrigin2), sample_double_value}});

  // Create the child.
  RenderFrameHost* child = AddChildRFH(parent, kOrigin2);

  EXPECT_TRUE(
      parent->IsFeatureEnabled(kParameterizedFeature, sample_double_value));
  EXPECT_FALSE(
      parent->IsFeatureEnabled(kParameterizedFeature, large_double_value));
  EXPECT_TRUE(
      child->IsFeatureEnabled(kParameterizedFeature, sample_double_value));
  EXPECT_FALSE(
      child->IsFeatureEnabled(kParameterizedFeature, large_double_value));

  // Set an empty allowlist and in the child to test that the policies combine
  // correctly. Child frame should be enabled since the feature default is
  // enabled for all.
  RefreshPageAndSetHeaderPolicy(&child, kDefaultSelfFeature,
                                std::map<std::string, blink::PolicyValue>());
  EXPECT_TRUE(
      child->IsFeatureEnabled(kParameterizedFeature, sample_double_value));

  // disable the feature in the child.
  RefreshPageAndSetHeaderPolicy(&child, kParameterizedFeature,
                                {{std::string(kOrigin2), min_double_value}});
  EXPECT_TRUE(child->IsFeatureEnabled(kParameterizedFeature, min_double_value));
  EXPECT_FALSE(
      child->IsFeatureEnabled(kParameterizedFeature, sample_double_value));

  // Navigate the child. Check that the feature is disabled.
  SimulateNavigation(&child, GURL(kOrigin3));
  EXPECT_FALSE(
      child->IsFeatureEnabled(kParameterizedFeature, sample_double_value));
}

TEST_F(RenderFrameHostFeaturePolicyTest, ParameterizedContainerPolicy) {
  RenderFrameHost* parent = GetMainRFH(kOrigin1);
  RenderFrameHost* child = AddChildRFH(parent, kOrigin2);

  // Set a container policy on origin 3 to give it the feature. It should not
  // be enabled because container policy will only take effect after navigation.
  SetContainerPolicy(parent, child, kParameterizedFeature,
                     {{std::string(kOrigin2), sample_double_value},
                      {std::string(kOrigin3), sample_double_value}});
  blink::PolicyValue large_double_value(
      3.0, blink::mojom::PolicyValueType::kDecDouble);
  EXPECT_TRUE(
      child->IsFeatureEnabled(kParameterizedFeature, large_double_value));

  // Navigate the child so that the container policy takes effect.
  SimulateNavigation(&child, GURL(kOrigin3));
  EXPECT_FALSE(
      child->IsFeatureEnabled(kParameterizedFeature, large_double_value));
  EXPECT_TRUE(
      child->IsFeatureEnabled(kParameterizedFeature, sample_double_value));

  // Navigate the child again, the feature should be disabled by container
  // policy.
  SimulateNavigation(&child, GURL(kOrigin4));
  EXPECT_FALSE(
      child->IsFeatureEnabled(kParameterizedFeature, sample_double_value));
}

TEST_F(RenderFrameHostFeaturePolicyTest,
       ParameterizedHeaderAndContainerPolicy) {
  RenderFrameHost* parent = GetMainRFH(kOrigin1);

  // Set a header policy and container policy. Check that they both take effect.
  RefreshPageAndSetHeaderPolicy(&parent, kParameterizedFeature,
                                {{std::string(kOrigin1), sample_double_value},
                                 {std::string(kOrigin2), sample_double_value}});

  RenderFrameHost* child = AddChildRFH(parent, kOrigin2);
  SetContainerPolicy(parent, child, kParameterizedFeature,
                     {{std::string(kOrigin3), sample_double_value}});

  // The feature should be enabled in kOrigin2, kOrigin3 but not kOrigin4.
  EXPECT_TRUE(
      child->IsFeatureEnabled(kParameterizedFeature, sample_double_value));
  SimulateNavigation(&child, GURL(kOrigin3));
  EXPECT_TRUE(
      child->IsFeatureEnabled(kParameterizedFeature, sample_double_value));
  SimulateNavigation(&child, GURL(kOrigin4));
  EXPECT_FALSE(
      child->IsFeatureEnabled(kParameterizedFeature, sample_double_value));

  // Change the header policy to turn off the feature. It should be disabled in
  // all children.
  RefreshPageAndSetHeaderPolicy(&parent, kParameterizedFeature,
                                std::map<std::string, blink::PolicyValue>());
  child = AddChildRFH(parent, kOrigin2);
  SetContainerPolicy(parent, child, kParameterizedFeature,
                     {{std::string(kOrigin3), sample_double_value}});

  SimulateNavigation(&child, GURL(kOrigin2));
  EXPECT_FALSE(
      child->IsFeatureEnabled(kParameterizedFeature, sample_double_value));
  SimulateNavigation(&child, GURL(kOrigin3));
  EXPECT_FALSE(
      child->IsFeatureEnabled(kParameterizedFeature, sample_double_value));
}

}  // namespace content
