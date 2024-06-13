// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/public/flutter_linux/fl_view.h"

#include "flutter/shell/platform/linux/fl_view_private.h"

#include <atk/atk.h>
#if !GTK_CHECK_VERSION(4, 0, 0)
#include <gtk/gtk-a11y.h>
#endif

#include <cstring>

#include "flutter/shell/platform/linux/fl_accessible_node.h"
#include "flutter/shell/platform/linux/fl_backing_store_provider.h"
#include "flutter/shell/platform/linux/fl_engine_private.h"
#include "flutter/shell/platform/linux/fl_key_event.h"
#include "flutter/shell/platform/linux/fl_keyboard_manager.h"
#include "flutter/shell/platform/linux/fl_keyboard_view_delegate.h"
#include "flutter/shell/platform/linux/fl_mouse_cursor_plugin.h"
#include "flutter/shell/platform/linux/fl_platform_plugin.h"
#include "flutter/shell/platform/linux/fl_plugin_registrar_private.h"
#include "flutter/shell/platform/linux/fl_renderer_gdk.h"
#if !GTK_CHECK_VERSION(4, 0, 0)
#include "flutter/shell/platform/linux/fl_scrolling_manager.h"
#endif
#include "flutter/shell/platform/linux/fl_scrolling_view_delegate.h"
#if !GTK_CHECK_VERSION(4, 0, 0)
#include "flutter/shell/platform/linux/fl_socket_accessible.h"
#endif
#include "flutter/shell/platform/linux/fl_text_input_plugin.h"
#include "flutter/shell/platform/linux/fl_text_input_view_delegate.h"
#include "flutter/shell/platform/linux/fl_view_accessible.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_engine.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_plugin_registry.h"

#if !GTK_CHECK_VERSION(4, 0, 0)
static constexpr int kMicrosecondsPerMillisecond = 1000;
#endif

struct _FlView {
  GtkBox parent_instance;

  // Project being run.
  FlDartProject* project;

  // Rendering output.
  FlRendererGdk* renderer;

  // Engine running @project.
  FlEngine* engine;

  // Pointer button state recorded for sending status updates.
  int64_t button_state;

#if !GTK_CHECK_VERSION(4, 0, 0)
  // Current state information for the window associated with this view.
  GdkWindowState window_state;
#endif

  // Flutter system channel handlers.
  FlKeyboardManager* keyboard_manager;
#if !GTK_CHECK_VERSION(4, 0, 0)
  FlScrollingManager* scrolling_manager;
#endif
  FlTextInputPlugin* text_input_plugin;
  FlMouseCursorPlugin* mouse_cursor_plugin;
  FlPlatformPlugin* platform_plugin;

#if !GTK_CHECK_VERSION(4, 0, 0)
  GtkWidget* event_box;
#endif
  GtkGLArea* gl_area;

  // Tracks whether mouse pointer is inside the view.
  gboolean pointer_inside;

#if !GTK_CHECK_VERSION(4, 0, 0)
  /* FlKeyboardViewDelegate related properties */
  KeyboardLayoutNotifier keyboard_layout_notifier;
  GdkKeymap* keymap;
  gulong keymap_keys_changed_cb_id;  // Signal connection ID for
                                     // keymap-keys-changed
  gulong window_state_cb_id;  // Signal connection ID for window-state-changed
#endif

#if !GTK_CHECK_VERSION(4, 0, 0)
  // Accessible tree from Flutter, exposed as an AtkPlug.
  FlViewAccessible* view_accessible;
#endif
};

enum { kPropFlutterProject = 1, kPropLast };

static void fl_view_plugin_registry_iface_init(
    FlPluginRegistryInterface* iface);

#if !GTK_CHECK_VERSION(4, 0, 0)
static void fl_view_keyboard_delegate_iface_init(
    FlKeyboardViewDelegateInterface* iface);

static void fl_view_scrolling_delegate_iface_init(
    FlScrollingViewDelegateInterface* iface);

static void fl_view_text_input_delegate_iface_init(
    FlTextInputViewDelegateInterface* iface);
#endif

