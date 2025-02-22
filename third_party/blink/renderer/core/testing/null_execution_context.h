// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_TESTING_NULL_EXECUTION_CONTEXT_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_TESTING_NULL_EXECUTION_CONTEXT_H_

#include <memory>
#include "base/single_thread_task_runner.h"
#include "third_party/blink/renderer/bindings/core/v8/source_location.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/execution_context/security_context.h"
#include "third_party/blink/renderer/core/inspector/console_message.h"
#include "third_party/blink/renderer/platform/heap/handle.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"

namespace blink {

class NullExecutionContext : public GarbageCollected<NullExecutionContext>,
                             public ExecutionContext {
  USING_GARBAGE_COLLECTED_MIXIN(NullExecutionContext);

 public:
  NullExecutionContext(OriginTrialContext* origin_trial_context = nullptr);
  ~NullExecutionContext() override;

  void SetURL(const KURL& url) { url_ = url; }

  const KURL& Url() const override { return url_; }
  const KURL& BaseURL() const override { return url_; }
  KURL CompleteURL(const String&) const override { return url_; }

  void DisableEval(const String&) override {}
  String UserAgent() const override { return String(); }

  HttpsState GetHttpsState() const override {
    return CalculateHttpsState(GetSecurityOrigin());
  }

  EventTarget* ErrorEventTarget() override { return nullptr; }

  void AddConsoleMessageImpl(ConsoleMessage*,
                             bool discard_duplicates) override {}
  void ExceptionThrown(ErrorEvent*) override {}

  void SetIsSecureContext(bool);
  bool IsSecureContext(String& error_message) const override;

  void SetUpSecurityContextForTesting();

  ResourceFetcher* Fetcher() const override { return nullptr; }

  FrameOrWorkerScheduler* GetScheduler() override;
  scoped_refptr<base::SingleThreadTaskRunner> GetTaskRunner(TaskType) override;

  void CountUse(mojom::WebFeature) override {}
  void CountDeprecation(mojom::WebFeature) override {}

  BrowserInterfaceBrokerProxy& GetBrowserInterfaceBroker() override;

 private:
  bool is_secure_context_;

  KURL url_;

  // A dummy scheduler to ensure that the callers of
  // ExecutionContext::GetScheduler don't have to check for whether it's null or
  // not.
  std::unique_ptr<FrameOrWorkerScheduler> scheduler_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_TESTING_NULL_EXECUTION_CONTEXT_H_
