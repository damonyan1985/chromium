// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_ASH_LAUNCHER_APP_SERVICE_APP_SERVICE_INSTANCE_REGISTRY_HELPER_H_
#define CHROME_BROWSER_UI_ASH_LAUNCHER_APP_SERVICE_APP_SERVICE_INSTANCE_REGISTRY_HELPER_H_

#include <memory>

#include "chrome/browser/ui/ash/launcher/launcher_controller_helper.h"
#include "chrome/services/app_service/public/cpp/instance.h"

namespace apps {
class AppServiceProxy;
}

namespace aura {
class Window;
}

namespace content {
class WebContents;
}

class Profile;

// The helper class to operate the App Service Instance Registry.
class AppServiceInstanceRegistryHelper {
 public:
  explicit AppServiceInstanceRegistryHelper(Profile* profile);
  ~AppServiceInstanceRegistryHelper();

  void ActiveUserChanged();

  // Notifies the AppService InstanceRegistry that active tabs are changed.
  void OnActiveTabChanged(content::WebContents* old_contents,
                          content::WebContents* new_contents);

  // Notifies the AppService InstanceRegistry that the tab's contents are
  // changed. The |old_contents|'s instance will be removed, and the
  // |new_contents|'s instance will be added.
  void OnTabReplaced(content::WebContents* old_contents,
                     content::WebContents* new_contents);

  // Notifies the AppService InstanceRegistry that a new tab is inserted. A new
  // instance will be add tp App Service InstanceRegistry.
  void OnTabInserted(content::WebContents* contents);

  // Notifies the AppService InstanceRegistry that the tab is closed. The
  // instance will be removed from App Service InstanceRegistry.
  void OnTabClosing(content::WebContents* contents);

  // Notifies the AppService InstanceRegistry that the browser is closed. The
  // instance will be removed from App Service InstanceRegistry.
  void OnBrowserRemoved();

  // Helper function to update App Service InstanceRegistry.
  void OnInstances(const std::string& app_id,
                   aura::Window* window,
                   const std::string& launch_id,
                   apps::InstanceState state);

  // Updates the apps state when the browser's visibility is changed.
  void OnWindowVisibilityChanged(const ash::ShelfID& shelf_id,
                                 aura::Window* window,
                                 bool visible);

  // Updates the apps state when the browser is inactivated.
  void SetWindowActivated(const ash::ShelfID& shelf_id,
                          aura::Window* window,
                          bool active);

  // Returns the instance state for |window| based on |visible|.
  apps::InstanceState CalculateVisibilityState(aura::Window* window,
                                               bool visible) const;

  // Returns the instance state for |window| based on |active|.
  apps::InstanceState CalculateActivatedState(aura::Window* window,
                                              bool active) const;

  // Return true if the app is opend in a browser.
  bool IsOpenedInBrowser(const std::string& app_id, aura::Window* window) const;

 private:
  // Returns an app id to represent |contents| in InstanceRegistry. If there is
  // no app in |contents|, returns the app id of the Chrome component
  // application.
  std::string GetAppId(content::WebContents* contents) const;

  // Returns a window to represent |contents| in InstanceRegistry. If |contents|
  // is a Web app, returns the native window for it. If there is no app in
  // |contents|, returns the toplevel window.
  aura::Window* GetWindow(content::WebContents* contents);

  // Adds the tab's |window| to |browser_window_to_tab_window_|.
  void AddTabWindow(const std::string& app_id, aura::Window* window);
  // Removes the tab's |window| from |browser_window_to_tab_window_|.
  void RemoveTabWindow(const std::string& app_id, aura::Window* window);

  apps::AppServiceProxy* proxy_ = nullptr;

  // Used to get app info for tabs.
  std::unique_ptr<LauncherControllerHelper> launcher_controller_helper_;

  // Maps the browser window to tab windows in the browser. When the browser
  // window is inactive or invisible, tab windows in the browser should be
  // updated accordingly as well.
  std::map<aura::Window*, std::set<aura::Window*>>
      browser_window_to_tab_window_;

  DISALLOW_COPY_AND_ASSIGN(AppServiceInstanceRegistryHelper);
};

#endif  // CHROME_BROWSER_UI_ASH_LAUNCHER_APP_SERVICE_APP_SERVICE_INSTANCE_REGISTRY_HELPER_H_