G_DEFINE_TYPE_WITH_CODE(
    FlView,
    fl_view,
    GTK_TYPE_BOX,
    G_IMPLEMENT_INTERFACE(fl_plugin_registry_get_type(),
                          fl_view_plugin_registry_iface_init)
#if !GTK_CHECK_VERSION(4, 0, 0)
        G_IMPLEMENT_INTERFACE(fl_keyboard_view_delegate_get_type(),
                              fl_view_keyboard_delegate_iface_init)
            G_IMPLEMENT_INTERFACE(fl_scrolling_view_delegate_get_type(),
                                  fl_view_scrolling_delegate_iface_init)
                G_IMPLEMENT_INTERFACE(fl_text_input_view_delegate_get_type(),
                                      fl_view_text_input_delegate_iface_init)
#endif
)

#if !GTK_CHECK_VERSION(4, 0, 0)
// Signal handler for GtkWidget::delete-event
static gboolean window_delete_event_cb(FlView* self) {
  fl_platform_plugin_request_app_exit(self->platform_plugin);
  // Stop the event from propagating.
  return TRUE;
}
#endif

// Initialize keyboard manager.
static void init_keyboard(FlView* self) {
  FlBinaryMessenger* messenger = fl_engine_get_binary_messenger(self->engine);

#if GTK_CHECK_VERSION(4, 0, 0)
  g_autoptr(GtkIMContext) im_context = gtk_im_multicontext_new();
  gtk_im_context_set_client_widget(im_context, GTK_WIDGET(self));
#else
  GdkWindow* window =
      gtk_widget_get_window(gtk_widget_get_toplevel(GTK_WIDGET(self)));
  g_return_if_fail(GDK_IS_WINDOW(window));
  g_autoptr(GtkIMContext) im_context = gtk_im_multicontext_new();
  gtk_im_context_set_client_window(im_context, window);
#endif

  g_clear_object(&self->text_input_plugin);
  self->text_input_plugin = fl_text_input_plugin_new(
      messenger, im_context, FL_TEXT_INPUT_VIEW_DELEGATE(self));
  g_clear_object(&self->keyboard_manager);
  self->keyboard_manager =
      fl_keyboard_manager_new(messenger, FL_KEYBOARD_VIEW_DELEGATE(self));
}

#if !GTK_CHECK_VERSION(4, 0, 0)
static void init_scrolling(FlView* self) {
  g_clear_object(&self->scrolling_manager);
  self->scrolling_manager =
      fl_scrolling_manager_new(FL_SCROLLING_VIEW_DELEGATE(self));
}

static FlutterPointerDeviceKind get_device_kind(GdkEvent* event) {
  GdkDevice* device = gdk_event_get_source_device(event);
  GdkInputSource source = gdk_device_get_source(device);
  switch (source) {
    case GDK_SOURCE_PEN:
    case GDK_SOURCE_ERASER:
    case GDK_SOURCE_CURSOR:
    case GDK_SOURCE_TABLET_PAD:
      return kFlutterPointerDeviceKindStylus;
    case GDK_SOURCE_TOUCHSCREEN:
      return kFlutterPointerDeviceKindTouch;
    case GDK_SOURCE_TOUCHPAD:  // trackpad device type is reserved for gestures
    case GDK_SOURCE_TRACKPOINT:
    case GDK_SOURCE_KEYBOARD:
    case GDK_SOURCE_MOUSE:
      return kFlutterPointerDeviceKindMouse;
  }
}

