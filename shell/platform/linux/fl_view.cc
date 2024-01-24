// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/public/flutter_linux/fl_view.h"

#include "flutter/shell/platform/linux/fl_view_private.h"

#include <cstring>

#include "flutter/shell/platform/linux/fl_engine_private.h"
#include "flutter/shell/platform/linux/fl_key_event.h"
#include "flutter/shell/platform/linux/fl_keyboard_view_delegate.h"
#include "flutter/shell/platform/linux/fl_mouse_cursor_plugin.h"
#include "flutter/shell/platform/linux/fl_platform_plugin.h"
#include "flutter/shell/platform/linux/fl_plugin_registrar_private.h"
#include "flutter/shell/platform/linux/fl_renderer_gl.h"
#include "flutter/shell/platform/linux/fl_scrolling_view_delegate.h"
#include "flutter/shell/platform/linux/fl_text_input_plugin.h"
#include "flutter/shell/platform/linux/fl_text_input_view_delegate.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_engine.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_plugin_registry.h"

#include <GL/gl.h>
#include <EGL/egl.h>

//static constexpr int kMicrosecondsPerMillisecond = 1000;

struct _FlView {
  GtkBox parent_instance;

  // Project being run.
  FlDartProject* project;

  // Rendering output.
  FlRenderer* renderer;

  // Engine running @project.
  FlEngine* engine;

  // Pointer button state recorded for sending status updates.
  int64_t button_state;

  // Current state information for the window associated with this view.
  //GdkWindowState window_state;

  // Flutter system channel handlers.
  //FlAccessibilityPlugin* accessibility_plugin;
  FlTextInputPlugin* text_input_plugin;
  FlMouseCursorPlugin* mouse_cursor_plugin;
  FlPlatformPlugin* platform_plugin;

  GtkGLArea* gl_area;

  // Tracks whether mouse pointer is inside the view.
  gboolean pointer_inside;

  /* FlKeyboardViewDelegate related properties */
  KeyboardLayoutNotifier keyboard_layout_notifier;
  //GdkKeymap* keymap;
  gulong keymap_keys_changed_cb_id;  // Signal connection ID for
                                     // keymap-keys-changed
  gulong window_state_cb_id;  // Signal connection ID for window-state-changed
};

enum { kPropFlutterProject = 1, kPropLast };

static void fl_view_plugin_registry_iface_init(
    FlPluginRegistryInterface* iface);

static void fl_view_keyboard_delegate_iface_init(
    FlKeyboardViewDelegateInterface* iface);

static void fl_view_scrolling_delegate_iface_init(
    FlScrollingViewDelegateInterface* iface);

static void fl_view_text_input_delegate_iface_init(
    FlTextInputViewDelegateInterface* iface);

G_DEFINE_TYPE_WITH_CODE(
    FlView,
    fl_view,
    GTK_TYPE_BOX,
    G_IMPLEMENT_INTERFACE(fl_plugin_registry_get_type(),
                          fl_view_plugin_registry_iface_init)
        G_IMPLEMENT_INTERFACE(fl_keyboard_view_delegate_get_type(),
                              fl_view_keyboard_delegate_iface_init)
            G_IMPLEMENT_INTERFACE(fl_scrolling_view_delegate_get_type(),
                                  fl_view_scrolling_delegate_iface_init)
                G_IMPLEMENT_INTERFACE(fl_text_input_view_delegate_get_type(),
                                      fl_view_text_input_delegate_iface_init))

#if 0
// Signal handler for GtkWidget::delete-event
static gboolean window_delete_event_cb(GtkWidget* widget,
                                       GdkEvent* event,
                                       FlView* self) {
  fl_platform_plugin_request_app_exit(self->platform_plugin);
  // Stop the event from propagating.
  return TRUE;
}
#endif

