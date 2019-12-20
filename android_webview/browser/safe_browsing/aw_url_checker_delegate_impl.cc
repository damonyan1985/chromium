// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "android_webview/browser/safe_browsing/aw_url_checker_delegate_impl.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "android_webview/browser/aw_browser_context.h"
#include "android_webview/browser/aw_contents_client_bridge.h"
#include "android_webview/browser/aw_contents_io_thread_client.h"
#include "android_webview/browser/network_service/aw_web_resource_request.h"
#include "android_webview/browser/safe_browsing/aw_safe_browsing_ui_manager.h"
#include "android_webview/browser/safe_browsing/aw_safe_browsing_whitelist_manager.h"
#include "android_webview/browser_jni_headers/AwSafeBrowsingConfigHelper_jni.h"
#include "base/android/jni_android.h"
#include "base/bind.h"
#include "base/task/post_task.h"
#include "components/safe_browsing/db/database_manager.h"
#include "components/safe_browsing/db/v4_protocol_manager_util.h"
#include "components/safe_browsing/web_ui/constants.h"
#include "components/security_interstitials/content/unsafe_resource.h"
#include "components/security_interstitials/core/urls.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/url_constants.h"

namespace android_webview {

AwUrlCheckerDelegateImpl::AwUrlCheckerDelegateImpl(
    scoped_refptr<safe_browsing::SafeBrowsingDatabaseManager> database_manager,
    scoped_refptr<AwSafeBrowsingUIManager> ui_manager,
    AwSafeBrowsingWhitelistManager* whitelist_manager)
    : database_manager_(std::move(database_manager)),
      ui_manager_(std::move(ui_manager)),
      threat_types_(safe_browsing::CreateSBThreatTypeSet(
          {safe_browsing::SB_THREAT_TYPE_URL_MALWARE,
           safe_browsing::SB_THREAT_TYPE_URL_PHISHING,
           safe_browsing::SB_THREAT_TYPE_URL_UNWANTED,
           safe_browsing::SB_THREAT_TYPE_BILLING})),
      whitelist_manager_(whitelist_manager) {}

AwUrlCheckerDelegateImpl::~AwUrlCheckerDelegateImpl() = default;

void AwUrlCheckerDelegateImpl::MaybeDestroyPrerenderContents(
    content::WebContents::OnceGetter web_contents_getter) {}

void AwUrlCheckerDelegateImpl::StartDisplayingBlockingPageHelper(
    const security_interstitials::UnsafeResource& resource,
    const std::string& method,
    const net::HttpRequestHeaders& headers,
    bool is_main_frame,
    bool has_user_gesture) {
  AwWebResourceRequest request(resource.url.spec(), method, is_main_frame,
                               has_user_gesture, headers);

  base::PostTask(
      FROM_HERE, {content::BrowserThread::UI},
      base::BindOnce(&AwUrlCheckerDelegateImpl::StartApplicationResponse,
                     ui_manager_, resource, std::move(request)));
}

bool AwUrlCheckerDelegateImpl::IsUrlWhitelisted(const GURL& url) {
  return whitelist_manager_->IsURLWhitelisted(url);
}

bool AwUrlCheckerDelegateImpl::ShouldSkipRequestCheck(
    content::ResourceContext* resource_context,
    const GURL& original_url,
    int frame_tree_node_id,
    int render_process_id,
    int render_frame_id,
    bool originated_from_service_worker) {
  std::unique_ptr<AwContentsIoThreadClient> client;

  if (originated_from_service_worker) {
    client = AwContentsIoThreadClient::GetServiceWorkerIoThreadClient();
  } else if (render_process_id == -1 || render_frame_id == -1) {
    client = AwContentsIoThreadClient::FromID(frame_tree_node_id);
  } else {
    client =
        AwContentsIoThreadClient::FromID(render_process_id, render_frame_id);
  }

  // If Safe Browsing is disabled by the app, skip the check. Default to
  // performing the check if we can't find the |client|, since the |client| may
  // be null for some service worker requests (see https://crbug.com/979321).
  bool safe_browsing_enabled = client ? client->GetSafeBrowsingEnabled() : true;
  if (!safe_browsing_enabled)
    return true;

  // If this is a hardcoded WebUI URL we use for testing, do not skip the safe
  // browsing check. We do not check user consent here because we do not ever
  // send such URLs to GMS anyway. It's important to ignore user consent in this
  // case because the GMS APIs we rely on to check user consent often get
  // confused during CTS tests, reporting the user has not consented regardless
  // of the on-device setting. See https://crbug.com/938538.
  bool is_hardcoded_url =
      original_url.SchemeIs(content::kChromeUIScheme) &&
      original_url.host() == safe_browsing::kChromeUISafeBrowsingHost;
  if (is_hardcoded_url)
    return false;

  // For other requests, follow user consent.
  JNIEnv* env = base::android::AttachCurrentThread();
  bool safe_browsing_user_consent =
      Java_AwSafeBrowsingConfigHelper_getSafeBrowsingUserOptIn(env);
  return !safe_browsing_user_consent;
}

void AwUrlCheckerDelegateImpl::NotifySuspiciousSiteDetected(
    const base::RepeatingCallback<content::WebContents*()>&
        web_contents_getter) {}

const safe_browsing::SBThreatTypeSet&
AwUrlCheckerDelegateImpl::GetThreatTypes() {
  return threat_types_;
}

safe_browsing::SafeBrowsingDatabaseManager*
AwUrlCheckerDelegateImpl::GetDatabaseManager() {
  return database_manager_.get();
}

safe_browsing::BaseUIManager* AwUrlCheckerDelegateImpl::GetUIManager() {
  return ui_manager_.get();
}

// static
void AwUrlCheckerDelegateImpl::StartApplicationResponse(
    scoped_refptr<AwSafeBrowsingUIManager> ui_manager,
    const security_interstitials::UnsafeResource& resource,
    const AwWebResourceRequest& request) {
  content::WebContents* web_contents = resource.web_contents_getter.Run();
  AwContentsClientBridge* client =
      AwContentsClientBridge::FromWebContents(web_contents);

  if (client) {
    base::OnceCallback<void(SafeBrowsingAction, bool)> callback =
        base::BindOnce(&AwUrlCheckerDelegateImpl::DoApplicationResponse,
                       ui_manager, resource);

    client->OnSafeBrowsingHit(request, resource.threat_type,
                              std::move(callback));
  }
}

// static
void AwUrlCheckerDelegateImpl::DoApplicationResponse(
    scoped_refptr<AwSafeBrowsingUIManager> ui_manager,
    const security_interstitials::UnsafeResource& resource,
    SafeBrowsingAction action,
    bool reporting) {
  content::WebContents* web_contents = resource.web_contents_getter.Run();

  if (!reporting) {
    AwBrowserContext* browser_context =
        AwBrowserContext::FromWebContents(web_contents);
    browser_context->SetExtendedReportingAllowed(false);
  }

  // TODO(ntfschr): fully handle reporting once we add support (crbug/688629)
  bool proceed;
  switch (action) {
    case SafeBrowsingAction::SHOW_INTERSTITIAL:
      base::PostTask(
          FROM_HERE, {content::BrowserThread::UI},
          base::BindOnce(
              &AwUrlCheckerDelegateImpl::StartDisplayingDefaultBlockingPage,
              ui_manager, resource));
      return;
    case SafeBrowsingAction::PROCEED:
      proceed = true;
      break;
    case SafeBrowsingAction::BACK_TO_SAFETY:
      proceed = false;
      break;
    default:
      NOTREACHED();
  }

  content::NavigationEntry* entry = resource.GetNavigationEntryForResource();
  GURL main_frame_url = entry ? entry->GetURL() : GURL();

  // Navigate back for back-to-safety on subresources
  if (!proceed && resource.is_subframe) {
    if (web_contents->GetController().CanGoBack()) {
      web_contents->GetController().GoBack();
    } else {
      web_contents->GetController().LoadURL(
          ui_manager->default_safe_page(), content::Referrer(),
          ui::PAGE_TRANSITION_AUTO_TOPLEVEL, std::string());
    }
  }

  ui_manager->OnBlockingPageDone(
      std::vector<security_interstitials::UnsafeResource>{resource}, proceed,
      web_contents, main_frame_url);
}

// static
void AwUrlCheckerDelegateImpl::StartDisplayingDefaultBlockingPage(
    scoped_refptr<AwSafeBrowsingUIManager> ui_manager,
    const security_interstitials::UnsafeResource& resource) {
  content::WebContents* web_contents = resource.web_contents_getter.Run();
  if (web_contents) {
    ui_manager->DisplayBlockingPage(resource);
    return;
  }

  // Reporting back that it is not okay to proceed with loading the URL.
  base::PostTask(FROM_HERE, {content::BrowserThread::IO},
                 base::BindOnce(resource.callback, false));
}

}  // namespace android_webview