// Converts a GDK button event into a Flutter event and sends it to the engine.
static gboolean send_pointer_button_event(FlView* self, GdkEvent* event) {
  guint event_time = gdk_event_get_time(event);
  GdkEventType event_type = gdk_event_get_event_type(event);
  GdkModifierType event_state = static_cast<GdkModifierType>(0);
  gdk_event_get_state(event, &event_state);
  guint event_button = 0;
  gdk_event_get_button(event, &event_button);
  gdouble event_x = 0.0, event_y = 0.0;
  gdk_event_get_coords(event, &event_x, &event_y);

  int64_t button;
  switch (event_button) {
    case 1:
      button = kFlutterPointerButtonMousePrimary;
      break;
    case 2:
      button = kFlutterPointerButtonMouseMiddle;
      break;
    case 3:
      button = kFlutterPointerButtonMouseSecondary;
      break;
    default:
      return FALSE;
  }
  int old_button_state = self->button_state;
  FlutterPointerPhase phase = kMove;
  if (event_type == GDK_BUTTON_PRESS) {
    // Drop the event if Flutter already thinks the button is down.
    if ((self->button_state & button) != 0) {
      return FALSE;
    }
    self->button_state ^= button;

    phase = old_button_state == 0 ? kDown : kMove;
  } else if (event_type == GDK_BUTTON_RELEASE) {
    // Drop the event if Flutter already thinks the button is up.
    if ((self->button_state & button) == 0) {
      return FALSE;
    }
    self->button_state ^= button;

    phase = self->button_state == 0 ? kUp : kMove;
  }

  if (self->engine == nullptr) {
    return FALSE;
  }

  gint scale_factor = gtk_widget_get_scale_factor(GTK_WIDGET(self));
#if !GTK_CHECK_VERSION(4, 0, 0)
  fl_scrolling_manager_set_last_mouse_position(
      self->scrolling_manager, event_x * scale_factor, event_y * scale_factor);
#endif
  fl_keyboard_manager_sync_modifier_if_needed(self->keyboard_manager,
                                              event_state, event_time);
  fl_engine_send_mouse_pointer_event(
      self->engine, phase, event_time * kMicrosecondsPerMillisecond,
      event_x * scale_factor, event_y * scale_factor,
      get_device_kind((GdkEvent*)event), 0, 0, self->button_state);

  return TRUE;
}

// Generates a mouse pointer event if the pointer appears inside the window.
static void check_pointer_inside(FlView* self, GdkEvent* event) {
  if (!self->pointer_inside) {
    self->pointer_inside = TRUE;

    gdouble x, y;
    if (gdk_event_get_coords(event, &x, &y)) {
      gint scale_factor = gtk_widget_get_scale_factor(GTK_WIDGET(self));

      fl_engine_send_mouse_pointer_event(
          self->engine, kAdd,
          gdk_event_get_time(event) * kMicrosecondsPerMillisecond,
          x * scale_factor, y * scale_factor, get_device_kind(event), 0, 0,
          self->button_state);
    }
  }
}

// Updates the engine with the current window metrics.
static void handle_geometry_changed(FlView* self) {
  GtkAllocation allocation;
  gtk_widget_get_allocation(GTK_WIDGET(self), &allocation);
  gint scale_factor = gtk_widget_get_scale_factor(GTK_WIDGET(self));
  fl_engine_send_window_metrics_event(
      self->engine, allocation.width * scale_factor,
      allocation.height * scale_factor, scale_factor);

  // Make sure the view has been realized and its size has been allocated before
  // waiting for a frame. `fl_view_realize()` and `fl_view_size_allocate()` may
  // be called in either order depending on the order in which the window is
  // shown and the view is added to a container in the app runner.
  //
  // Note: `gtk_widget_init()` initializes the size allocation to 1x1.
  if (allocation.width > 1 && allocation.height > 1 &&
      gtk_widget_get_realized(GTK_WIDGET(self))) {
    fl_renderer_wait_for_frame(FL_RENDERER(self->renderer),
                               allocation.width * scale_factor,
                               allocation.height * scale_factor);
  }
}

// Called when the engine updates accessibility.
static void update_semantics_cb(FlEngine* engine,
                                const FlutterSemanticsUpdate2* update,
                                gpointer user_data) {
  FlView* self = FL_VIEW(user_data);

  fl_view_accessible_handle_update_semantics(self->view_accessible, update);
}
#endif

