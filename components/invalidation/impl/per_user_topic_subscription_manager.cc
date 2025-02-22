// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/invalidation/impl/per_user_topic_subscription_manager.h"

#include <stdint.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

#include "base/bind.h"
#include "base/feature_list.h"
#include "base/metrics/histogram_functions.h"
#include "base/rand_util.h"
#include "base/stl_util.h"
#include "base/strings/strcat.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"
#include "components/gcm_driver/instance_id/instance_id_driver.h"
#include "components/invalidation/impl/invalidation_switches.h"
#include "components/invalidation/public/identity_provider.h"
#include "components/invalidation/public/invalidation_util.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"

namespace syncer {

namespace {

const char kTypeSubscribedForInvalidationsDeprecated[] =
    "invalidation.registered_for_invalidation";

const char kTypeSubscribedForInvalidations[] =
    "invalidation.per_sender_registered_for_invalidation";

const char kActiveRegistrationTokenDeprecated[] =
    "invalidation.active_registration_token";

const char kActiveRegistrationTokens[] =
    "invalidation.per_sender_active_registration_tokens";

const char kInvalidationRegistrationScope[] =
    "https://firebaseperusertopics-pa.googleapis.com";

const char kFCMOAuthScope[] =
    "https://www.googleapis.com/auth/firebase.messaging";

// Note: Taking |topic| and |private_topic_name| by value (rather than const
// ref) because the caller (in practice, SubscriptionEntry) may be destroyed by
// the callback.
using SubscriptionFinishedCallback =
    base::OnceCallback<void(Topic topic,
                            Status code,
                            std::string private_topic_name,
                            PerUserTopicRegistrationRequest::RequestType type)>;

static const net::BackoffEntry::Policy kBackoffPolicy = {
    // Number of initial errors (in sequence) to ignore before applying
    // exponential back-off rules.
    0,

    // Initial delay for exponential back-off in ms.
    2000,

    // Factor by which the waiting time will be multiplied.
    2,

    // Fuzzing percentage. ex: 10% will spread requests randomly
    // between 90%-100% of the calculated time.
    0.2,  // 20%

    // Maximum amount of time we are willing to delay our request in ms.
    1000 * 3600 * 4,  // 4 hours.

    // Time to keep an entry from being discarded even when it
    // has no significant state, -1 to never discard.
    -1,

    // Don't use initial delay unless the last request was an error.
    false,
};

class PerProjectDictionaryPrefUpdate {
 public:
  explicit PerProjectDictionaryPrefUpdate(PrefService* prefs,
                                          const std::string& project_id)
      : update_(prefs, kTypeSubscribedForInvalidations) {
    per_sender_pref_ = update_->FindDictKey(project_id);
    if (!per_sender_pref_) {
      update_->SetDictionary(project_id,
                             std::make_unique<base::DictionaryValue>());
      per_sender_pref_ = update_->FindDictKey(project_id);
    }
    DCHECK(per_sender_pref_);
  }

  base::Value& operator*() { return *per_sender_pref_; }

  base::Value* operator->() { return per_sender_pref_; }

 private:
  DictionaryPrefUpdate update_;
  base::Value* per_sender_pref_;
};

// Added in M76.
void MigratePrefs(PrefService* prefs, const std::string& project_id) {
  if (!prefs->HasPrefPath(kActiveRegistrationTokenDeprecated)) {
    return;
  }
  {
    DictionaryPrefUpdate token_update(prefs, kActiveRegistrationTokens);
    token_update->SetString(
        project_id, prefs->GetString(kActiveRegistrationTokenDeprecated));
  }

  auto* old_subscriptions =
      prefs->GetDictionary(kTypeSubscribedForInvalidationsDeprecated);
  {
    PerProjectDictionaryPrefUpdate update(prefs, project_id);
    *update = old_subscriptions->Clone();
  }
  prefs->ClearPref(kActiveRegistrationTokenDeprecated);
  prefs->ClearPref(kTypeSubscribedForInvalidationsDeprecated);
}

}  // namespace

// static
void PerUserTopicSubscriptionManager::RegisterProfilePrefs(
    PrefRegistrySimple* registry) {
  registry->RegisterDictionaryPref(kTypeSubscribedForInvalidationsDeprecated);
  registry->RegisterStringPref(kActiveRegistrationTokenDeprecated,
                               std::string());

  registry->RegisterDictionaryPref(kTypeSubscribedForInvalidations);
  registry->RegisterDictionaryPref(kActiveRegistrationTokens);
}

// static
void PerUserTopicSubscriptionManager::RegisterPrefs(
    PrefRegistrySimple* registry) {
  // Same as RegisterProfilePrefs; see comment in the header.
  RegisterProfilePrefs(registry);
}

// State of the instance ID token when subscription is requested.
// Used by UMA histogram, so entries shouldn't be reordered or removed.
enum class PerUserTopicSubscriptionManager::TokenStateOnSubscriptionRequest {
  kTokenWasEmpty = 0,
  kTokenUnchanged = 1,
  kTokenChanged = 2,
  kTokenCleared = 3,
  kMaxValue = kTokenCleared,
};

struct PerUserTopicSubscriptionManager::SubscriptionEntry {
  SubscriptionEntry(const Topic& topic,
                    SubscriptionFinishedCallback completion_callback,
                    PerUserTopicRegistrationRequest::RequestType type,
                    bool topic_is_public = false);
  ~SubscriptionEntry();

