// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/safe_browsing/verdict_cache_manager.h"

#include "base/base64.h"
#include "base/optional.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/task/post_task.h"
#include "components/history/core/browser/history_service_observer.h"
#include "components/safe_browsing/db/v4_protocol_manager_util.h"
#include "components/safe_browsing/proto/csd.pb.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"

namespace safe_browsing {

namespace {

// Keys for storing password protection verdict into a DictionaryValue.
const char kCacheCreationTime[] = "cache_creation_time";
const char kVerdictProto[] = "verdict_proto";
const char kRealTimeThreatInfoProto[] = "rt_threat_info_proto";
const char kPasswordOnFocusCacheKey[] = "password_on_focus_cache_key";
const char kRealTimeUrlCacheKey[] = "real_time_url_cache_key";

// Given a URL of either http or https scheme, return its http://hostname.
// e.g., "https://www.foo.com:80/bar/test.cgi" -> "http://www.foo.com".
GURL GetHostNameWithHTTPScheme(const GURL& url) {
  DCHECK(url.SchemeIsHTTPOrHTTPS());
  std::string result(url::kHttpScheme);
  result.append(url::kStandardSchemeSeparator).append(url.host());
  return GURL(result);
}

// Convert a Proto object into a DictionaryValue.
template <class T>
std::unique_ptr<base::DictionaryValue> CreateDictionaryFromVerdict(
    const T& verdict,
    const base::Time& receive_time,
    const char* proto_name) {
  DCHECK(proto_name == kVerdictProto || proto_name == kRealTimeThreatInfoProto);
  std::unique_ptr<base::DictionaryValue> result =
      std::make_unique<base::DictionaryValue>();
  result->SetInteger(kCacheCreationTime,
                     static_cast<int>(receive_time.ToDoubleT()));
  std::string serialized_proto(verdict.SerializeAsString());
  // Performs a base64 encoding on the serialized proto.
  base::Base64Encode(serialized_proto, &serialized_proto);
  result->SetString(proto_name, serialized_proto);
  return result;
}

// Generate path variants of the given URL.
void GeneratePathVariantsWithoutQuery(const GURL& url,
                                      std::vector<std::string>* paths) {
  std::string canonical_path;
  V4ProtocolManagerUtil::CanonicalizeUrl(url, nullptr, &canonical_path,
                                         nullptr);
  V4ProtocolManagerUtil::GeneratePathVariantsToCheck(canonical_path,
                                                     std::string(), paths);
}

template <class T>
bool ParseVerdictEntry(base::Value* verdict_entry,
                       int* out_verdict_received_time,
                       T* out_verdict,
                       const char* proto_name) {
  DCHECK(proto_name == kVerdictProto || proto_name == kRealTimeThreatInfoProto);

  if (!verdict_entry || !verdict_entry->is_dict() || !out_verdict)
    return false;
  base::Value* cache_creation_time_value =
      verdict_entry->FindKey(kCacheCreationTime);

  if (!cache_creation_time_value || !cache_creation_time_value->is_int())
    return false;
  *out_verdict_received_time = cache_creation_time_value->GetInt();

  base::Value* verdict_proto_value = verdict_entry->FindKey(proto_name);
  if (!verdict_proto_value || !verdict_proto_value->is_string())
    return false;
  std::string serialized_proto = verdict_proto_value->GetString();

  return base::Base64Decode(serialized_proto, &serialized_proto) &&
         out_verdict->ParseFromString(serialized_proto);
}

// Return the path of the cache expression. e.g.:
// "www.google.com"     -> ""
// "www.google.com/abc" -> "/abc"
// "foo.com/foo/bar/"  -> "/foo/bar/"
std::string GetCacheExpressionPath(const std::string& cache_expression) {
  DCHECK(!cache_expression.empty());
  size_t first_slash_pos = cache_expression.find_first_of("/");
  if (first_slash_pos == std::string::npos)
    return "";
  return cache_expression.substr(first_slash_pos);
}

// Returns the number of path segments in |cache_expression_path|.
// For example, return 0 for "/", since there is no path after the leading
// slash; return 3 for "/abc/def/gh.html".
size_t GetPathDepth(const std::string& cache_expression_path) {
  return base::SplitString(base::StringPiece(cache_expression_path), "/",
                           base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)
      .size();
}

bool PathVariantsMatchCacheExpression(
    const std::vector<std::string>& generated_paths,
    const std::string& cache_expression_path) {
  return base::Contains(generated_paths, cache_expression_path);
}

bool IsCacheExpired(int cache_creation_time, int cache_duration) {
  // Note that we assume client's clock is accurate or almost accurate.
  return base::Time::Now().ToDoubleT() >
         static_cast<double>(cache_creation_time + cache_duration);
}

template <class T>
size_t RemoveExpiredEntries(base::Value* verdict_dictionary,
                            const char* proto_name) {
  DCHECK(proto_name == kVerdictProto || proto_name == kRealTimeThreatInfoProto);
  std::vector<std::string> expired_keys;
  for (const auto& item : verdict_dictionary->DictItems()) {
    int verdict_received_time;
    T verdict;
    if (!ParseVerdictEntry<T>(&item.second, &verdict_received_time, &verdict,
                              proto_name) ||
        IsCacheExpired(verdict_received_time, verdict.cache_duration_sec())) {
      expired_keys.push_back(item.first);
    }
  }

  for (const std::string& key : expired_keys)
    verdict_dictionary->RemoveKey(key);

  return expired_keys.size();
}

// Helper function to determine if the given origin matches content settings
// map's patterns.
bool OriginMatchPrimaryPattern(
    const GURL& origin,
    const ContentSettingsPattern& primary_pattern,
    const ContentSettingsPattern& secondary_pattern_unused) {
  return ContentSettingsPattern::FromURLNoWildcard(origin) == primary_pattern;
}

std::string GetKeyOfTypeFromTriggerType(
    LoginReputationClientRequest::TriggerType trigger_type,
    ReusedPasswordAccountType password_type) {
  return trigger_type == LoginReputationClientRequest::UNFAMILIAR_LOGIN_PAGE
             ? kPasswordOnFocusCacheKey
             : base::NumberToString(
                   static_cast<std::underlying_type_t<
                       ReusedPasswordAccountType::AccountType>>(
                       password_type.account_type()));
}

template <class T>
typename T::VerdictType GetMostMatchingCachedVerdict(
    const GURL& url,
    const std::string& type_key,
    scoped_refptr<HostContentSettingsMap> content_settings,
    const ContentSettingsType contents_setting_type,
    const char* proto_name,
    T* out_response) {
  DCHECK(proto_name == kVerdictProto || proto_name == kRealTimeThreatInfoProto);

  GURL hostname = GetHostNameWithHTTPScheme(url);
  std::unique_ptr<base::DictionaryValue> cache_dictionary =
      base::DictionaryValue::From(content_settings->GetWebsiteSetting(
          hostname, GURL(), contents_setting_type, std::string(), nullptr));

  if (!cache_dictionary || cache_dictionary->empty())
    return T::VERDICT_TYPE_UNSPECIFIED;

  base::Value* verdict_dictionary =
      cache_dictionary->FindKeyOfType(type_key, base::Value::Type::DICTIONARY);
  if (!verdict_dictionary) {
    return T::VERDICT_TYPE_UNSPECIFIED;
  }

  std::vector<std::string> paths;
  GeneratePathVariantsWithoutQuery(url, &paths);
  int max_path_depth = -1;
  typename T::VerdictType most_matching_verdict_type =
      T::VERDICT_TYPE_UNSPECIFIED;
  // For all the verdicts of the same origin, we key them by |cache_expression|.
  // Its corresponding value is a DictionaryValue contains its creation time and
  // the serialized verdict proto.
  for (const auto& item : verdict_dictionary->DictItems()) {
    int verdict_received_time;
    T verdict;
    // Ignore any entry that we cannot parse. These invalid entries will be
    // cleaned up during shutdown.
    if (!ParseVerdictEntry<T>(&item.second, &verdict_received_time, &verdict,
                              proto_name))
      continue;
    // Since verdict content settings are keyed by origin, we only need to
    // compare the path part of the cache_expression and the given url.
    std::string cache_expression_path =
        GetCacheExpressionPath(verdict.cache_expression());

    // Finds the most specific match.
    int path_depth = static_cast<int>(GetPathDepth(cache_expression_path));
    if (path_depth > max_path_depth &&
        PathVariantsMatchCacheExpression(paths, cache_expression_path)) {
      max_path_depth = path_depth;
      // If the most matching verdict is expired, set the result to
      // VERDICT_TYPE_UNSPECIFIED.
      most_matching_verdict_type =
          IsCacheExpired(verdict_received_time, verdict.cache_duration_sec())
              ? T::VERDICT_TYPE_UNSPECIFIED
              : verdict.verdict_type();
      out_response->CopyFrom(verdict);
    }
  }
  return most_matching_verdict_type;
}

}  // namespace

VerdictCacheManager::VerdictCacheManager(
    history::HistoryService* history_service,
    scoped_refptr<HostContentSettingsMap> content_settings)
    : stored_verdict_count_password_on_focus_(base::nullopt),
      stored_verdict_count_password_entry_(base::nullopt),
      content_settings_(content_settings) {
  if (history_service)
    history_service_observer_.Add(history_service);
}

VerdictCacheManager::~VerdictCacheManager() {
  CleanUpExpiredVerdicts();
  history_service_observer_.RemoveAll();
  weak_factory_.InvalidateWeakPtrs();
}

void VerdictCacheManager::CachePhishGuardVerdict(
    const GURL& url,
    LoginReputationClientRequest::TriggerType trigger_type,
    ReusedPasswordAccountType password_type,
    const LoginReputationClientResponse& verdict,
    const base::Time& receive_time) {
  DCHECK(content_settings_);
  DCHECK(trigger_type == LoginReputationClientRequest::UNFAMILIAR_LOGIN_PAGE ||
         trigger_type == LoginReputationClientRequest::PASSWORD_REUSE_EVENT);

  GURL hostname = GetHostNameWithHTTPScheme(url);

  std::unique_ptr<base::DictionaryValue> cache_dictionary =
      base::DictionaryValue::From(content_settings_->GetWebsiteSetting(
          hostname, GURL(), ContentSettingsType::PASSWORD_PROTECTION,
          std::string(), nullptr));

  if (!cache_dictionary)
    cache_dictionary = std::make_unique<base::DictionaryValue>();

  std::unique_ptr<base::DictionaryValue> verdict_entry(
      CreateDictionaryFromVerdict<LoginReputationClientResponse>(
          verdict, receive_time, kVerdictProto));

  std::string type_key =
      GetKeyOfTypeFromTriggerType(trigger_type, password_type);
  base::Value* verdict_dictionary =
      cache_dictionary->FindKeyOfType(type_key, base::Value::Type::DICTIONARY);
  if (!verdict_dictionary) {
    verdict_dictionary = cache_dictionary->SetKey(
        type_key, base::Value(base::Value::Type::DICTIONARY));
  }

  // Increases stored verdict count if we haven't seen this cache expression
  // before.
  if (!verdict_dictionary->FindKey(verdict.cache_expression())) {
    base::Optional<size_t>* stored_verdict_count =
        trigger_type == LoginReputationClientRequest::UNFAMILIAR_LOGIN_PAGE
            ? &stored_verdict_count_password_on_focus_
            : &stored_verdict_count_password_entry_;
    *stored_verdict_count = GetStoredPhishGuardVerdictCount(trigger_type) + 1;
  }

  // If same cache_expression is already in this verdict_dictionary, we simply
  // override it.
  verdict_dictionary->SetKey(
      verdict.cache_expression(),
      base::Value::FromUniquePtrValue(std::move(verdict_entry)));
  content_settings_->SetWebsiteSettingDefaultScope(
      hostname, GURL(), ContentSettingsType::PASSWORD_PROTECTION, std::string(),
      std::move(cache_dictionary));
}

LoginReputationClientResponse::VerdictType
VerdictCacheManager::GetCachedPhishGuardVerdict(
    const GURL& url,
    LoginReputationClientRequest::TriggerType trigger_type,
    ReusedPasswordAccountType password_type,
    LoginReputationClientResponse* out_response) {
  DCHECK(trigger_type == LoginReputationClientRequest::UNFAMILIAR_LOGIN_PAGE ||
         trigger_type == LoginReputationClientRequest::PASSWORD_REUSE_EVENT);

  std::string type_key =
      GetKeyOfTypeFromTriggerType(trigger_type, password_type);

  return GetMostMatchingCachedVerdict<LoginReputationClientResponse>(
      url, type_key, content_settings_,
      ContentSettingsType::PASSWORD_PROTECTION, kVerdictProto, out_response);
}

size_t VerdictCacheManager::GetStoredPhishGuardVerdictCount(
    LoginReputationClientRequest::TriggerType trigger_type) {
  DCHECK(content_settings_);
  DCHECK(trigger_type == LoginReputationClientRequest::UNFAMILIAR_LOGIN_PAGE ||
         trigger_type == LoginReputationClientRequest::PASSWORD_REUSE_EVENT);
  base::Optional<size_t>* stored_verdict_count =
      trigger_type == LoginReputationClientRequest::UNFAMILIAR_LOGIN_PAGE
          ? &stored_verdict_count_password_on_focus_
          : &stored_verdict_count_password_entry_;
  // If we have already computed this, return its value.
  if (stored_verdict_count->has_value())
    return stored_verdict_count->value();

  ContentSettingsForOneType password_protection_settings;
  content_settings_->GetSettingsForOneType(
      ContentSettingsType::PASSWORD_PROTECTION, std::string(),
      &password_protection_settings);
  stored_verdict_count_password_on_focus_ = 0;
  stored_verdict_count_password_entry_ = 0;
  if (password_protection_settings.empty())
    return 0;

  for (const ContentSettingPatternSource& source :
       password_protection_settings) {
    std::unique_ptr<base::DictionaryValue> cache_dictionary =
        base::DictionaryValue::From(content_settings_->GetWebsiteSetting(
            GURL(source.primary_pattern.ToString()), GURL(),
            ContentSettingsType::PASSWORD_PROTECTION, std::string(), nullptr));
    if (cache_dictionary.get() && !cache_dictionary->empty()) {
      for (const auto& item : cache_dictionary->DictItems()) {
        if (item.first == base::StringPiece(kPasswordOnFocusCacheKey)) {
          stored_verdict_count_password_on_focus_.value() +=
              item.second.DictSize();
        } else {
          stored_verdict_count_password_entry_.value() +=
              item.second.DictSize();
        }
      }
    }
  }

  return stored_verdict_count->value();
}

void VerdictCacheManager::CacheRealTimeUrlVerdict(
    const GURL& url,
    const RTLookupResponse& verdict,
    const base::Time& receive_time) {
  GURL hostname = GetHostNameWithHTTPScheme(url);
  std::unique_ptr<base::DictionaryValue> cache_dictionary =
      base::DictionaryValue::From(content_settings_->GetWebsiteSetting(
          hostname, GURL(), ContentSettingsType::SAFE_BROWSING_URL_CHECK_DATA,
          std::string(), nullptr));

  if (!cache_dictionary)
    cache_dictionary = std::make_unique<base::DictionaryValue>();

  base::Value* verdict_dictionary = cache_dictionary->FindKeyOfType(
      kRealTimeUrlCacheKey, base::Value::Type::DICTIONARY);
  if (!verdict_dictionary) {
    verdict_dictionary = cache_dictionary->SetKey(
        kRealTimeUrlCacheKey, base::Value(base::Value::Type::DICTIONARY));
  }

  std::vector<std::string> visited_cache_expressions;
  for (const auto& threat_info : verdict.threat_info()) {
    std::string cache_expression = threat_info.cache_expression();
    // TODO(crbug.com/1033692): For the same cache_expression, threat_info is in
    // decreasing order of severity. To avoid lower severity threat being
    // overridden by higher one, only store threat info that is first seen for a
    // cache expression.
    if (base::Contains(visited_cache_expressions, cache_expression))
      continue;
    std::unique_ptr<base::DictionaryValue> threat_info_entry(
        CreateDictionaryFromVerdict<RTLookupResponse::ThreatInfo>(
            threat_info, receive_time, kRealTimeThreatInfoProto));
    // Increases stored verdict count if we haven't seen this cache expression
    // before.
    if (!verdict_dictionary->FindKey(cache_expression)) {
      stored_verdict_count_real_time_url_check_++;
    }
    verdict_dictionary->SetKey(
        cache_expression,
        base::Value::FromUniquePtrValue(std::move(threat_info_entry)));
    visited_cache_expressions.push_back(cache_expression);
  }
  content_settings_->SetWebsiteSettingDefaultScope(
      hostname, GURL(), ContentSettingsType::SAFE_BROWSING_URL_CHECK_DATA,
      std::string(), std::move(cache_dictionary));
}

RTLookupResponse::ThreatInfo::VerdictType
VerdictCacheManager::GetCachedRealTimeUrlVerdict(
    const GURL& url,
    RTLookupResponse::ThreatInfo* out_threat_info) {
  return GetMostMatchingCachedVerdict<RTLookupResponse::ThreatInfo>(
      url, kRealTimeUrlCacheKey, content_settings_,
      ContentSettingsType::SAFE_BROWSING_URL_CHECK_DATA,
      kRealTimeThreatInfoProto, out_threat_info);
}

void VerdictCacheManager::CleanUpExpiredVerdicts() {
  DCHECK(content_settings_);

  CleanUpExpiredPhishGuardVerdicts();
  CleanUpExpiredRealTimeUrlCheckVerdicts();
}

void VerdictCacheManager::CleanUpExpiredPhishGuardVerdicts() {
  if (GetStoredPhishGuardVerdictCount(
          LoginReputationClientRequest::UNFAMILIAR_LOGIN_PAGE) <= 0 &&
      GetStoredPhishGuardVerdictCount(
          LoginReputationClientRequest::PASSWORD_REUSE_EVENT) <= 0)
    return;

  ContentSettingsForOneType password_protection_settings;
  content_settings_->GetSettingsForOneType(
      ContentSettingsType::PASSWORD_PROTECTION, std::string(),
      &password_protection_settings);

  for (const ContentSettingPatternSource& source :
       password_protection_settings) {
    GURL primary_pattern_url = GURL(source.primary_pattern.ToString());
    // Find all verdicts associated with this origin.
    std::unique_ptr<base::DictionaryValue> cache_dictionary =
        base::DictionaryValue::From(content_settings_->GetWebsiteSetting(
            primary_pattern_url, GURL(),
            ContentSettingsType::PASSWORD_PROTECTION, std::string(), nullptr));
    bool has_expired_password_on_focus_entry = RemoveExpiredPhishGuardVerdicts(
        LoginReputationClientRequest::UNFAMILIAR_LOGIN_PAGE,
        cache_dictionary.get());
    bool has_expired_password_reuse_entry = RemoveExpiredPhishGuardVerdicts(
        LoginReputationClientRequest::PASSWORD_REUSE_EVENT,
        cache_dictionary.get());

    if (cache_dictionary->size() == 0u) {
      content_settings_->ClearSettingsForOneTypeWithPredicate(
          ContentSettingsType::PASSWORD_PROTECTION, base::Time(),
          base::Time::Max(),
          base::BindRepeating(&OriginMatchPrimaryPattern, primary_pattern_url));
    } else if (has_expired_password_on_focus_entry ||
               has_expired_password_reuse_entry) {
      // Set the website setting of this origin with the updated
      // |cache_dictionary|.
      content_settings_->SetWebsiteSettingDefaultScope(
          primary_pattern_url, GURL(), ContentSettingsType::PASSWORD_PROTECTION,
          std::string(), std::move(cache_dictionary));
    }
  }
}

void VerdictCacheManager::CleanUpExpiredRealTimeUrlCheckVerdicts() {
  ContentSettingsForOneType safe_browsing_url_check_data_settings;
  content_settings_->GetSettingsForOneType(
      ContentSettingsType::SAFE_BROWSING_URL_CHECK_DATA, std::string(),
      &safe_browsing_url_check_data_settings);

  for (const ContentSettingPatternSource& source :
       safe_browsing_url_check_data_settings) {
    GURL primary_pattern_url = GURL(source.primary_pattern.ToString());
    // Find all verdicts associated with this origin.
    std::unique_ptr<base::DictionaryValue> cache_dictionary =
        base::DictionaryValue::From(content_settings_->GetWebsiteSetting(
            primary_pattern_url, GURL(),
            ContentSettingsType::SAFE_BROWSING_URL_CHECK_DATA, std::string(),
            nullptr));
    bool has_expired_entry =
        RemoveExpiredRealTimeUrlCheckVerdicts(cache_dictionary.get());

    if (cache_dictionary->size() == 0u) {
      content_settings_->ClearSettingsForOneTypeWithPredicate(
          ContentSettingsType::SAFE_BROWSING_URL_CHECK_DATA, base::Time(),
          base::Time::Max(),
          base::BindRepeating(&OriginMatchPrimaryPattern, primary_pattern_url));
    } else if (has_expired_entry) {
      // Set the website setting of this origin with the updated
      // |cache_dictionary|.
      content_settings_->SetWebsiteSettingDefaultScope(
          primary_pattern_url, GURL(),
          ContentSettingsType::SAFE_BROWSING_URL_CHECK_DATA, std::string(),
          std::move(cache_dictionary));
    }
  }
}

// Overridden from history::HistoryServiceObserver.
void VerdictCacheManager::OnURLsDeleted(
    history::HistoryService* history_service,
    const history::DeletionInfo& deletion_info) {
  base::PostTask(FROM_HERE, {content::BrowserThread::UI},
                 base::BindRepeating(
                     &VerdictCacheManager::RemoveContentSettingsOnURLsDeleted,
                     GetWeakPtr(), deletion_info.IsAllHistory(),
                     deletion_info.deleted_rows()));
}

// Overridden from history::HistoryServiceObserver.
void VerdictCacheManager::HistoryServiceBeingDeleted(
    history::HistoryService* history_service) {
  history_service_observer_.Remove(history_service);
}

bool VerdictCacheManager::RemoveExpiredPhishGuardVerdicts(
    LoginReputationClientRequest::TriggerType trigger_type,
    base::DictionaryValue* cache_dictionary) {
  DCHECK(trigger_type == LoginReputationClientRequest::UNFAMILIAR_LOGIN_PAGE ||
         trigger_type == LoginReputationClientRequest::PASSWORD_REUSE_EVENT);
  if (!cache_dictionary || cache_dictionary->empty())
    return false;

  size_t verdicts_removed = 0;
  std::vector<std::string> empty_keys;
  for (auto item : cache_dictionary->DictItems()) {
    if (trigger_type == LoginReputationClientRequest::UNFAMILIAR_LOGIN_PAGE &&
        item.first == std::string(kPasswordOnFocusCacheKey)) {
      size_t removed_cnt = RemoveExpiredEntries<LoginReputationClientResponse>(
          &item.second, kVerdictProto);
      verdicts_removed += removed_cnt;
      if (stored_verdict_count_password_on_focus_.has_value())
        stored_verdict_count_password_on_focus_.value() -= removed_cnt;
    } else {
      size_t removed_cnt = RemoveExpiredEntries<LoginReputationClientResponse>(
          &item.second, kVerdictProto);
      verdicts_removed += removed_cnt;
      if (stored_verdict_count_password_entry_.has_value())
        stored_verdict_count_password_entry_.value() -= removed_cnt;
    }

    if (item.second.DictSize() == 0U)
      empty_keys.push_back(item.first);
  }
  for (const auto& key : empty_keys)
    cache_dictionary->RemoveKey(key);

  return verdicts_removed > 0U;
}

bool VerdictCacheManager::RemoveExpiredRealTimeUrlCheckVerdicts(
    base::DictionaryValue* cache_dictionary) {
  if (!cache_dictionary || cache_dictionary->empty())
    return false;

  size_t verdicts_removed = 0;
  std::vector<std::string> empty_keys;
  for (auto item : cache_dictionary->DictItems()) {
    size_t removed_cnt = RemoveExpiredEntries<RTLookupResponse::ThreatInfo>(
        &item.second, kRealTimeThreatInfoProto);
    verdicts_removed += removed_cnt;
    stored_verdict_count_real_time_url_check_ -= removed_cnt;
    if (item.second.DictSize() == 0U)
      empty_keys.push_back(item.first);
  }
  for (const auto& key : empty_keys)
    cache_dictionary->RemoveKey(key);

  return verdicts_removed > 0U;
}

void VerdictCacheManager::RemoveContentSettingsOnURLsDeleted(
    bool all_history,
    const history::URLRows& deleted_rows) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(content_settings_);

