// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ANDROID_AUTOFILL_ASSISTANT_GENERIC_UI_CONTROLLER_ANDROID_H_
#define CHROME_BROWSER_ANDROID_AUTOFILL_ASSISTANT_GENERIC_UI_CONTROLLER_ANDROID_H_

#include <map>
#include <memory>
#include <string>

#include "base/android/jni_android.h"
#include "base/android/scoped_java_ref.h"
#include "base/macros.h"
#include "components/autofill_assistant/browser/service.pb.h"

namespace autofill_assistant {
class EventHandler;
class InteractionHandlerAndroid;
class UserModel;

class GenericUiControllerAndroid {
 public:
  // Attempts to creata a new instance. May fail if the proto is invalid.
  // |delegate|, |user_model| and |event_handler| must outlive this instance.
  static std::unique_ptr<GenericUiControllerAndroid> CreateFromProto(
      const GenericUserInterfaceProto& proto,
      const base::android::ScopedJavaLocalRef<jobject>& jcontext,
      const base::android::ScopedJavaGlobalRef<jobject>& delegate,
      UserModel* user_model,
      EventHandler* event_handler);

  base::android::ScopedJavaGlobalRef<jobject> GetRootView() {
    return jroot_view_;
  }

  GenericUiControllerAndroid(
      base::android::ScopedJavaGlobalRef<jobject> jroot_view,
      std::unique_ptr<
          std::map<std::string, base::android::ScopedJavaGlobalRef<jobject>>>
          views,
      std::unique_ptr<InteractionHandlerAndroid> interaction_handler);
  ~GenericUiControllerAndroid();

 private:
  void CreateViewLayout();
  void CreateInteractions();

  base::android::ScopedJavaGlobalRef<jobject> jroot_view_;

  // Maps view-ids to android views.
  std::unique_ptr<
      std::map<std::string, base::android::ScopedJavaGlobalRef<jobject>>>
      views_;
  std::unique_ptr<InteractionHandlerAndroid> interaction_handler_;

  DISALLOW_COPY_AND_ASSIGN(GenericUiControllerAndroid);
};

}  //  namespace autofill_assistant

#endif  //  CHROME_BROWSER_ANDROID_AUTOFILL_ASSISTANT_GENERIC_UI_CONTROLLER_ANDROID_H_
