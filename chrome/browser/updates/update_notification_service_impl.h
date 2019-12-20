// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UPDATES_UPDATE_NOTIFICATION_SERVICE_IMPL_H_
#define CHROME_BROWSER_UPDATES_UPDATE_NOTIFICATION_SERVICE_IMPL_H_

#include "chrome/browser/updates/update_notification_service.h"

#include <memory>

#include "base/memory/weak_ptr.h"

namespace notifications {
struct ClientOverview;
class NotificationScheduleService;
struct ScheduleParams;
}  // namespace notifications

namespace updates {

struct UpdateNotificationConfig;
struct UpdateNotificationInfo;

class UpdateNotificationServiceImpl : public UpdateNotificationService {
 public:
  UpdateNotificationServiceImpl(
      notifications::NotificationScheduleService* schedule_service);
  ~UpdateNotificationServiceImpl() override;

 private:
  // UpdateNotificationService implementation.
  void Schedule(UpdateNotificationInfo data) override;

  // Called after querying the |ClientOverview| struct from scheduler system
  // completed.
  void OnClientOverviewQueried(UpdateNotificationInfo data,
                               notifications::ClientOverview overview);

  // Build notification ScheduleParams for update notification.
  notifications::ScheduleParams BuildScheduleParams();

  // Used to schedule notification to show in the future. Must outlive this
  // class.
  notifications::NotificationScheduleService* schedule_service_;

  std::unique_ptr<UpdateNotificationConfig> config_;

  base::WeakPtrFactory<UpdateNotificationServiceImpl> weak_ptr_factory_{this};

  DISALLOW_COPY_AND_ASSIGN(UpdateNotificationServiceImpl);
};

}  // namespace updates

#endif  // CHROME_BROWSER_UPDATES_UPDATE_NOTIFICATION_SERVICE_IMPL_H_