// Invoked by the engine right before the engine is restarted.
//
// This method should reset states to be as if the engine had just been started,
// which usually indicates the user has requested a hot restart (Shift-R in the
// Flutter CLI.)
static void on_pre_engine_restart_cb(FlEngine* engine, gpointer user_data) {
  FlView* self = FL_VIEW(user_data);

  init_keyboard(self);
#if !GTK_CHECK_VERSION(4, 0, 0)
  init_scrolling(self);
#endif
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

#if !GTK_CHECK_VERSION(4, 0, 0)
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
    GdkEventType event_type = gdk_event_get_event_type(event->origin);
    g_return_if_fail(event_type == GDK_KEY_PRESS ||
                     event_type == GDK_KEY_RELEASE);
    gdk_event_put(event->origin);
    fl_key_event_dispose(event);
  };

  iface->subscribe_to_layout_change = [](FlKeyboardViewDelegate* view_delegate,
                                         KeyboardLayoutNotifier notifier) {
    FlView* self = FL_VIEW(view_delegate);
    self->keyboard_layout_notifier = std::move(notifier);
  };

  iface->lookup_key = [](FlKeyboardViewDelegate* view_delegate,
                         const GdkKeymapKey* key) -> guint {
    FlView* self = FL_VIEW(view_delegate);
    g_return_val_if_fail(self->keymap != nullptr, 0);
    return gdk_keymap_lookup_key(self->keymap, key);
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
         size_t timestamp, double x, double y,
         FlutterPointerDeviceKind device_kind, double scroll_delta_x,
         double scroll_delta_y, int64_t buttons) {
        FlView* self = FL_VIEW(view_delegate);
        if (self->engine != nullptr) {
          fl_engine_send_mouse_pointer_event(self->engine, phase, timestamp, x,
                                             y, device_kind, scroll_delta_x,
                                             scroll_delta_y, buttons);
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
    FlView* self = FL_VIEW(delegate);
    gtk_widget_translate_coordinates(GTK_WIDGET(self),
                                     gtk_widget_get_toplevel(GTK_WIDGET(self)),
                                     view_x, view_y, window_x, window_y);
  };
}

// Signal handler for GtkWidget::button-press-event
static gboolean button_press_event_cb(FlView* self,
                                      GdkEventButton* button_event) {
  GdkEvent* event = reinterpret_cast<GdkEvent*>(button_event);

  // Flutter doesn't handle double and triple click events.
  GdkEventType event_type = gdk_event_get_event_type(event);
  if (event_type == GDK_DOUBLE_BUTTON_PRESS ||
      event_type == GDK_TRIPLE_BUTTON_PRESS) {
    return FALSE;
  }

  check_pointer_inside(self, event);

  return send_pointer_button_event(self, event);
}

// Signal handler for GtkWidget::button-release-event
static gboolean button_release_event_cb(FlView* self,
                                        GdkEventButton* button_event) {
  GdkEvent* event = reinterpret_cast<GdkEvent*>(button_event);
  return send_pointer_button_event(self, event);
}

// Signal handler for GtkWidget::scroll-event
static gboolean scroll_event_cb(FlView* self, GdkEventScroll* event) {
  // TODO(robert-ancell): Update to use GtkEventControllerScroll when we can
  // depend on GTK 3.24.

  fl_scrolling_manager_handle_scroll_event(
      self->scrolling_manager, event,
      gtk_widget_get_scale_factor(GTK_WIDGET(self)));
  return TRUE;
}

// Signal handler for GtkWidget::motion-notify-event
static gboolean motion_notify_event_cb(FlView* self,
                                       GdkEventMotion* motion_event) {
  GdkEvent* event = reinterpret_cast<GdkEvent*>(motion_event);

  if (self->engine == nullptr) {
    return FALSE;
  }

  guint event_time = gdk_event_get_time(event);
  GdkModifierType event_state = static_cast<GdkModifierType>(0);
  gdk_event_get_state(event, &event_state);
  gdouble event_x = 0.0, event_y = 0.0;
  gdk_event_get_coords(event, &event_x, &event_y);

  check_pointer_inside(self, event);

  gint scale_factor = gtk_widget_get_scale_factor(GTK_WIDGET(self));

  fl_keyboard_manager_sync_modifier_if_needed(self->keyboard_manager,
                                              event_state, event_time);
  fl_engine_send_mouse_pointer_event(
      self->engine, self->button_state != 0 ? kMove : kHover,
      event_time * kMicrosecondsPerMillisecond, event_x * scale_factor,
      event_y * scale_factor, get_device_kind((GdkEvent*)event), 0, 0,
      self->button_state);

  return TRUE;
}

// Signal handler for GtkWidget::enter-notify-event
static gboolean enter_notify_event_cb(FlView* self,
                                      GdkEventCrossing* crossing_event) {
  GdkEvent* event = reinterpret_cast<GdkEvent*>(crossing_event);

  if (self->engine == nullptr) {
    return FALSE;
  }

  check_pointer_inside(self, event);

  return TRUE;
}