  void SubscriptionFinished(const Status& code,
                            const std::string& private_topic_name);
  void Cancel();

  // The object for which this is the status.
  const Topic topic;
  const bool topic_is_public;
  SubscriptionFinishedCallback completion_callback;
  PerUserTopicRegistrationRequest::RequestType type;

  base::OneShotTimer request_retry_timer_;
  net::BackoffEntry request_backoff_;

  std::unique_ptr<PerUserTopicRegistrationRequest> request;

  DISALLOW_COPY_AND_ASSIGN(SubscriptionEntry);
};

PerUserTopicSubscriptionManager::SubscriptionEntry::SubscriptionEntry(
    const Topic& topic,
    SubscriptionFinishedCallback completion_callback,
    PerUserTopicRegistrationRequest::RequestType type,
    bool topic_is_public)
    : topic(topic),
      topic_is_public(topic_is_public),
      completion_callback(std::move(completion_callback)),
      type(type),
      request_backoff_(&kBackoffPolicy) {}

PerUserTopicSubscriptionManager::SubscriptionEntry::~SubscriptionEntry() {}

void PerUserTopicSubscriptionManager::SubscriptionEntry::SubscriptionFinished(
    const Status& code,
    const std::string& topic_name) {
  if (completion_callback)
    std::move(completion_callback).Run(topic, code, topic_name, type);
}

void PerUserTopicSubscriptionManager::SubscriptionEntry::Cancel() {
  request_retry_timer_.Stop();
  request.reset();
}

PerUserTopicSubscriptionManager::PerUserTopicSubscriptionManager(
    invalidation::IdentityProvider* identity_provider,
    PrefService* pref_service,
    network::mojom::URLLoaderFactory* url_loader_factory,
    const std::string& project_id,
    bool migrate_prefs)
    : pref_service_(pref_service),
      identity_provider_(identity_provider),
      url_loader_factory_(url_loader_factory),
      project_id_(project_id),
      migrate_prefs_(migrate_prefs),
      request_access_token_backoff_(&kBackoffPolicy) {}

PerUserTopicSubscriptionManager::~PerUserTopicSubscriptionManager() {}

// static
std::unique_ptr<PerUserTopicSubscriptionManager>
PerUserTopicSubscriptionManager::Create(
    invalidation::IdentityProvider* identity_provider,
    PrefService* pref_service,
    network::mojom::URLLoaderFactory* url_loader_factory,
    const std::string& project_id,
    bool migrate_prefs) {
  return std::make_unique<PerUserTopicSubscriptionManager>(
      identity_provider, pref_service, url_loader_factory, project_id,
      migrate_prefs);
}

void PerUserTopicSubscriptionManager::Init() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (migrate_prefs_) {
    MigratePrefs(pref_service_, project_id_);
  }
  PerProjectDictionaryPrefUpdate update(pref_service_, project_id_);
  if (update->DictEmpty()) {
    return;
  }

  std::vector<std::string> keys_to_remove;
  // Load subscribed topics from prefs.
  for (const auto& it : update->DictItems()) {
    Topic topic = it.first;
    std::string private_topic_name;
    if (it.second.GetAsString(&private_topic_name) &&
        !private_topic_name.empty()) {
      topic_to_private_topic_[topic] = private_topic_name;
      private_topic_to_topic_[private_topic_name] = topic;
    } else {
      // Couldn't decode the pref value; remove it.
      keys_to_remove.push_back(topic);
    }
  }

  // Delete prefs, which weren't decoded successfully.
  for (const std::string& key : keys_to_remove) {
    update->RemoveKey(key);
  }
}

