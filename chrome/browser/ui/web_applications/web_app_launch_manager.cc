// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/web_applications/web_app_launch_manager.h"

#include "base/metrics/histogram_macros.h"
#include "chrome/browser/app_mode/app_mode_utils.h"
#include "chrome/browser/apps/app_service/app_launch_params.h"
#include "chrome/browser/banners/app_banner_settings_helper.h"
#include "chrome/browser/engagement/site_engagement_service.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/web_applications/system_web_app_ui_utils.h"
#include "chrome/browser/web_applications/components/file_handler_manager.h"
#include "chrome/browser/web_applications/components/web_app_constants.h"
#include "chrome/browser/web_applications/components/web_app_helpers.h"
#include "chrome/browser/web_applications/components/web_app_install_utils.h"
#include "chrome/browser/web_applications/components/web_app_tab_helper.h"
#include "chrome/browser/web_applications/system_web_app_manager.h"
#include "chrome/browser/web_applications/web_app.h"
#include "chrome/browser/web_applications/web_app_provider.h"
#include "chrome/browser/web_applications/web_app_registrar.h"
#include "chrome/browser/web_launch/web_launch_files_helper.h"
#include "content/public/browser/render_view_host.h"
#include "extensions/common/constants.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/public/mojom/renderer_preferences.mojom.h"