// Signal handler for GtkWidget::leave-notify-event
static gboolean leave_notify_event_cb(FlView* self,
                                      GdkEventCrossing* crossing_event) {
  GdkEvent* event = reinterpret_cast<GdkEvent*>(crossing_event);

  guint event_time = gdk_event_get_time(event);
  gdouble event_x = 0.0, event_y = 0.0;
  gdk_event_get_coords(event, &event_x, &event_y);

  if (crossing_event->mode != GDK_CROSSING_NORMAL) {
    return FALSE;
  }

  if (self->engine == nullptr) {
    return FALSE;
  }

  // Don't remove pointer while button is down; In case of dragging outside of
  // window with mouse grab active Gtk will send another leave notify on
  // release.
  if (self->pointer_inside && self->button_state == 0) {
    gint scale_factor = gtk_widget_get_scale_factor(GTK_WIDGET(self));
    fl_engine_send_mouse_pointer_event(
        self->engine, kRemove, event_time * kMicrosecondsPerMillisecond,
        event_x * scale_factor, event_y * scale_factor,
        get_device_kind((GdkEvent*)event), 0, 0, self->button_state);
    self->pointer_inside = FALSE;
  }

  return TRUE;
}

static void keymap_keys_changed_cb(FlView* self) {
  if (self->keyboard_layout_notifier == nullptr) {
    return;
  }

  self->keyboard_layout_notifier();
}

static void gesture_rotation_begin_cb(FlView* self) {
  fl_scrolling_manager_handle_rotation_begin(self->scrolling_manager);
}

static void gesture_rotation_update_cb(FlView* self,
                                       gdouble rotation,
                                       gdouble delta) {
  fl_scrolling_manager_handle_rotation_update(self->scrolling_manager,
                                              rotation);
}

static void gesture_rotation_end_cb(FlView* self) {
  fl_scrolling_manager_handle_rotation_end(self->scrolling_manager);
}

static void gesture_zoom_begin_cb(FlView* self) {
  fl_scrolling_manager_handle_zoom_begin(self->scrolling_manager);
}

static void gesture_zoom_update_cb(FlView* self, gdouble scale) {
  fl_scrolling_manager_handle_zoom_update(self->scrolling_manager, scale);
}

static void gesture_zoom_end_cb(FlView* self) {
  fl_scrolling_manager_handle_zoom_end(self->scrolling_manager);
}

static gboolean window_state_event_cb(FlView* self, GdkEvent* event) {
  g_return_val_if_fail(FL_IS_ENGINE(self->engine), FALSE);

  GdkWindowState state = event->window_state.new_window_state;
  GdkWindowState previous_state = self->window_state;
  self->window_state = state;
  bool was_visible = !((previous_state & GDK_WINDOW_STATE_WITHDRAWN) ||
                       (previous_state & GDK_WINDOW_STATE_ICONIFIED));
  bool is_visible = !((state & GDK_WINDOW_STATE_WITHDRAWN) ||
                      (state & GDK_WINDOW_STATE_ICONIFIED));
  bool was_focused = (previous_state & GDK_WINDOW_STATE_FOCUSED);
  bool is_focused = (state & GDK_WINDOW_STATE_FOCUSED);
  if (was_visible != is_visible || was_focused != is_focused) {
    if (self->engine != nullptr) {
      fl_engine_send_window_state_event(FL_ENGINE(self->engine), is_visible,
                                        is_focused);
    }
  }
  return FALSE;
}
#endif