void PerUserTopicSubscriptionManager::UpdateSubscribedTopics(
    const Topics& topics,
    const std::string& instance_id_token) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  instance_id_token_ = instance_id_token;
  DropAllSavedSubscriptionsOnTokenChange();

  for (const auto& topic : topics) {
    // If the topic isn't subscribed yet, schedule the subscription.
    if (topic_to_private_topic_.find(topic.first) ==
        topic_to_private_topic_.end()) {
      // If there's already a pending request for this topic, cancel it first.
      auto it = pending_subscriptions_.find(topic.first);
      if (it != pending_subscriptions_.end())
        it->second->Cancel();

      pending_subscriptions_[topic.first] = std::make_unique<SubscriptionEntry>(
          topic.first,
          base::BindOnce(
              &PerUserTopicSubscriptionManager::SubscriptionFinishedForTopic,
              base::Unretained(this)),
          PerUserTopicRegistrationRequest::SUBSCRIBE, topic.second.is_public);
    }
  }

  // There may be subscribed topics which need to be unsubscribed.
  // Schedule unsubscription and immediately remove from
  // |topic_to_private_topic_| and |private_topic_to_topic_|.
  for (auto it = topic_to_private_topic_.begin();
       it != topic_to_private_topic_.end();) {
    Topic topic = it->first;
    if (topics.find(topic) == topics.end()) {
      // TODO(crbug.com/1020117): If there's already a pending request for this
      // topic, we should probably cancel it first?
      pending_subscriptions_[topic] = std::make_unique<SubscriptionEntry>(
          topic,
          base::BindOnce(
              &PerUserTopicSubscriptionManager::SubscriptionFinishedForTopic,
              base::Unretained(this)),
          PerUserTopicRegistrationRequest::UNSUBSCRIBE);
      private_topic_to_topic_.erase(it->second);
      it = topic_to_private_topic_.erase(it);
      // The decision to unsubscribe from invalidations for |topic| was
      // made, the preferences should be cleaned up immediately.
      PerProjectDictionaryPrefUpdate update(pref_service_, project_id_);
      update->RemoveKey(topic);
    } else {
      // Topic is still wanted, nothing to do.
      ++it;
    }
  }

  // Kick off the process of actually processing the (un)subscriptions we just
  // scheduled.
  // TODO(crbug.com/1020117): Only do this if we actually scheduled anything,
  // i.e. |pending_subscriptions_| is not empty.
  RequestAccessToken();
}

void PerUserTopicSubscriptionManager::ClearInstanceIDToken() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  instance_id_token_.clear();
  DropAllSavedSubscriptionsOnTokenChange();
}

void PerUserTopicSubscriptionManager::StartPendingSubscriptions() {
  for (const auto& pending_subscription : pending_subscriptions_) {
    StartPendingSubscriptionRequest(pending_subscription.first);
  }
}

void PerUserTopicSubscriptionManager::StartPendingSubscriptionRequest(
    const Topic& topic) {
  auto it = pending_subscriptions_.find(topic);
  if (it == pending_subscriptions_.end()) {
    NOTREACHED() << "StartPendingSubscriptionRequest called on " << topic
                 << " which is not in the registration map";
    return;
  }
  PerUserTopicRegistrationRequest::Builder builder;
  // Resetting request in case it's running.
  // TODO(crbug.com/1020117): Should probably call it->second->Cancel() instead.
  it->second->request.reset();
  it->second->request = builder.SetInstanceIdToken(instance_id_token_)
                            .SetScope(kInvalidationRegistrationScope)
                            .SetPublicTopicName(topic)
                            .SetAuthenticationHeader(base::StringPrintf(
                                "Bearer %s", access_token_.c_str()))
                            .SetProjectId(project_id_)
                            .SetType(it->second->type)
                            .SetTopicIsPublic(it->second->topic_is_public)
                            .Build();
  it->second->request->Start(
      base::BindOnce(&PerUserTopicSubscriptionManager::SubscriptionEntry::
                         SubscriptionFinished,
                     base::Unretained(it->second.get())),
      url_loader_factory_);
}

