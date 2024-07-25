// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/fl_view_handler.h"

#include "flutter/shell/platform/linux/fl_engine_private.h"
#include "flutter/shell/platform/linux/fl_view_private.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_method_channel.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_standard_method_codec.h"

static constexpr char kChannelName[] = "flutter/view";
static constexpr char kBadArgumentsError[] = "Bad Arguments";
static constexpr char kNoEngineError[] = "No Engine";
static constexpr char kAddMethod[] = "Add";
static constexpr char kWidthKey[] = "width";
static constexpr char kHeightKey[] = "height";

struct _FlViewHandler {
  GObject parent_instance;

  // The engine this handler is using.
  GWeakRef engine;

  // Channel to engine.
  FlMethodChannel* channel;

  GCancellable* cancellable;
};

typedef struct {
  FlViewHandler* self;

  // Request to add view.
  FlMethodCall* method_call;
} AddData;

static AddData* add_data_new(FlViewHandler* self, FlMethodCall* method_call) {
  AddData* data = g_new0(AddData, 1);

  data->self = self;
  data->method_call = FL_METHOD_CALL(g_object_ref(method_call));

  return data;
}

static void add_data_free(AddData* data) {
  g_object_unref(data->method_call);
  g_free(data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AddData, add_data_free)

// Sends the method call response to Flutter.
static void send_response(FlMethodCall* method_call,
                          FlMethodResponse* response) {
  g_autoptr(GError) error = nullptr;
  if (!fl_method_call_respond(method_call, response, &error)) {
    g_warning("Failed to send method call response: %s", error->message);
  }
}

static void view_added_cb(GObject* object,
                          GAsyncResult* result,
                          gpointer user_data) {
  g_autoptr(AddData) data = static_cast<AddData*>(user_data);

  g_autoptr(GError) error = nullptr;
  FlutterViewId view_id =
      fl_engine_add_view_finish(FL_ENGINE(object), result, &error);
  if (view_id == 0) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      return;
    }
    g_warning("Failed to add view: %s", error->message);
    return;
  }

  FlEngine* engine = FL_ENGINE(object);

  // FIXME: Allow this to be overridden
  GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  FlView* view = fl_view_new2(engine, view_id);
  gtk_widget_show(GTK_WIDGET(view));
  gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(view));
  gtk_window_present(GTK_WINDOW(window));

  g_autoptr(FlValue) return_value = fl_value_new_int(view_id);
  g_autoptr(FlMethodSuccessResponse) response =
      fl_method_success_response_new(return_value);
  send_response(data->method_call, FL_METHOD_RESPONSE(response));
}

static FlMethodResponse* add_view(FlViewHandler* self,
                                  FlMethodCall* method_call) {
  FlValue* args = fl_method_call_get_args(method_call);

  if (fl_value_get_type(args) != FL_VALUE_TYPE_MAP) {
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        kBadArgumentsError, "Argument map missing or malformed", nullptr));
  }

  FlValue* width_value = fl_value_lookup_string(args, kWidthKey);
  if (width_value == nullptr ||
      fl_value_get_type(width_value) != FL_VALUE_TYPE_INT) {
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        kBadArgumentsError, "Missing width", nullptr));
  }
  FlValue* height_value = fl_value_lookup_string(args, kHeightKey);
  if (height_value == nullptr ||
      fl_value_get_type(height_value) != FL_VALUE_TYPE_INT) {
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        kBadArgumentsError, "Missing height", nullptr));
  }

  int64_t width = fl_value_get_int(width_value);
  int64_t height = fl_value_get_int(height_value);

  // FIXME: Get correct ratio.
  double pixel_ratio = 1.0;

  g_autoptr(FlEngine) engine = FL_ENGINE(g_weak_ref_get(&self->engine));
  if (engine == nullptr) {
    return FL_METHOD_RESPONSE(
        fl_method_error_response_new(kNoEngineError, "No engine", nullptr));
  }

  fl_engine_add_view(engine, width, height, pixel_ratio, self->cancellable,
                     view_added_cb, add_data_new(self, method_call));

  // Will respond later.
  return nullptr;
}

// Called when a method call is received from Flutter.
static void method_call_cb(FlMethodChannel* channel,
                           FlMethodCall* method_call,
                           gpointer user_data) {
  FlViewHandler* self = FL_VIEW_HANDLER(user_data);

  const gchar* method = fl_method_call_get_name(method_call);

  g_autoptr(FlMethodResponse) response = nullptr;
  if (strcmp(method, kAddMethod) == 0) {
    response = add_view(self, method_call);
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  if (response != nullptr) {
    send_response(method_call, response);
  }
}

G_DEFINE_TYPE(FlViewHandler, fl_view_handler, G_TYPE_OBJECT)

static void fl_view_handler_dispose(GObject* object) {
  FlViewHandler* self = FL_VIEW_HANDLER(object);

  g_cancellable_cancel(self->cancellable);

  g_weak_ref_clear(&self->engine);
  g_clear_object(&self->channel);
  g_clear_object(&self->cancellable);

  G_OBJECT_CLASS(fl_view_handler_parent_class)->dispose(object);
}

static void fl_view_handler_class_init(FlViewHandlerClass* klass) {
  GObjectClass* object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = fl_view_handler_dispose;
}

static void fl_view_handler_init(FlViewHandler* self) {}

G_MODULE_EXPORT FlViewHandler* fl_view_handler_new(FlEngine* engine) {
  FlViewHandler* self =
      FL_VIEW_HANDLER(g_object_new(fl_view_handler_get_type(), nullptr));

  g_weak_ref_init(&self->engine, engine);
  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  self->channel = fl_method_channel_new(fl_engine_get_binary_messenger(engine),
                                        kChannelName, FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(self->channel, method_call_cb, self,
                                            nullptr);

  return self;
}
