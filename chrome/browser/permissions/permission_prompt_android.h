// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PERMISSIONS_PERMISSION_PROMPT_ANDROID_H_
#define CHROME_BROWSER_PERMISSIONS_PERMISSION_PROMPT_ANDROID_H_

#include <memory>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/strings/string16.h"
#include "chrome/browser/permissions/permission_request_notification_android.h"
#include "chrome/browser/ui/permission_bubble/permission_prompt.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/infobars/core/infobar_manager.h"

namespace content {
class WebContents;
}
namespace infobars {
class InfoBar;
}
class PermissionRequestNotificationAndroid;

class PermissionPromptAndroid : public PermissionPrompt,
                                public infobars::InfoBarManager::Observer {
 public:
  PermissionPromptAndroid(content::WebContents* web_contents,
                          Delegate* delegate);
  ~PermissionPromptAndroid() override;

  // PermissionPrompt:
  void UpdateAnchorPosition() override;
  TabSwitchingBehavior GetTabSwitchingBehavior() override;

  void Closing();
  void Accept();
  void Deny();

  // We show one permission at a time except for grouped mic+camera, for which
  // we still have a single icon and message text.
  size_t PermissionCount() const;
  ContentSettingsType GetContentSettingType(size_t position) const;
  int GetIconId() const;
  base::string16 GetTitleText() const;
  base::string16 GetMessageText() const;

  const content::WebContents* web_contents() { return web_contents_; }

  // InfoBar::Manager:
  void OnInfoBarRemoved(infobars::InfoBar* infobar, bool animate) override;
  void OnManagerShuttingDown(infobars::InfoBarManager* manager) override;

 private:
  // PermissionPromptAndroid is owned by PermissionRequestManager, so it should
  // be safe to hold a raw WebContents pointer here because this class is
  // destroyed before the WebContents.
  content::WebContents* web_contents_;
  // |delegate_| is the PermissionRequestManager, which owns this object.
  Delegate* delegate_;

  // The permission requestion notification used to display the permission
  // request, if displayed in that format.
  std::unique_ptr<PermissionRequestNotificationAndroid>
      permission_request_notification_;

  // The infobar used to display the permission request, if displayed in that
  // format. Never assume that this pointer is currently alive.
  infobars::InfoBar* permission_infobar_;

  base::WeakPtrFactory<PermissionPromptAndroid> weak_factory_{this};

  DISALLOW_COPY_AND_ASSIGN(PermissionPromptAndroid);
};

#endif  // CHROME_BROWSER_PERMISSIONS_PERMISSION_PROMPT_ANDROID_H_
