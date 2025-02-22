// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_SYNC_DRIVER_TRUSTED_VAULT_CLIENT_H_
#define COMPONENTS_SYNC_DRIVER_TRUSTED_VAULT_CLIENT_H_

#include <memory>
#include <string>
#include <vector>

#include "base/callback_forward.h"
#include "base/callback_list.h"
#include "base/macros.h"

namespace syncer {

// Interface that allows platform-specific logic related to accessing locally
// available trusted vault encryption keys.
class TrustedVaultClient {
 public:
  TrustedVaultClient() = default;
  virtual ~TrustedVaultClient() = default;

  using CallbackList = base::CallbackList<void()>;
  using Subscription = CallbackList::Subscription;

  // Registers an observer-like callback that will be invoked when the content
  // of the vault has changed (e.g. new keys added). The subscription must not
  // outlive |*this|.
  virtual std::unique_ptr<Subscription> AddKeysChangedObserver(
      const base::RepeatingClosure& cb) = 0;

  // Attempts to fetch decryption keys, required by sync to resume.
  // Implementations are expected to NOT prompt the user for actions. |cb| is
  // called on completion with known keys or an empty list if none known.
  // Concurrent calls to FetchKeys() must not be issued since implementations
  // may not support them.
  virtual void FetchKeys(
      const std::string& gaia_id,
      base::OnceCallback<void(const std::vector<std::string>&)> cb) = 0;

  // Allows implementations to store encryption keys fetched by other means such
  // as Web interactions. Implementations are free to completely ignore these
  // keys, so callers may not assume that later calls to FetchKeys() would
  // necessarily return the keys passed here.
  virtual void StoreKeys(const std::string& gaia_id,
                         const std::vector<std::string>& keys) = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(TrustedVaultClient);
};

}  // namespace syncer

#endif  // COMPONENTS_SYNC_DRIVER_TRUSTED_VAULT_CLIENT_H_