void PerUserTopicSubscriptionManager::ActOnSuccessfulSubscription(
    const Topic& topic,
    const std::string& private_topic_name,
    PerUserTopicRegistrationRequest::RequestType type) {
  auto it = pending_subscriptions_.find(topic);
  it->second->request_backoff_.InformOfRequest(true);
  pending_subscriptions_.erase(it);
  if (type == PerUserTopicRegistrationRequest::SUBSCRIBE) {
    // If this was a subscription, update the prefs now (if it was an
    // unsubscription, we've already updated the prefs when scheduling the
    // request).
    {
      PerProjectDictionaryPrefUpdate update(pref_service_, project_id_);
      update->SetKey(topic, base::Value(private_topic_name));
      topic_to_private_topic_[topic] = private_topic_name;
      private_topic_to_topic_[private_topic_name] = topic;
    }
    pref_service_->CommitPendingWrite();
  }
  // Check if there are any other subscription (not unsubscription) requests
  // pending.
  bool all_subscriptions_completed = true;
  for (const auto& entry : pending_subscriptions_) {
    if (entry.second->type == PerUserTopicRegistrationRequest::SUBSCRIBE) {
      all_subscriptions_completed = false;
    }
  }
  // Emit ENABLED once we recovered from failed request.
  if (all_subscriptions_completed &&
      base::FeatureList::IsEnabled(
          invalidation::switches::kFCMInvalidationsConservativeEnabling)) {
    NotifySubscriptionChannelStateChange(SubscriptionChannelState::ENABLED);
  }
}

void PerUserTopicSubscriptionManager::ScheduleRequestForRepetition(
    const Topic& topic) {
  pending_subscriptions_[topic]->completion_callback = base::BindOnce(
      &PerUserTopicSubscriptionManager::SubscriptionFinishedForTopic,
      base::Unretained(this));
  // TODO(crbug.com/1020117): We already called InformOfRequest(false) before in
  // SubscriptionFinishedForTopic(), should probably not call it again here?
  pending_subscriptions_[topic]->request_backoff_.InformOfRequest(false);
  pending_subscriptions_[topic]->request_retry_timer_.Start(
      FROM_HERE,
      pending_subscriptions_[topic]->request_backoff_.GetTimeUntilRelease(),
      base::BindRepeating(
          &PerUserTopicSubscriptionManager::StartPendingSubscriptionRequest,
          base::Unretained(this), topic));
}

void PerUserTopicSubscriptionManager::SubscriptionFinishedForTopic(
    Topic topic,
    Status code,
    std::string private_topic_name,
    PerUserTopicRegistrationRequest::RequestType type) {
  if (code.IsSuccess()) {
    ActOnSuccessfulSubscription(topic, private_topic_name, type);
  } else {
    auto it = pending_subscriptions_.find(topic);
    it->second->request_backoff_.InformOfRequest(false);
    if (code.IsAuthFailure()) {
      // Re-request access token and try subscription requests again.
      RequestAccessToken();
    } else {
      // If one of the subscription requests failed, emit SUBSCRIPTION_FAILURE.
      if (type == PerUserTopicRegistrationRequest::SUBSCRIBE &&
          base::FeatureList::IsEnabled(
              invalidation::switches::kFCMInvalidationsConservativeEnabling)) {
        NotifySubscriptionChannelStateChange(
            SubscriptionChannelState::SUBSCRIPTION_FAILURE);
      }
      if (!code.ShouldRetry()) {
        pending_subscriptions_.erase(it);
        return;
      }
      ScheduleRequestForRepetition(topic);
    }
  }
}

TopicSet PerUserTopicSubscriptionManager::GetSubscribedTopicsForTest() const {
  TopicSet topics;
  for (const auto& t : topic_to_private_topic_)
    topics.insert(t.first);

  return topics;
}

void PerUserTopicSubscriptionManager::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void PerUserTopicSubscriptionManager::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void PerUserTopicSubscriptionManager::RequestAccessToken() {
  // TODO(crbug.com/1020117): Implement traffic optimisation.
  // * Before sending request to server ask for access token from identity
  //   provider (don't invalidate previous token).
  //   Identity provider will take care of retrieving/caching.
  // * Only invalidate access token when server didn't accept it.

  // Only one active request at a time.
  if (access_token_fetcher_ != nullptr)
    return;
  request_access_token_retry_timer_.Stop();
  OAuth2AccessTokenManager::ScopeSet oauth2_scopes = {kFCMOAuthScope};
  // Invalidate previous token, otherwise the identity provider will return the
  // same token again.
  identity_provider_->InvalidateAccessToken(oauth2_scopes, access_token_);
  access_token_.clear();
  access_token_fetcher_ = identity_provider_->FetchAccessToken(
      "fcm_invalidation", oauth2_scopes,
      base::BindOnce(
          &PerUserTopicSubscriptionManager::OnAccessTokenRequestCompleted,
          base::Unretained(this)));
}

void PerUserTopicSubscriptionManager::OnAccessTokenRequestCompleted(
    GoogleServiceAuthError error,
    std::string access_token) {
  access_token_fetcher_.reset();
  if (error.state() == GoogleServiceAuthError::NONE)
    OnAccessTokenRequestSucceeded(access_token);
  else
    OnAccessTokenRequestFailed(error);
}