namespace web_app {

namespace {

ui::WindowShowState DetermineWindowShowState() {
  if (chrome::IsRunningInForcedAppMode())
    return ui::SHOW_STATE_FULLSCREEN;

  return ui::SHOW_STATE_DEFAULT;
}

}  // namespace

Browser* CreateWebApplicationWindow(Profile* profile,
                                    const std::string& app_id) {
  std::string app_name = GenerateApplicationNameFromAppId(app_id);
  gfx::Rect initial_bounds;
  auto browser_params = Browser::CreateParams::CreateForApp(
      app_name, /*trusted_source=*/true, initial_bounds, profile,
      /*user_gesture=*/true);
  browser_params.initial_show_state = DetermineWindowShowState();
  return new Browser(browser_params);
}

content::WebContents* NavigateWebApplicationWindow(
    Browser* browser,
    const std::string& app_id,
    const GURL& url,
    WindowOpenDisposition disposition) {
  NavigateParams nav_params(browser, url, ui::PAGE_TRANSITION_AUTO_BOOKMARK);
  nav_params.disposition = disposition;
  Navigate(&nav_params);

  content::WebContents* web_contents =
      nav_params.navigated_or_inserted_contents;

  // TODO(https://crbug.com/1032443):
  // Eventually move this to browser_navigator.cc: CreateTargetContents().
  WebAppTabHelper* tab_helper = WebAppTabHelper::FromWebContents(web_contents);
  DCHECK(tab_helper);
  tab_helper->SetAppId(app_id);

  return web_contents;
}

WebAppLaunchManager::WebAppLaunchManager(Profile* profile)
    : apps::LaunchManager(profile), provider_(WebAppProvider::Get(profile)) {}

WebAppLaunchManager::~WebAppLaunchManager() = default;

content::WebContents* WebAppLaunchManager::OpenApplication(
    const apps::AppLaunchParams& params) {
  if (!provider_->registrar().IsInstalled(params.app_id))
    return nullptr;

  if (params.container == apps::mojom::LaunchContainer::kLaunchContainerWindow)
    RecordAppWindowLaunch(profile(), params.app_id);

  web_app::FileHandlerManager& file_handler_manager =
      provider_->file_handler_manager();

  const GURL url =
      params.override_url.is_empty()
          ? file_handler_manager
                .GetMatchingFileHandlerURL(params.app_id, params.launch_files)
                .value_or(provider_->registrar().GetAppLaunchURL(params.app_id))
          : params.override_url;

  // System Web Apps go through their own launch path.
  base::Optional<SystemAppType> system_app_type =
      GetSystemWebAppTypeForAppId(profile(), params.app_id);
  if (system_app_type) {
    Browser* browser =
        LaunchSystemWebApp(profile(), *system_app_type, url, params);
    return browser->tab_strip_model()->GetActiveWebContents();
  }

  Browser* browser = CreateWebApplicationWindow(profile(), params.app_id);

  content::WebContents* web_contents = NavigateWebApplicationWindow(
      browser, params.app_id, url, WindowOpenDisposition::NEW_FOREGROUND_TAB);

  if (base::FeatureList::IsEnabled(blink::features::kFileHandlingAPI)) {
    web_launch::WebLaunchFilesHelper::SetLaunchPaths(web_contents, url,
                                                     params.launch_files);
  }

  browser->window()->Show();

  // TODO(crbug.com/1014328): Populate WebApp metrics instead of Extensions.

  UMA_HISTOGRAM_ENUMERATION("Extensions.HostedAppLaunchContainer",
                            params.container);
  if (params.container == apps::mojom::LaunchContainer::kLaunchContainerTab) {
    UMA_HISTOGRAM_ENUMERATION("Extensions.AppTabLaunchType",
                              extensions::LAUNCH_TYPE_REGULAR, 100);
  }
  UMA_HISTOGRAM_ENUMERATION("Extensions.BookmarkAppLaunchSource",
                            params.source);
  UMA_HISTOGRAM_ENUMERATION("Extensions.BookmarkAppLaunchContainer",
                            params.container);

  // Record the launch time in the site engagement service. A recent web
  // app launch will provide an engagement boost to the origin.
  SiteEngagementService::Get(profile())->SetLastShortcutLaunchTime(web_contents,
                                                                   url);

  // Refresh the app banner added to homescreen event. The user may have
  // cleared their browsing data since installing the app, which removes the
  // event and will potentially permit a banner to be shown for the site.
  RecordAppBanner(web_contents, url);

  return web_contents;
}

bool WebAppLaunchManager::OpenApplicationWindow(
    const std::string& app_id,
    const base::CommandLine& command_line,
    const base::FilePath& current_directory) {
  if (!provider_)
    return false;

  apps::AppLaunchParams params(
      app_id, apps::mojom::LaunchContainer::kLaunchContainerWindow,
      WindowOpenDisposition::NEW_WINDOW,
      apps::mojom::AppLaunchSource::kSourceCommandLine);
  params.command_line = command_line;
  params.current_directory = current_directory;
  params.launch_files =
      apps::LaunchManager::GetLaunchFilesFromCommandLine(command_line);

  provider_->on_registry_ready().Post(
      FROM_HERE, base::BindOnce(&WebAppLaunchManager::OpenWebApplication,
                                weak_ptr_factory_.GetWeakPtr(), params));

  return true;
}

bool WebAppLaunchManager::OpenApplicationTab(const std::string& app_id) {
  if (!provider_)
    return false;

  apps::AppLaunchParams params(
      app_id, apps::mojom::LaunchContainer::kLaunchContainerTab,
      WindowOpenDisposition::NEW_FOREGROUND_TAB,
      apps::mojom::AppLaunchSource::kSourceCommandLine);

  // Wait for the web applications database to load.
  // If the profile and WebAppLaunchManager are destroyed,
  // on_registry_ready will not fire.
  provider_->on_registry_ready().Post(
      FROM_HERE, base::BindOnce(&WebAppLaunchManager::OpenWebApplication,
                                weak_ptr_factory_.GetWeakPtr(), params));
  return true;
}

void WebAppLaunchManager::OpenWebApplication(
    const apps::AppLaunchParams& params) {
  OpenApplication(params);
}

void RecordAppWindowLaunch(Profile* profile, const std::string& app_id) {
  WebAppProvider* provider = WebAppProvider::Get(profile);
  if (!provider)
    return;

  DisplayMode display = provider->registrar().GetAppDisplayMode(app_id);
  if (display == DisplayMode::kUndefined)
    return;

  DCHECK_LT(DisplayMode::kUndefined, display);
  DCHECK_LE(display, DisplayMode::kMaxValue);
  UMA_HISTOGRAM_ENUMERATION("Launch.WebAppDisplayMode", display);
}

}  // namespace web_app