// Called when the engine updates accessibility nodes.
static void update_semantics_node_cb(FlEngine* engine,
                                     const FlutterSemanticsNode* node,
                                     gpointer user_data) {
  //FlView* self = FL_VIEW(user_data);

  //fl_accessibility_plugin_handle_update_semantics_node(
  //    self->accessibility_plugin, node);
}

// Invoked by the engine right before the engine is restarted.
//
// This method should reset states to be as if the engine had just been started,
// which usually indicates the user has requested a hot restart (Shift-R in the
// Flutter CLI.)
static void on_pre_engine_restart_cb(FlEngine* engine, gpointer user_data) {
  FlView* self = FL_VIEW(user_data);

  g_clear_object(&self->text_input_plugin);
}

// Implements FlPluginRegistry::get_registrar_for_plugin.
static FlPluginRegistrar* fl_view_get_registrar_for_plugin(
    FlPluginRegistry* registry,
    const gchar* name) {
  FlView* self = FL_VIEW(registry);

  return fl_plugin_registrar_new(self,
                                 fl_engine_get_binary_messenger(self->engine),
                                 fl_engine_get_texture_registrar(self->engine));
}

static void fl_view_plugin_registry_iface_init(
    FlPluginRegistryInterface* iface) {
  iface->get_registrar_for_plugin = fl_view_get_registrar_for_plugin;
}

static void fl_view_keyboard_delegate_iface_init(
    FlKeyboardViewDelegateInterface* iface) {
  iface->send_key_event =
      [](FlKeyboardViewDelegate* view_delegate, const FlutterKeyEvent* event,
         FlutterKeyEventCallback callback, void* user_data) {
        FlView* self = FL_VIEW(view_delegate);
        if (self->engine != nullptr) {
          fl_engine_send_key_event(self->engine, event, callback, user_data);
        };
      };

  iface->text_filter_key_press = [](FlKeyboardViewDelegate* view_delegate,
                                    FlKeyEvent* event) {
    FlView* self = FL_VIEW(view_delegate);
    return fl_text_input_plugin_filter_keypress(self->text_input_plugin, event);
  };

  iface->get_messenger = [](FlKeyboardViewDelegate* view_delegate) {
    FlView* self = FL_VIEW(view_delegate);
    return fl_engine_get_binary_messenger(self->engine);
  };

  iface->redispatch_event = [](FlKeyboardViewDelegate* view_delegate,
                               std::unique_ptr<FlKeyEvent> in_event) {
    FlKeyEvent* event = in_event.release();
    GdkEvent* gdk_event = reinterpret_cast<GdkEvent*>(event->origin);
    GdkEventType event_type = gdk_event_get_event_type(gdk_event);
    g_return_if_fail(event_type == GDK_KEY_PRESS ||
                     event_type == GDK_KEY_RELEASE);
    //gdk_event_put(gdk_event);
    fl_key_event_dispose(event);
  };

  iface->subscribe_to_layout_change = [](FlKeyboardViewDelegate* view_delegate,
                                         KeyboardLayoutNotifier notifier) {
    FlView* self = FL_VIEW(view_delegate);
    self->keyboard_layout_notifier = std::move(notifier);
  };

  iface->lookup_key = [](FlKeyboardViewDelegate* view_delegate,
                         const GdkKeymapKey* key) -> guint {
    //FlView* self = FL_VIEW(view_delegate);
    //g_return_val_if_fail(self->keymap != nullptr, 0);
    return 0;//gdk_keymap_lookup_key(self->keymap, key);
  };

  iface->get_keyboard_state =
      [](FlKeyboardViewDelegate* view_delegate) -> GHashTable* {
    FlView* self = FL_VIEW(view_delegate);
    return fl_view_get_keyboard_state(self);
  };
}

