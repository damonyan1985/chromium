// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROMEOS_CHILD_ACCOUNTS_TIME_LIMITS_APP_SERVICE_WRAPPER_H_
#define CHROME_BROWSER_CHROMEOS_CHILD_ACCOUNTS_TIME_LIMITS_APP_SERVICE_WRAPPER_H_

#include <vector>

#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/time/time.h"
#include "chrome/services/app_service/public/cpp/app_registry_cache.h"
#include "chrome/services/app_service/public/cpp/instance_registry.h"

class Profile;

namespace apps {
class AppUpdate;
class InstanceUpdate;
}  // namespace apps

namespace chromeos {
namespace app_time {

class AppId;

// Wrapper around AppService.
// Provides abstraction layer for Per-App Time Limits (PATL). Takes care of
// types conversions and data filetering, so those operations are not spread
// around the PATL code.
class AppServiceWrapper : public apps::AppRegistryCache::Observer,
                          public apps::InstanceRegistry::Observer {
 public:
  // Notifies listeners about app state changes.
  // Listener only get updates about apps that are relevant for PATL feature.
  class EventListener : public base::CheckedObserver {
   public:
    // Called when app with |app_id| is installed and at the beginning of each
    // user session (because AppService does not store apps information between
    // sessions).
    virtual void OnAppInstalled(const AppId& app_id) {}

    // Called when app with |app_id| is uninstalled.
    virtual void OnAppUninstalled(const AppId& app_id) {}

    // Called when app with |app_id| become available for usage. Usually when
    // app is unblocked.
    virtual void OnAppAvailable(const AppId& app_id) {}

    // Called when app with |app_id| become disabled and cannot be used.
    virtual void OnAppBlocked(const AppId& app_id) {}

    // Called when app with |app_id| becomes active.
    // Active means that the app is in usage (visible in foreground).
    // |timestamp| indicates the time when the app became active.
    virtual void OnAppActive(const AppId& app_id, base::Time timestamp) {}

    // Called when app with |app_id| becomes inactive.
    // Inactive means that the app is not in the foreground. It still can run
    // and be partially visible. |timestamp| indicates the time when the app
    // became inactive. Note: This can be called for the app that is already
    // inactive.
    virtual void OnAppInactive(const AppId& app_id, base::Time timestamp) {}
  };

  explicit AppServiceWrapper(Profile* profile);
  AppServiceWrapper(const AppServiceWrapper&) = delete;
  AppServiceWrapper& operator=(const AppServiceWrapper&) = delete;
  ~AppServiceWrapper() override;

  // Returns installed apps that are relevant for Per-App Time Limits feature.
  // Installed apps of unsupported types will not be included.
  std::vector<AppId> GetInstalledApps() const;

  // Returns short name of the app identified by |app_id|.
  // Might return empty string.
  std::string GetAppName(const AppId& app_id) const;

  void AddObserver(EventListener* observer);
  void RemoveObserver(EventListener* observer);

  // apps::AppRegistryCache::Observer:
  void OnAppUpdate(const apps::AppUpdate& update) override;
  void OnAppRegistryCacheWillBeDestroyed(
      apps::AppRegistryCache* cache) override;

  // apps::InstanceRegistry::Observer:
  void OnInstanceUpdate(const apps::InstanceUpdate& update) override;
  void OnInstanceRegistryWillBeDestroyed(
      apps::InstanceRegistry* cache) override;

 private:
  apps::AppRegistryCache& GetAppCache() const;
  apps::InstanceRegistry& GetInstanceRegistry() const;

  base::ObserverList<EventListener> listeners_;

  Profile* const profile_;
};

}  // namespace app_time
}  // namespace chromeos

#endif  // CHROME_BROWSER_CHROMEOS_CHILD_ACCOUNTS_TIME_LIMITS_APP_SERVICE_WRAPPER_H_
