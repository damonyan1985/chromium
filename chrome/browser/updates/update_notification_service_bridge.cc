// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/updates/update_notification_service_bridge.h"

#include <utility>

#include "base/android/jni_string.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/android/chrome_jni_headers/UpdateNotificationServiceBridge_jni.h"
#include "chrome/browser/notifications/scheduler/public/notification_params.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_android.h"
#include "chrome/browser/updates/update_notification_info.h"
#include "chrome/browser/updates/update_notification_service.h"
#include "chrome/browser/updates/update_notification_service_factory.h"

using base::android::JavaParamRef;
using base::android::ScopedJavaLocalRef;

namespace updates {

//
// Java -> C++
//
void JNI_UpdateNotificationServiceBridge_Schedule(
    JNIEnv* env,
    const JavaParamRef<jobject>& j_profile,
    const JavaParamRef<jstring>& j_title,
    const JavaParamRef<jstring>& j_message) {
  Profile* profile = ProfileAndroid::FromProfileAndroid(j_profile);
  auto* update_notification_service =
      UpdateNotificationServiceFactory::GetForBrowserContext(profile);
  UpdateNotificationInfo data;
  data.title = ConvertJavaStringToUTF16(env, j_title);
  data.message = ConvertJavaStringToUTF16(env, j_message);
  update_notification_service->Schedule(std::move(data));
}

//
// C++ -> Java
//
void UpdateLastShownTimeStamp(base::Time timestamp) {
  JNIEnv* env = base::android::AttachCurrentThread();
  Java_UpdateNotificationServiceBridge_updateLastShownTimeStamp(
      env, timestamp.ToJavaTime());
}

base::Optional<base::Time> GetLastShownTimeStamp() {
  JNIEnv* env = base::android::AttachCurrentThread();
  auto timestamp =
      Java_UpdateNotificationServiceBridge_getLastShownTimeStamp(env);
  return timestamp == 0
             ? base::nullopt
             : (base::make_optional(base::Time::FromJavaTime(timestamp)));
}

}  // namespace updates