static GdkGLContext* create_context_cb(FlView* self) {
#if GTK_CHECK_VERSION(4, 0, 0)
  self->renderer = fl_renderer_gdk_new(
      gtk_native_get_surface(gtk_widget_get_native(GTK_WIDGET(self))));
#else
  self->renderer =
      fl_renderer_gdk_new(gtk_widget_get_parent_window(GTK_WIDGET(self)));
#endif
  self->engine = fl_engine_new(self->project, FL_RENDERER(self->renderer));
#if !GTK_CHECK_VERSION(4, 0, 0)
  fl_engine_set_update_semantics_handler(self->engine, update_semantics_cb,
                                         self, nullptr);
#endif
  fl_engine_set_on_pre_engine_restart_handler(
      self->engine, on_pre_engine_restart_cb, self, nullptr);

#if !GTK_CHECK_VERSION(4, 0, 0)
  // Must initialize the keymap before the keyboard.
  self->keymap = gdk_keymap_get_for_display(gdk_display_get_default());
  self->keymap_keys_changed_cb_id = g_signal_connect_swapped(
      self->keymap, "keys-changed", G_CALLBACK(keymap_keys_changed_cb), self);
#endif

  // Create system channel handlers.
  FlBinaryMessenger* messenger = fl_engine_get_binary_messenger(self->engine);
#if !GTK_CHECK_VERSION(4, 0, 0)
  init_scrolling(self);
#endif
  self->mouse_cursor_plugin = fl_mouse_cursor_plugin_new(messenger, self);
  self->platform_plugin = fl_platform_plugin_new(messenger);

  g_autoptr(GError) error = nullptr;
  if (!fl_renderer_gdk_create_contexts(self->renderer, &error)) {
    gtk_gl_area_set_error(self->gl_area, error);
    return nullptr;
  }

  return GDK_GL_CONTEXT(
      g_object_ref(fl_renderer_gdk_get_context(self->renderer)));
}

static void realize_cb(FlView* self) {
  g_autoptr(GError) error = nullptr;

  fl_renderer_make_current(FL_RENDERER(self->renderer));

  GError* gl_error = gtk_gl_area_get_error(self->gl_area);
  if (gl_error != NULL) {
    g_warning("Failed to initialize GLArea: %s", gl_error->message);
    return;
  }

  fl_renderer_setup(FL_RENDERER(self->renderer));

#if !GTK_CHECK_VERSION(4, 0, 0)
  // Handle requests by the user to close the application.
  GtkWidget* toplevel_window = gtk_widget_get_toplevel(GTK_WIDGET(self));

  // Listen to window state changes.
  self->window_state_cb_id =
      g_signal_connect_swapped(toplevel_window, "window-state-event",
                               G_CALLBACK(window_state_event_cb), self);
  self->window_state =
      gdk_window_get_state(gtk_widget_get_window(toplevel_window));

  g_signal_connect_swapped(toplevel_window, "delete-event",
                           G_CALLBACK(window_delete_event_cb), self);
#endif

  init_keyboard(self);

  fl_renderer_start(FL_RENDERER(FL_RENDERER(self->renderer)), self);

  if (!fl_engine_start(self->engine, &error)) {
    g_warning("Failed to start Flutter engine: %s", error->message);
    return;
  }

#if !GTK_CHECK_VERSION(4, 0, 0)
  handle_geometry_changed(self);
#endif

#if !GTK_CHECK_VERSION(4, 0, 0)
  self->view_accessible = fl_view_accessible_new(self->engine);
  fl_socket_accessible_embed(
      FL_SOCKET_ACCESSIBLE(gtk_widget_get_accessible(GTK_WIDGET(self))),
      atk_plug_get_id(ATK_PLUG(self->view_accessible)));
#endif
}

static gboolean render_cb(FlView* self, GdkGLContext* context) {
  if (gtk_gl_area_get_error(self->gl_area) != NULL) {
    return FALSE;
  }

#if GTK_CHECK_VERSION(4, 0, 0)
  int width = gtk_widget_get_width(GTK_WIDGET(self->gl_area));
  int height = gtk_widget_get_height(GTK_WIDGET(self->gl_area));
#else
  int width = gtk_widget_get_allocated_width(GTK_WIDGET(self->gl_area));
  int height = gtk_widget_get_allocated_height(GTK_WIDGET(self->gl_area));
#endif
  gint scale_factor = gtk_widget_get_scale_factor(GTK_WIDGET(self->gl_area));
  fl_renderer_render(FL_RENDERER(self->renderer), width * scale_factor,
                     height * scale_factor);

  return TRUE;
}

static void unrealize_cb(FlView* self) {
  g_autoptr(GError) error = nullptr;

  fl_renderer_make_current(FL_RENDERER(self->renderer));

  GError* gl_error = gtk_gl_area_get_error(self->gl_area);
  if (gl_error != NULL) {
    g_warning("Failed to uninitialize GLArea: %s", gl_error->message);
    return;
  }

  fl_renderer_cleanup(FL_RENDERER(self->renderer));
}

