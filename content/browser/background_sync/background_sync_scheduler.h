// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_BACKGROUND_SYNC_BACKGROUND_SYNC_SCHEDULER_H_
#define CONTENT_BROWSER_BACKGROUND_SYNC_BACKGROUND_SYNC_SCHEDULER_H_

#include <map>

#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "content/common/content_export.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "third_party/blink/public/mojom/background_sync/background_sync.mojom.h"

namespace content {

class StoragePartitionImpl;

// Key name on BrowserContext.
extern const char kBackgroundSyncSchedulerKey[];

// This contains the logic to schedule delayed processing of (periodic)
// Background Sync registrations.
// It keeps track of all storage partitions, and the soonest time we should
// attempt to fire (periodic)sync events for it.
class CONTENT_EXPORT BackgroundSyncScheduler
    : public base::RefCountedThreadSafe<BackgroundSyncScheduler,
                                        BrowserThread::DeleteOnUIThread> {
 public:
  static BackgroundSyncScheduler* GetFor(BrowserContext* browser_context);

  BackgroundSyncScheduler();

  // Schedules delayed_processing for |sync_type| for |storage_partition|.
  // On non-Android platforms, runs |delayed_task| after |delay| has passed.
  // TODO(crbug.com/996166): Add logic to schedule browser wakeup on Android.
  // Must be called on the UI thread.
  virtual void ScheduleDelayedProcessing(
      StoragePartitionImpl* storage_partition,
      blink::mojom::BackgroundSyncType sync_type,
      base::TimeDelta delay,
      base::OnceClosure delayed_task);

  // Cancels delayed_processing for |sync_type| for |storage_partition|.
  // Must be called on the UI thread.
  virtual void CancelDelayedProcessing(
      StoragePartitionImpl* storage_partition,
      blink::mojom::BackgroundSyncType sync_type);

 private:
  virtual ~BackgroundSyncScheduler();

  friend struct BrowserThread::DeleteOnThread<BrowserThread::UI>;
  friend class base::DeleteHelper<BackgroundSyncScheduler>;

  std::map<StoragePartitionImpl*, std::unique_ptr<base::OneShotTimer>>&
  GetDelayedProcessingInfoMap(blink::mojom::BackgroundSyncType sync_type);
  void RunDelayedTaskAndPruneInfoMap(blink::mojom::BackgroundSyncType sync_type,
                                     StoragePartitionImpl* storage_partition,
                                     base::OnceClosure delayed_task);
#if defined(OS_ANDROID)
  void ScheduleOrCancelBrowserWakeupForSyncType(
      blink::mojom::BackgroundSyncType sync_type,
      StoragePartitionImpl* storage_partition);
#endif

  std::map<StoragePartitionImpl*, std::unique_ptr<base::OneShotTimer>>
      delayed_processing_info_one_shot_;
  std::map<StoragePartitionImpl*, std::unique_ptr<base::OneShotTimer>>
      delayed_processing_info_periodic_;

  base::WeakPtrFactory<BackgroundSyncScheduler> weak_ptr_factory_{this};

  DISALLOW_COPY_AND_ASSIGN(BackgroundSyncScheduler);
};

}  // namespace content

#endif  // CONTENT_BROWSER_BACKGROUND_SYNC_BACKGROUND_SYNC_SCHEDULER_H_