  if (all_history) {
    content_settings_->ClearSettingsForOneType(
        ContentSettingsType::PASSWORD_PROTECTION);
    stored_verdict_count_password_on_focus_ = 0;
    stored_verdict_count_password_entry_ = 0;
    stored_verdict_count_real_time_url_check_ = 0;
    content_settings_->ClearSettingsForOneType(
        ContentSettingsType::SAFE_BROWSING_URL_CHECK_DATA);
    return;
  }

  // For now, if a URL is deleted from history, we simply remove all the
  // cached verdicts of the same origin. This is a pretty aggressive deletion.
  // We might revisit this logic later to decide if we want to only delete the
  // cached verdict whose cache expression matches this URL.
  for (const history::URLRow& row : deleted_rows) {
    if (!row.url().SchemeIsHTTPOrHTTPS())
      continue;

    GURL url_key = GetHostNameWithHTTPScheme(row.url());
    stored_verdict_count_password_on_focus_ =
        GetStoredPhishGuardVerdictCount(
            LoginReputationClientRequest::UNFAMILIAR_LOGIN_PAGE) -
        GetPhishGuardVerdictCountForURL(
            url_key, LoginReputationClientRequest::UNFAMILIAR_LOGIN_PAGE);
    stored_verdict_count_password_entry_ =
        GetStoredPhishGuardVerdictCount(
            LoginReputationClientRequest::PASSWORD_REUSE_EVENT) -
        GetPhishGuardVerdictCountForURL(
            url_key, LoginReputationClientRequest::PASSWORD_REUSE_EVENT);
    stored_verdict_count_real_time_url_check_ -=
        GetRealTimeUrlCheckVerdictCountForURL(url_key);
    content_settings_->ClearSettingsForOneTypeWithPredicate(
        ContentSettingsType::PASSWORD_PROTECTION, base::Time(),
        base::Time::Max(),
        base::BindRepeating(&OriginMatchPrimaryPattern, url_key));
    content_settings_->ClearSettingsForOneTypeWithPredicate(
        ContentSettingsType::SAFE_BROWSING_URL_CHECK_DATA, base::Time(),
        base::Time::Max(),
        base::BindRepeating(&OriginMatchPrimaryPattern, url_key));
  }
}