static void size_allocate_cb(FlView* self) {
#if !GTK_CHECK_VERSION(4, 0, 0)
  handle_geometry_changed(self);
#endif
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

#if !GTK_CHECK_VERSION(4, 0, 0)
static void fl_view_notify(GObject* object, GParamSpec* pspec) {
  FlView* self = FL_VIEW(object);

  if (strcmp(pspec->name, "scale-factor") == 0) {
    handle_geometry_changed(self);
  }

  if (G_OBJECT_CLASS(fl_view_parent_class)->notify != nullptr) {
    G_OBJECT_CLASS(fl_view_parent_class)->notify(object, pspec);
  }
}
#endif

static void fl_view_dispose(GObject* object) {
  FlView* self = FL_VIEW(object);

  if (self->engine != nullptr) {
#if !GTK_CHECK_VERSION(4, 0, 0)
    fl_engine_set_update_semantics_handler(self->engine, nullptr, nullptr,
                                           nullptr);
#endif
    fl_engine_set_on_pre_engine_restart_handler(self->engine, nullptr, nullptr,
                                                nullptr);
  }

#if !GTK_CHECK_VERSION(4, 0, 0)
  if (self->window_state_cb_id != 0) {
    GtkWidget* toplevel_window = gtk_widget_get_toplevel(GTK_WIDGET(self));
    g_signal_handler_disconnect(toplevel_window, self->window_state_cb_id);
    self->window_state_cb_id = 0;
  }
#endif

  g_clear_object(&self->project);
  g_clear_object(&self->renderer);
  g_clear_object(&self->engine);
#if !GTK_CHECK_VERSION(4, 0, 0)
  g_clear_object(&self->keyboard_manager);
  if (self->keymap_keys_changed_cb_id != 0) {
    g_signal_handler_disconnect(self->keymap, self->keymap_keys_changed_cb_id);
    self->keymap_keys_changed_cb_id = 0;
  }
#endif
  g_clear_object(&self->mouse_cursor_plugin);
  g_clear_object(&self->platform_plugin);
#if !GTK_CHECK_VERSION(4, 0, 0)
  g_clear_object(&self->view_accessible);
#endif

  G_OBJECT_CLASS(fl_view_parent_class)->dispose(object);
}

#if !GTK_CHECK_VERSION(4, 0, 0)
// Implements GtkWidget::key_press_event.
static gboolean fl_view_key_press_event(GtkWidget* widget, GdkEventKey* event) {
  FlView* self = FL_VIEW(widget);

  return fl_keyboard_manager_handle_event(
      self->keyboard_manager, fl_key_event_new_from_gdk_event(gdk_event_copy(
                                  reinterpret_cast<GdkEvent*>(event))));
}

// Implements GtkWidget::key_release_event.
static gboolean fl_view_key_release_event(GtkWidget* widget,
                                          GdkEventKey* event) {
  FlView* self = FL_VIEW(widget);
  return fl_keyboard_manager_handle_event(
      self->keyboard_manager, fl_key_event_new_from_gdk_event(gdk_event_copy(
                                  reinterpret_cast<GdkEvent*>(event))));
}
#endif

static void fl_view_class_init(FlViewClass* klass) {
  GObjectClass* object_class = G_OBJECT_CLASS(klass);
  object_class->set_property = fl_view_set_property;
  object_class->get_property = fl_view_get_property;
#if !GTK_CHECK_VERSION(4, 0, 0)
  object_class->notify = fl_view_notify;
#endif
  object_class->dispose = fl_view_dispose;

#if !GTK_CHECK_VERSION(4, 0, 0)
  GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);
  widget_class->key_press_event = fl_view_key_press_event;
  widget_class->key_release_event = fl_view_key_release_event;
#endif

  g_object_class_install_property(
      G_OBJECT_CLASS(klass), kPropFlutterProject,
      g_param_spec_object(
          "flutter-project", "flutter-project", "Flutter project in use",
          fl_dart_project_get_type(),
          static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                                   G_PARAM_STATIC_STRINGS)));

#if !GTK_CHECK_VERSION(4, 0, 0)
  gtk_widget_class_set_accessible_type(GTK_WIDGET_CLASS(klass),
                                       fl_socket_accessible_get_type());
