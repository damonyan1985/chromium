// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/remote_objects/remote_object.h"

namespace blink {

gin::WrapperInfo RemoteObject::kWrapperInfo = {gin::kEmbedderNativeGin};

RemoteObject::RemoteObject(v8::Isolate* isolate,
                           RemoteObjectGatewayImpl* gateway,
                           int32_t object_id)
    : gin::NamedPropertyInterceptor(isolate, this),
      gateway_(gateway),
      object_id_(object_id) {}

gin::ObjectTemplateBuilder RemoteObject::GetObjectTemplateBuilder(
    v8::Isolate* isolate) {
  return gin::Wrappable<RemoteObject>::GetObjectTemplateBuilder(isolate)
      .AddNamedPropertyInterceptor();
}

v8::Local<v8::Value> RemoteObject::GetNamedProperty(
    v8::Isolate* isolate,
    const std::string& property) {
  // TODO(crbug.com/794320): implement this.
  ignore_result(object_id_);
  return v8::Local<v8::Value>();
}

std::vector<std::string> RemoteObject::EnumerateNamedProperties(
    v8::Isolate* isolate) {
  // TODO(crbug.com/794320): implement this.
  return std::vector<std::string>();
}

}  // namespace blink