static void fl_view_scrolling_delegate_iface_init(
    FlScrollingViewDelegateInterface* iface) {
  iface->send_mouse_pointer_event =
      [](FlScrollingViewDelegate* view_delegate, FlutterPointerPhase phase,
         size_t timestamp, double x, double y, double scroll_delta_x,
         double scroll_delta_y, int64_t buttons) {
        FlView* self = FL_VIEW(view_delegate);
        if (self->engine != nullptr) {
          fl_engine_send_mouse_pointer_event(self->engine, phase, timestamp, x,
                                             y, scroll_delta_x, scroll_delta_y,
                                             buttons);
        }
      };
  iface->send_pointer_pan_zoom_event =
      [](FlScrollingViewDelegate* view_delegate, size_t timestamp, double x,
         double y, FlutterPointerPhase phase, double pan_x, double pan_y,
         double scale, double rotation) {
        FlView* self = FL_VIEW(view_delegate);
        if (self->engine != nullptr) {
          fl_engine_send_pointer_pan_zoom_event(self->engine, timestamp, x, y,
                                                phase, pan_x, pan_y, scale,
                                                rotation);
        };
      };
}

static void fl_view_text_input_delegate_iface_init(
    FlTextInputViewDelegateInterface* iface) {
  iface->translate_coordinates = [](FlTextInputViewDelegate* delegate,
                                    gint view_x, gint view_y, gint* window_x,
                                    gint* window_y) {
    //FlView* self = FL_VIEW(delegate);
    //gtk_widget_translate_coordinates(GTK_WIDGET(self),
    //                                 gtk_widget_get_toplevel(GTK_WIDGET(self)),
    //                                 view_x, view_y, window_x, window_y);
  };
}

static GdkGLContext *create_context_cb(FlView *self) {
   g_printerr("CREATE CONTEXT\n");
   return gdk_surface_create_gl_context (gtk_native_get_surface (gtk_widget_get_native (GTK_WIDGET(self->gl_area))), NULL);
}

static void realize_cb(FlView *self) {
g_printerr("REALIZE\n");
   
  gtk_gl_area_make_current (self->gl_area);

  GError *gl_error = gtk_gl_area_get_error (self->gl_area);
  if (gl_error != NULL) {
     g_warning("Failed to initialize GLArea: %s", gl_error->message);
     return;
  }

  // Handle requests by the user to close the application.
  //GtkWidget* toplevel_window = gtk_widget_get_toplevel(GTK_WIDGET(self));

  // Listen to window state changes.
  // FIXME: How to do in GTK4

  //g_signal_connect(toplevel_window, "delete-event",
  //                 G_CALLBACK(window_delete_event_cb), self);

   // FIXME: gtk_gl_area_set_error

g_printerr("START RENDERER\n");
  g_autoptr(GError) error = nullptr;
  if (!fl_renderer_start(self->renderer, self, &error)) {
    g_warning("Failed to start Flutter renderer: %s", error->message);
    return;
  }

  const char*(*egl_glGetString)(GLenum name) = reinterpret_cast<const char*(*)(GLenum name)>(eglGetProcAddress("glGetString"));
  g_printerr("%p %p\n", glGetString, egl_glGetString);
  g_printerr("GL_VERSION='%s' '%s'\n", glGetString(GL_VERSION), egl_glGetString(GL_VERSION));

g_printerr("START ENGINE\n");
  if (!fl_engine_start(self->engine, &error)) {
    g_warning("Failed to start Flutter engine: %s", error->message);
    return;
  }

  egl_glGetString = reinterpret_cast<const char*(*)(GLenum name)>(eglGetProcAddress("glGetString"));
  g_printerr("%p %p\n", glGetString, egl_glGetString);
  g_printerr("GL_VERSION='%s' '%s'\n", glGetString(GL_VERSION), egl_glGetString(GL_VERSION));

   g_printerr("REALIZE DONE\n");
}

static void render_cb(FlView *self) {
g_printerr("RENDER\n");
}