#endif
}

static void fl_view_init(FlView* self) {
  gtk_widget_set_can_focus(GTK_WIDGET(self), TRUE);

#if !GTK_CHECK_VERSION(4, 0, 0)
  self->event_box = gtk_event_box_new();
  gtk_widget_set_hexpand(self->event_box, TRUE);
  gtk_widget_set_vexpand(self->event_box, TRUE);
  gtk_container_add(GTK_CONTAINER(self), self->event_box);
  gtk_widget_show(self->event_box);
  gtk_widget_add_events(self->event_box,
                        GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK |
                            GDK_BUTTON_RELEASE_MASK | GDK_SCROLL_MASK |
                            GDK_SMOOTH_SCROLL_MASK);

  g_signal_connect_swapped(self->event_box, "button-press-event",
                           G_CALLBACK(button_press_event_cb), self);
  g_signal_connect_swapped(self->event_box, "button-release-event",
                           G_CALLBACK(button_release_event_cb), self);
  g_signal_connect_swapped(self->event_box, "scroll-event",
                           G_CALLBACK(scroll_event_cb), self);
  g_signal_connect_swapped(self->event_box, "motion-notify-event",
                           G_CALLBACK(motion_notify_event_cb), self);
  g_signal_connect_swapped(self->event_box, "enter-notify-event",
                           G_CALLBACK(enter_notify_event_cb), self);
  g_signal_connect_swapped(self->event_box, "leave-notify-event",
                           G_CALLBACK(leave_notify_event_cb), self);
  GtkGesture* zoom = gtk_gesture_zoom_new(self->event_box);
  g_signal_connect_swapped(zoom, "begin", G_CALLBACK(gesture_zoom_begin_cb),
                           self);
  g_signal_connect_swapped(zoom, "scale-changed",
                           G_CALLBACK(gesture_zoom_update_cb), self);
  g_signal_connect_swapped(zoom, "end", G_CALLBACK(gesture_zoom_end_cb), self);
  GtkGesture* rotate = gtk_gesture_rotate_new(self->event_box);
  g_signal_connect_swapped(rotate, "begin",
                           G_CALLBACK(gesture_rotation_begin_cb), self);
  g_signal_connect_swapped(rotate, "angle-changed",
                           G_CALLBACK(gesture_rotation_update_cb), self);
  g_signal_connect_swapped(rotate, "end", G_CALLBACK(gesture_rotation_end_cb),
                           self);
#endif

  self->gl_area = GTK_GL_AREA(gtk_gl_area_new());
#if GTK_CHECK_VERSION(4, 0, 0)
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->gl_area));
#else
  gtk_widget_show(GTK_WIDGET(self->gl_area));
  gtk_container_add(GTK_CONTAINER(self->event_box), GTK_WIDGET(self->gl_area));
#endif

  g_signal_connect_swapped(self->gl_area, "create-context",
                           G_CALLBACK(create_context_cb), self);
  g_signal_connect_swapped(self->gl_area, "realize", G_CALLBACK(realize_cb),
                           self);
  g_signal_connect_swapped(self->gl_area, "render", G_CALLBACK(render_cb),
                           self);
  g_signal_connect_swapped(self->gl_area, "unrealize", G_CALLBACK(unrealize_cb),
                           self);

  g_signal_connect_swapped(self, "size-allocate", G_CALLBACK(size_allocate_cb),
                           self);
}

G_MODULE_EXPORT FlView* fl_view_new(FlDartProject* project) {
  return static_cast<FlView*>(
      g_object_new(fl_view_get_type(), "flutter-project", project, nullptr));
}

G_MODULE_EXPORT FlEngine* fl_view_get_engine(FlView* self) {
  g_return_val_if_fail(FL_IS_VIEW(self), nullptr);
  return self->engine;
}

void fl_view_redraw(FlView* self) {
  g_return_if_fail(FL_IS_VIEW(self));
  gtk_widget_queue_draw(GTK_WIDGET(self->gl_area));
}

GHashTable* fl_view_get_keyboard_state(FlView* self) {
  g_return_val_if_fail(FL_IS_VIEW(self), nullptr);
  return fl_keyboard_manager_get_pressed_state(self->keyboard_manager);
}
