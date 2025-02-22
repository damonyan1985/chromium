// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_SMS_SMS_FETCHER_IMPL_H_
#define CONTENT_BROWSER_SMS_SMS_FETCHER_IMPL_H_

#include <memory>

#include "base/optional.h"
#include "base/sequence_checker.h"
#include "base/supports_user_data.h"
#include "content/browser/sms/sms_provider.h"
#include "content/browser/sms/sms_queue.h"
#include "content/common/content_export.h"
#include "content/public/browser/sms_fetcher.h"

namespace url {
class Origin;
}

namespace content {

class BrowserContext;
class SmsProvider;

// SmsFetcherImpls coordinate between local and remote SMS providers as well as
// between multiple origins. There is one SmsFetcherImpl per profile. An
// instance must only be used on the sequence it was created on.
class CONTENT_EXPORT SmsFetcherImpl : public content::SmsFetcher,
                                      public base::SupportsUserData::Data,
                                      public SmsProvider::Observer {
 public:
  SmsFetcherImpl(BrowserContext* context,
                 std::unique_ptr<SmsProvider> provider);
  ~SmsFetcherImpl() override;

  static SmsFetcher* Get(BrowserContext* context);

  void Subscribe(const url::Origin& origin, Subscriber* subscriber) override;
  void Unsubscribe(const url::Origin& origin, Subscriber* subscriber) override;

  // content::SmsProvider::Observer:
  bool OnReceive(const url::Origin& origin,
                 const std::string& one_time_code,
                 const std::string& sms) override;

  bool HasSubscribers() override;

  void SetSmsProviderForTesting(std::unique_ptr<SmsProvider> provider);

 private:
  void OnRemote(base::Optional<std::string> sms);

  bool Notify(const url::Origin& origin,
              const std::string& one_time_code,
              const std::string& sms);

  // |context_| is safe because all instances of SmsFetcherImpl are owned by
  // the BrowserContext itself.
  BrowserContext* context_;

  std::unique_ptr<SmsProvider> provider_;

  SmsQueue subscribers_;

  SEQUENCE_CHECKER(sequence_checker_);

  base::WeakPtrFactory<SmsFetcherImpl> weak_ptr_factory_{this};

  DISALLOW_COPY_AND_ASSIGN(SmsFetcherImpl);
};

}  // namespace content

#endif  // CONTENT_BROWSER_SMS_SMS_FETCHER_IMPL_H_
