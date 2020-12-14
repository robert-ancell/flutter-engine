// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/fl_navigation_plugin.h"

#include <cstring>

#include "flutter/shell/platform/linux/public/flutter_linux/fl_json_method_codec.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_method_channel.h"

static constexpr char kChannelName[] = "flutter/navigation";

static constexpr char kRouteUpdatedMethod[] = "routeUpdated";
static constexpr char kRouteInformationUpdatedMethod[] =
    "routeInformationUpdated";
static constexpr char kPopRouteMethod[] = "popRoute";
static constexpr char kPushRouteMethod[] = "pushRoute";
static constexpr char kPushRouteInformationMethod[] = "pushRouteInformation";

static constexpr char kRouteNameKey[] = "routeName";
static constexpr char kPreviousRouteNameKey[] = "previousRouteName";
static constexpr char kLocationKey[] = "location";
static constexpr char kStateKey[] = "state";

struct _FlNavigationPlugin {
  GObject parent_instance;

  FlMethodChannel* channel;
};

G_DEFINE_TYPE(FlNavigationPlugin, fl_navigation_plugin, G_TYPE_OBJECT)

static FlMethodResponse* route_updated(FlNavigationPlugin* self,
                                       FlValue* args) {
  FlValue* route_name_value = fl_value_lookup_string(args, kRouteNameKey);
  const gchar* route_name =
      route_name_value != nullptr &&
              fl_value_get_type(route_name_value) == FL_VALUE_TYPE_STRING
          ? fl_value_get_string(route_name_value)
          : nullptr;
  FlValue* previous_route_name_value =
      fl_value_lookup_string(args, kPreviousRouteNameKey);
  const gchar* previous_route_name =
      previous_route_name_value != nullptr &&
              fl_value_get_type(previous_route_name_value) ==
                  FL_VALUE_TYPE_STRING
          ? fl_value_get_string(previous_route_name_value)
          : nullptr;

  g_printerr("route updated name=%s previous=%s\n", route_name,
             previous_route_name);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
}

static FlMethodResponse* route_information_updated(FlNavigationPlugin* self,
                                                   FlValue* args) {
  FlValue* location_value = fl_value_lookup_string(args, kLocationKey);
  const gchar* location =
      location_value != nullptr &&
              fl_value_get_type(location_value) == FL_VALUE_TYPE_STRING
          ? fl_value_get_string(location_value)
          : nullptr;
  FlValue* state = fl_value_lookup_string(args, kStateKey);

  g_printerr("route info updated location=%s state=%s\n", location,
             fl_value_to_string(state));
  return FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
}

// Called when a method call is received from Flutter.
static void method_call_cb(FlMethodChannel* channel,
                           FlMethodCall* method_call,
                           gpointer user_data) {
  FlNavigationPlugin* self = FL_NAVIGATION_PLUGIN(user_data);

  const gchar* method = fl_method_call_get_name(method_call);
  FlValue* args = fl_method_call_get_args(method_call);

  g_autoptr(FlMethodResponse) response = nullptr;
  if (strcmp(method, kRouteUpdatedMethod) == 0) {
    response = route_updated(self, args);
  } else if (strcmp(method, kRouteInformationUpdatedMethod) == 0) {
    response = route_information_updated(self, args);
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  if (response != nullptr) {
    g_autoptr(GError) error = nullptr;
    if (!fl_method_call_respond(method_call, response, &error)) {
      g_warning("Failed to send method call response: %s", error->message);
    }
  }
}

static void fl_navigation_plugin_dispose(GObject* object) {
  FlNavigationPlugin* self = FL_NAVIGATION_PLUGIN(object);

  g_clear_object(&self->channel);

  G_OBJECT_CLASS(fl_navigation_plugin_parent_class)->dispose(object);
}

static void fl_navigation_plugin_class_init(FlNavigationPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fl_navigation_plugin_dispose;
}

static void fl_navigation_plugin_init(FlNavigationPlugin* self) {}

FlNavigationPlugin* fl_navigation_plugin_new(FlBinaryMessenger* messenger) {
  g_return_val_if_fail(FL_IS_BINARY_MESSENGER(messenger), nullptr);

  FlNavigationPlugin* self = FL_NAVIGATION_PLUGIN(
      g_object_new(fl_navigation_plugin_get_type(), nullptr));

  g_autoptr(FlJsonMethodCodec) codec = fl_json_method_codec_new();
  self->channel =
      fl_method_channel_new(messenger, kChannelName, FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(self->channel, method_call_cb, self,
                                            nullptr);

  return self;
}

void fl_navigation_plugin_pop_route(FlNavigationPlugin* self) {
  g_return_if_fail(FL_IS_NAVIGATION_PLUGIN(self));
  fl_method_channel_invoke_method(self->channel, kPopRouteMethod, nullptr,
                                  nullptr, nullptr, nullptr);
}

void fl_navigation_plugin_push_route(FlNavigationPlugin* self,
                                     const gchar* route) {
  g_return_if_fail(FL_IS_NAVIGATION_PLUGIN(self));

  g_autoptr(FlValue) args = fl_value_new_string(route);
  fl_method_channel_invoke_method(self->channel, kPushRouteMethod, args,
                                  nullptr, nullptr, nullptr);
}

void fl_navigation_plugin_push_route_information(FlNavigationPlugin* self,
                                                 const gchar* location,
                                                 FlValue* state) {
  g_return_if_fail(FL_IS_NAVIGATION_PLUGIN(self));

  g_autoptr(FlValue) args = fl_value_new_map();
  fl_value_set_string_take(args, kLocationKey, fl_value_new_string(location));
  fl_value_set_string_take(args, kStateKey, state);
  fl_method_channel_invoke_method(self->channel, kPushRouteInformationMethod,
                                  args, nullptr, nullptr, nullptr);
}
