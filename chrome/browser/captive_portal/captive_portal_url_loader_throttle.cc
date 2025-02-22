// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/captive_portal/captive_portal_url_loader_throttle.h"

#include "chrome/browser/captive_portal/captive_portal_tab_helper.h"

CaptivePortalURLLoaderThrottle::CaptivePortalURLLoaderThrottle(
    content::WebContents* web_contents) {
  is_captive_portal_window_ =
      web_contents && CaptivePortalTabHelper::FromWebContents(web_contents) &&
      CaptivePortalTabHelper::FromWebContents(web_contents)
          ->is_captive_portal_window();
}

void CaptivePortalURLLoaderThrottle::WillStartRequest(
    network::ResourceRequest* request,
    bool* defer) {
  if (!is_captive_portal_window_)
    return;

  if (!request->trusted_params)
    request->trusted_params = network::ResourceRequest::TrustedParams();
  request->trusted_params->disable_secure_dns = true;
}