void PerUserTopicSubscriptionManager::OnAccessTokenRequestSucceeded(
    const std::string& access_token) {
  // Reset backoff time after successful response.
  request_access_token_backoff_.Reset();
  access_token_ = access_token;
  // Emit ENABLED when successfully got the token.
  // TODO(crbug.com/1020117): This seems wrong; we generally emit ENABLED only
  // when all subscriptions have successfully completed.
  NotifySubscriptionChannelStateChange(SubscriptionChannelState::ENABLED);
  StartPendingSubscriptions();
}

void PerUserTopicSubscriptionManager::OnAccessTokenRequestFailed(
    GoogleServiceAuthError error) {
  DCHECK_NE(error.state(), GoogleServiceAuthError::NONE);
  NotifySubscriptionChannelStateChange(
      SubscriptionChannelState::ACCESS_TOKEN_FAILURE);
  request_access_token_backoff_.InformOfRequest(false);
  request_access_token_retry_timer_.Start(
      FROM_HERE, request_access_token_backoff_.GetTimeUntilRelease(),
      base::BindRepeating(&PerUserTopicSubscriptionManager::RequestAccessToken,
                          base::Unretained(this)));
}

void PerUserTopicSubscriptionManager::DropAllSavedSubscriptionsOnTokenChange() {
  TokenStateOnSubscriptionRequest outcome =
      DropAllSavedSubscriptionsOnTokenChangeImpl();
  base::UmaHistogramEnumeration(
      "FCMInvalidations.TokenStateOnRegistrationRequest2", outcome);
}

PerUserTopicSubscriptionManager::TokenStateOnSubscriptionRequest
PerUserTopicSubscriptionManager::DropAllSavedSubscriptionsOnTokenChangeImpl() {
  {
    DictionaryPrefUpdate token_update(pref_service_, kActiveRegistrationTokens);
    std::string previous_token;
    token_update->GetString(project_id_, &previous_token);
    if (previous_token == instance_id_token_) {
      // Note: This includes the case where the token was and still is empty.
      return TokenStateOnSubscriptionRequest::kTokenUnchanged;
    }

    token_update->SetString(project_id_, instance_id_token_);
    if (previous_token.empty()) {
      // If we didn't have a registration token before, we shouldn't have had
      // any subscriptions either, so no need to drop them.
      return TokenStateOnSubscriptionRequest::kTokenWasEmpty;
    }
  }

  // The token has been cleared or changed. In either case, clear all existing
  // subscriptions since they won't be valid anymore. (No need to send
  // unsubscribe requests - if the token was revoked, the server will drop the
  // subscriptions anyway.)
  PerProjectDictionaryPrefUpdate update(pref_service_, project_id_);
  *update = base::Value(base::Value::Type::DICTIONARY);
  topic_to_private_topic_.clear();
  private_topic_to_topic_.clear();
  // Also cancel any pending subscription requests.
  for (const auto& pending_subscription : pending_subscriptions_) {
    pending_subscription.second->Cancel();
  }
  pending_subscriptions_.clear();
  return instance_id_token_.empty()
             ? TokenStateOnSubscriptionRequest::kTokenCleared
             : TokenStateOnSubscriptionRequest::kTokenChanged;
}

void PerUserTopicSubscriptionManager::NotifySubscriptionChannelStateChange(
    SubscriptionChannelState state) {
  // NOT_STARTED is the default state of the subscription
  // channel and shouldn't explicitly issued.
  DCHECK(state != SubscriptionChannelState::NOT_STARTED);
  if (last_issued_state_ == state) {
    // Notify only on state change.
    return;
  }

  last_issued_state_ = state;
  for (auto& observer : observers_) {
    observer.OnSubscriptionChannelStateChanged(state);
  }
}

base::DictionaryValue PerUserTopicSubscriptionManager::CollectDebugData()
    const {
  base::DictionaryValue status;
  for (const auto& topic_to_private_topic : topic_to_private_topic_) {
    status.SetString(topic_to_private_topic.first,
                     topic_to_private_topic.second);
  }
  status.SetString("Instance id token", instance_id_token_);
  return status;
}

base::Optional<Topic>
PerUserTopicSubscriptionManager::LookupSubscribedPublicTopicByPrivateTopic(
    const std::string& private_topic) const {
  auto it = private_topic_to_topic_.find(private_topic);
  if (it == private_topic_to_topic_.end()) {
    return base::nullopt;
  }
  return it->second;
}

}  // namespace syncer