static void fl_view_constructed(GObject* object) {
  FlView* self = FL_VIEW(object);

  self->renderer = FL_RENDERER(fl_renderer_gl_new());
  self->engine = fl_engine_new(self->project, self->renderer);
  fl_engine_set_update_semantics_node_handler(
      self->engine, update_semantics_node_cb, self, nullptr);
  fl_engine_set_on_pre_engine_restart_handler(
      self->engine, on_pre_engine_restart_cb, self, nullptr);

  // Create system channel handlers.
  FlBinaryMessenger* messenger = fl_engine_get_binary_messenger(self->engine);
  //self->accessibility_plugin = fl_accessibility_plugin_new(self);
  self->mouse_cursor_plugin = fl_mouse_cursor_plugin_new(messenger, self);
  self->platform_plugin = fl_platform_plugin_new(messenger);
}

static void fl_view_set_property(GObject* object,
                                 guint prop_id,
                                 const GValue* value,
                                 GParamSpec* pspec) {
  FlView* self = FL_VIEW(object);

  switch (prop_id) {
    case kPropFlutterProject:
      g_set_object(&self->project,
                   static_cast<FlDartProject*>(g_value_get_object(value)));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void fl_view_get_property(GObject* object,
                                 guint prop_id,
                                 GValue* value,
                                 GParamSpec* pspec) {
  FlView* self = FL_VIEW(object);

  switch (prop_id) {
    case kPropFlutterProject:
      g_value_set_object(value, self->project);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void fl_view_dispose(GObject* object) {
  FlView* self = FL_VIEW(object);

  g_clear_object(&self->project);
  g_clear_object(&self->renderer);
  g_clear_object(&self->engine);
  //g_clear_object(&self->accessibility_plugin);
  g_clear_object(&self->mouse_cursor_plugin);
  g_clear_object(&self->platform_plugin);

  G_OBJECT_CLASS(fl_view_parent_class)->dispose(object);
}

static void fl_view_class_init(FlViewClass* klass) {
  GObjectClass* object_class = G_OBJECT_CLASS(klass);
  object_class->constructed = fl_view_constructed;
  object_class->set_property = fl_view_set_property;
  object_class->get_property = fl_view_get_property;
  //object_class->notify = fl_view_notify;
  object_class->dispose = fl_view_dispose;

  g_object_class_install_property(
      G_OBJECT_CLASS(klass), kPropFlutterProject,
      g_param_spec_object(
          "flutter-project", "flutter-project", "Flutter project in use",
          fl_dart_project_get_type(),
          static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                                   G_PARAM_STATIC_STRINGS)));
}

static void fl_view_init(FlView* self) {
  gtk_widget_set_can_focus(GTK_WIDGET(self), TRUE);

  self->gl_area = GTK_GL_AREA(gtk_gl_area_new());
  g_signal_connect_swapped(self->gl_area, "create-context", G_CALLBACK(create_context_cb), self);
  g_signal_connect_swapped(self->gl_area, "realize", G_CALLBACK(realize_cb), self);
  g_signal_connect_swapped(self->gl_area, "render", G_CALLBACK(render_cb), self);   
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->gl_area));
}

G_MODULE_EXPORT FlView* fl_view_new(FlDartProject* project) {
  return static_cast<FlView*>(
      g_object_new(fl_view_get_type(), "flutter-project", project, nullptr));
}

G_MODULE_EXPORT FlEngine* fl_view_get_engine(FlView* self) {
  g_return_val_if_fail(FL_IS_VIEW(self), nullptr);
  return self->engine;
}

void fl_view_set_textures(FlView* self,
                          GdkGLContext* context,
                          GPtrArray* textures) {
  g_return_if_fail(FL_IS_VIEW(self));

  //fl_gl_area_queue_render(self->gl_area, textures);
}

GHashTable* fl_view_get_keyboard_state(FlView* self) {
  g_return_val_if_fail(FL_IS_VIEW(self), nullptr);

  return nullptr;
}