size_t VerdictCacheManager::GetPhishGuardVerdictCountForURL(
    const GURL& url,
    LoginReputationClientRequest::TriggerType trigger_type) {
  DCHECK(trigger_type == LoginReputationClientRequest::UNFAMILIAR_LOGIN_PAGE ||
         trigger_type == LoginReputationClientRequest::PASSWORD_REUSE_EVENT);
  std::unique_ptr<base::DictionaryValue> cache_dictionary =
      base::DictionaryValue::From(content_settings_->GetWebsiteSetting(
          url, GURL(), ContentSettingsType::PASSWORD_PROTECTION, std::string(),
          nullptr));
  if (!cache_dictionary || cache_dictionary->empty())
    return 0;

  int verdict_cnt = 0;
  if (trigger_type == LoginReputationClientRequest::UNFAMILIAR_LOGIN_PAGE) {
    base::Value* password_on_focus_dict = nullptr;
    password_on_focus_dict =
        cache_dictionary->FindKey(kPasswordOnFocusCacheKey);
    verdict_cnt +=
        password_on_focus_dict ? password_on_focus_dict->DictSize() : 0;
  } else {
    for (const auto& item : cache_dictionary->DictItems()) {
      if (item.first == kPasswordOnFocusCacheKey)
        continue;
      verdict_cnt += item.second.DictSize();
    }
  }
  return verdict_cnt;
}

size_t VerdictCacheManager::GetRealTimeUrlCheckVerdictCountForURL(
    const GURL& url) {
  std::unique_ptr<base::DictionaryValue> cache_dictionary =
      base::DictionaryValue::From(content_settings_->GetWebsiteSetting(
          url, GURL(), ContentSettingsType::PASSWORD_PROTECTION, std::string(),
          nullptr));
  if (!cache_dictionary || cache_dictionary->empty())
    return 0;
  base::Value* verdict_dictionary =
      cache_dictionary->FindKey(kRealTimeUrlCacheKey);
  return verdict_dictionary ? verdict_dictionary->DictSize() : 0;
}

}  // namespace safe_browsing
