// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_METRICS_STRUCTURED_STRUCTURED_METRICS_PROVIDER_H_
#define COMPONENTS_METRICS_STRUCTURED_STRUCTURED_METRICS_PROVIDER_H_

#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "components/metrics/metrics_provider.h"
#include "components/metrics/structured/recorder.h"
#include "components/prefs/persistent_pref_store.h"
#include "components/prefs/pref_store.h"

class JsonPrefStore;

namespace metrics {
namespace structured {

// StructuredMetricsProvider is responsible for filling out the
// |structured_metrics_event| section of the UMA proto. This class should not be
// instantiated except by the ChromeMetricsServiceClient. This class is not
// thread safe and should only be called on the browser UI sequence, because
// calls from the metrics service come on the UI sequence.
//
// Currently, the structured metrics system is cros-only and relies on the cros
// cryptohome to store keys and unsent logs, collectively called 'state'. This
// means structured metrics collection cannot begin until a profile eligible
// for metrics collection is added.
//
// TODO(crbug.com/1016655): generalize structured metrics beyond cros.
//
// Initialization of the StructuredMetricsProvider must wait until a profile is
// added, because state is stored within the profile directory. Initialization
// happens in several steps:
//
// 1. A StructuredMetricsProvider instance is constructed and owned by the
//    MetricsService.
//
// 2. When recording is enabled, the provider registers itself as an observer of
//    Recorder.
//
// 3. When a profile is added that is eligible for recording,
//    ChromeMetricsServiceClient calls Recorder::ProfileAdded, which notifies
//    this class.
//
// 4. This class then begins initialization by asynchronously reading keys and
//    unsent logs from a JsonPrefStore within the profile path.
//
// 5. If the read succeeds, initialization is complete and this class starts
//    accepting events to record.
//
// After initialization, this class accepts events to record from
// StructuredMetricsProvider::OnRecord via Recorder::Record via
// EventBase::Record. These events are not uploaded immediately, and are cached
// in ready-to-upload form.
//
// On a call to ProvideCurrentSessionData, the cache of unsent logs is added to
// a ChromeUserMetricsExtension for upload, and is then cleared.
class StructuredMetricsProvider : public metrics::MetricsProvider,
                                  public Recorder::Observer,
                                  public PrefStore::Observer {
 public:
  StructuredMetricsProvider();
  ~StructuredMetricsProvider() override;
  StructuredMetricsProvider(const StructuredMetricsProvider&) = delete;
  StructuredMetricsProvider& operator=(const StructuredMetricsProvider&) =
      delete;

 private:
  friend class Recorder;
  friend class StructuredMetricsProviderTest;

  // An error delegate called when |storage_| has finished reading prefs from
  // disk.
  class PrefStoreErrorDelegate : public PersistentPrefStore::ReadErrorDelegate {
   public:
    PrefStoreErrorDelegate();
    ~PrefStoreErrorDelegate() override;

    // PersistentPrefStore::ReadErrorDelegate:
    void OnError(PersistentPrefStore::PrefReadError error) override;
  };

  // metrics::MetricsProvider:
  void OnRecordingEnabled() override;
  void OnRecordingDisabled() override;
  void ProvideCurrentSessionData(
      metrics::ChromeUserMetricsExtension* uma_proto) override;

  // Recorder::Observer:
  void OnRecord(const EventBase& event) override;
  void OnProfileAdded(const base::FilePath& profile_path) override;

  // PrefStore::Observer:
  void OnInitializationCompleted(bool success) override;
  void OnPrefValueChanged(const std::string& key) override {}

  // Beyond this number of logging events between successive calls to
  // ProvideCurrentSessionData, we stop recording events.
  static int kMaxEventsPerUpload;
  // The basename of the file to store key data and unsent logs. A JsonPrefStore
  // is initialized at {profile_path}/{kStorageFileName}.
  static char kStorageFileName[];

  // Whether the metrics provider has completed initialization. Initialization
  // occurs across OnProfileAdded and OnInitializationCompleted. No incoming
  // events are recorded until initialization has succeeded.
  //
  // Execution is:
  //  - A profile is added.
  //  - OnProfileAdded is called, which constructs |storage_| and
  //    asynchronously reads prefs.
  //  - OnInitializationCompleted is called once pref reading is complete, which
  //    sets |initialized_| to true.
  //
  // The metrics provider does not handle multiprofile: initialization happens
  // only once, for the first-logged-in account aka. primary user.
  bool initialized_ = false;

  // Tracks the recording state signalled to the metrics provider by
  // OnRecordingEnabled and OnRecordingDisabled.
  bool recording_enabled_ = false;

  // On-device storage within the user's cryptohome for keys and unsent logs.
  // This is constructed as part of initialization and is guaranteed to be
  // initialized if |initialized_| is true.
  scoped_refptr<JsonPrefStore> storage_;

  base::WeakPtrFactory<StructuredMetricsProvider> weak_factory_{this};
};

}  // namespace structured
}  // namespace metrics

#endif  // COMPONENTS_METRICS_STRUCTURED_STRUCTURED_METRICS_PROVIDER_H_
