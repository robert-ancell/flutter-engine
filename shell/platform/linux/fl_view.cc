// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/public/flutter_linux/fl_view.h"

#include "flutter/shell/platform/linux/fl_view_private.h"

#include <cstring>

#include "flutter/shell/platform/linux/fl_backing_store_provider.h"
#include "flutter/shell/platform/linux/fl_engine_private.h"
#include "flutter/shell/platform/linux/fl_key_event.h"
#include "flutter/shell/platform/linux/fl_keyboard_manager.h"
#include "flutter/shell/platform/linux/fl_keyboard_view_delegate.h"
#include "flutter/shell/platform/linux/fl_mouse_cursor_plugin.h"
#include "flutter/shell/platform/linux/fl_platform_plugin.h"
#include "flutter/shell/platform/linux/fl_plugin_registrar_private.h"
#include "flutter/shell/platform/linux/fl_renderer_gl.h"
#include "flutter/shell/platform/linux/fl_scrolling_manager.h"
#include "flutter/shell/platform/linux/fl_scrolling_view_delegate.h"
#include "flutter/shell/platform/linux/fl_text_input_plugin.h"
#include "flutter/shell/platform/linux/fl_text_input_view_delegate.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_engine.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_plugin_registry.h"

#include <epoxy/gl.h>

static constexpr int kMicrosecondsPerMillisecond = 1000;

struct _FlView {
  GtkGLArea parent_instance;

  // Project being run.
  FlDartProject* project;

  // Rendering output.
  FlRenderer* renderer;

  // Engine running @project.
  FlEngine* engine;

  // Pointer button state recorded for sending status updates.
  int64_t button_state;

  // Current state information for the window associated with this view.
  // GdkWindowState window_state;

  // Flutter system channel handlers.
  // FlAccessibilityPlugin* accessibility_plugin;
  FlKeyboardManager* keyboard_manager;
  FlScrollingManager* scrolling_manager;
  FlTextInputPlugin* text_input_plugin;
  FlMouseCursorPlugin* mouse_cursor_plugin;
  FlPlatformPlugin* platform_plugin;

  GtkGesture* click_gesture;
  GtkEventController* motion_controller;
  GtkEventController* key_controller;

  GLuint program;

  GPtrArray* textures;

  // Tracks whether mouse pointer is inside the view.
  gboolean pointer_inside;

  /* FlKeyboardViewDelegate related properties */
  KeyboardLayoutNotifier keyboard_layout_notifier;
  // GdkKeymap* keymap;
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
    GTK_TYPE_GL_AREA,
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

// Initialize keyboard manager.
static void init_keyboard(FlView* self) {
  FlBinaryMessenger* messenger = fl_engine_get_binary_messenger(self->engine);

  // GdkWindow* window =
  //     gtk_widget_get_window(gtk_widget_get_toplevel(GTK_WIDGET(self)));
  // g_return_if_fail(GDK_IS_WINDOW(window));
  // g_autoptr(GtkIMContext) im_context = gtk_im_multicontext_new();
  // gtk_im_context_set_client_window(im_context, window);

  // self->text_input_plugin = fl_text_input_plugin_new(
  //     messenger, im_context, FL_TEXT_INPUT_VIEW_DELEGATE(self));
  self->keyboard_manager =
      fl_keyboard_manager_new(messenger, FL_KEYBOARD_VIEW_DELEGATE(self));
}

static void init_scrolling(FlView* self) {
  self->scrolling_manager =
      fl_scrolling_manager_new(FL_SCROLLING_VIEW_DELEGATE(self));
}

// Called when the engine updates accessibility nodes.
static void update_semantics_node_cb(FlEngine* engine,
                                     const FlutterSemanticsNode* node,
                                     gpointer user_data) {
  // FlView* self = FL_VIEW(user_data);

  // fl_accessibility_plugin_handle_update_semantics_node(
  //     self->accessibility_plugin, node);
}

// Invoked by the engine right before the engine is restarted.
//
// This method should reset states to be as if the engine had just been started,
// which usually indicates the user has requested a hot restart (Shift-R in the
// Flutter CLI.)
static void on_pre_engine_restart_cb(FlEngine* engine, gpointer user_data) {
  FlView* self = FL_VIEW(user_data);

  g_clear_object(&self->keyboard_manager);
  g_clear_object(&self->text_input_plugin);
  g_clear_object(&self->scrolling_manager);
  init_keyboard(self);
  init_scrolling(self);
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
    // GdkEvent* gdk_event = reinterpret_cast<GdkEvent*>(event->origin);
    // GdkEventType event_type = gdk_event_get_event_type(gdk_event);
    // g_return_if_fail(event_type == GDK_KEY_PRESS ||
    //                  event_type == GDK_KEY_RELEASE);
    //  gdk_event_put(gdk_event);
    fl_key_event_dispose(event);
  };

  iface->subscribe_to_layout_change = [](FlKeyboardViewDelegate* view_delegate,
                                         KeyboardLayoutNotifier notifier) {
    FlView* self = FL_VIEW(view_delegate);
    self->keyboard_layout_notifier = std::move(notifier);
  };

  iface->lookup_key = [](FlKeyboardViewDelegate* view_delegate,
                         const GdkKeymapKey* key) -> guint {
    // FlView* self = FL_VIEW(view_delegate);
    // g_return_val_if_fail(self->keymap != nullptr, 0);
    return 0;  // gdk_keymap_lookup_key(self->keymap, key);
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
    // FlView* self = FL_VIEW(delegate);
    // gtk_widget_translate_coordinates(GTK_WIDGET(self),
    //                                  gtk_widget_get_toplevel(GTK_WIDGET(self)),
    //                                  view_x, view_y, window_x, window_y);
  };
}

static void send_mouse_pointer_event(FlView* self,
                                     FlutterPointerPhase phase,
                                     guint32 timestamp,
                                     double x,
                                     double y) {
  fl_engine_send_mouse_pointer_event(self->engine, phase,
                                     timestamp * kMicrosecondsPerMillisecond, x,
                                     y, 0, 0, self->button_state);
}

static void primary_pressed_cb(FlView* self, int n_press, double x, double y) {
  self->button_state ^= kFlutterPointerButtonMousePrimary;
  send_mouse_pointer_event(self, kDown,
                           gtk_event_controller_get_current_event_time(
                               GTK_EVENT_CONTROLLER(self->click_gesture)),
                           x, y);
}

static void primary_released_cb(FlView* self, int n_press, double x, double y) {
  self->button_state ^= kFlutterPointerButtonMousePrimary;
  send_mouse_pointer_event(self, kUp,
                           gtk_event_controller_get_current_event_time(
                               GTK_EVENT_CONTROLLER(self->click_gesture)),
                           x, y);
}

static void enter_cb(FlView* self, gdouble x, gdouble y) {
  send_mouse_pointer_event(
      self, kAdd,
      gtk_event_controller_get_current_event_time(self->motion_controller), x,
      y);
}

static void leave_cb(FlView* self) {
  send_mouse_pointer_event(
      self, kRemove,
      gtk_event_controller_get_current_event_time(self->motion_controller), 0,
      0);
}

static void motion_cb(FlView* self, gdouble x, gdouble y) {
  send_mouse_pointer_event(
      self, self->button_state != 0 ? kMove : kHover,
      gtk_event_controller_get_current_event_time(self->motion_controller), x,
      y);
}

static gboolean key_pressed_cb(FlView* self,
                               guint keyval,
                               guint keycode,
                               GdkModifierType state) {
  return fl_keyboard_manager_handle_event(
      self->keyboard_manager,
      fl_key_event_new(
          gtk_event_controller_get_current_event_time(self->key_controller),
          TRUE, keycode, keyval, state,
          gtk_event_controller_key_get_group(
              GTK_EVENT_CONTROLLER_KEY(self->key_controller))));
}

static void key_released_cb(FlView* self,
                            guint keyval,
                            guint keycode,
                            GdkModifierType state) {
  fl_keyboard_manager_handle_event(
      self->keyboard_manager,
      fl_key_event_new(
          gtk_event_controller_get_current_event_time(self->key_controller),
          FALSE, keycode, keyval, state,
          gtk_event_controller_key_get_group(
              GTK_EVENT_CONTROLLER_KEY(self->key_controller))));
}

static const char* vertex_shader_src =
    "attribute vec2 position;\n"
    "attribute vec2 in_texcoord;\n"
    "varying vec2 texcoord;\n"
    "\n"
    "void main() {\n"
    "  gl_Position = vec4(position, 0, 1);\n"
    "  texcoord = in_texcoord;\n"
    "}\n";

static const char* fragment_shader_src =
    "uniform sampler2D texture;\n"
    "varying vec2 texcoord;\n"
    "\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(texture, texcoord);\n"
    "}\n";

static gchar* get_shader_log(GLuint shader) {
  int log_length;
  gchar* log;

  glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);

  log = static_cast<gchar*>(g_malloc(log_length + 1));
  glGetShaderInfoLog(shader, log_length, NULL, log);

  return log;
}

static gchar* get_program_log(GLuint program) {
  int log_length;
  gchar* log;

  glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);

  log = static_cast<gchar*>(g_malloc(log_length + 1));
  glGetProgramInfoLog(program, log_length, NULL, log);

  return log;
}

static void realize_cb(FlView* self) {
  gtk_gl_area_make_current(GTK_GL_AREA(self));

  GError* gl_error = gtk_gl_area_get_error(GTK_GL_AREA(self));
  if (gl_error != NULL) {
    g_warning("Failed to initialize GLArea: %s", gl_error->message);
    return;
  }

  GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vertex_shader, 1, &vertex_shader_src, NULL);
  glCompileShader(vertex_shader);
  int vertex_compile_status;
  glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &vertex_compile_status);
  if (vertex_compile_status == GL_FALSE) {
    g_autofree gchar* shader_log = get_shader_log(vertex_shader);
    g_warning("Failed to compile vertex shader: %s", shader_log);
  }

  GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fragment_shader, 1, &fragment_shader_src, NULL);
  glCompileShader(fragment_shader);
  int fragment_compile_status;
  glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &fragment_compile_status);
  if (fragment_compile_status == GL_FALSE) {
    g_autofree gchar* shader_log = get_shader_log(fragment_shader);
    g_warning("Failed to compile fragment shader: %s", shader_log);
  }

  self->program = glCreateProgram();
  glAttachShader(self->program, vertex_shader);
  glAttachShader(self->program, fragment_shader);
  glLinkProgram(self->program);

  int link_status;
  glGetProgramiv(self->program, GL_LINK_STATUS, &link_status);
  if (link_status == GL_FALSE) {
    g_autofree gchar* program_log = get_program_log(self->program);
    g_warning("Failed to link program: %s", program_log);
  }

  glDeleteShader(vertex_shader);
  glDeleteShader(fragment_shader);

  // Handle requests by the user to close the application.
  // GtkWidget* toplevel_window = gtk_widget_get_toplevel(GTK_WIDGET(self));

  // Listen to window state changes.
  // FIXME: How to do in GTK4

  // g_signal_connect(toplevel_window, "delete-event",
  //                  G_CALLBACK(window_delete_event_cb), self);

  // FIXME: gtk_gl_area_set_error

  init_keyboard(self);

  g_autoptr(GError) error = nullptr;
  if (!fl_renderer_start(self->renderer, self, &error)) {
    g_warning("Failed to start Flutter renderer: %s", error->message);
    return;
  }

  if (!fl_engine_start(self->engine, &error)) {
    g_warning("Failed to start Flutter engine: %s", error->message);
    return;
  }
}

static void unrealize_cb(FlView* self) {
  gtk_gl_area_make_current(GTK_GL_AREA(self));

  GError* gl_error = gtk_gl_area_get_error(GTK_GL_AREA(self));
  if (gl_error != NULL) {
    g_warning("Failed to cleanup GLArea: %s", gl_error->message);
    return;
  }

  glDeleteProgram(self->program);
}

static gboolean render_cb(FlView* self, GdkGLContext* context) {
  if (gtk_gl_area_get_error(GTK_GL_AREA(self)) != NULL) {
    return FALSE;
  }

  if (self->textures == nullptr) {
    return TRUE;
  }

  glClearColor(0.5, 0.5, 0.5, 1.0);
  glClear(GL_COLOR_BUFFER_BIT);

  glUseProgram(self->program);

  for (guint i = 0; i < self->textures->len; i++) {
    FlBackingStoreProvider* texture =
        FL_BACKING_STORE_PROVIDER(g_ptr_array_index(self->textures, i));
    uint32_t texture_id = fl_backing_store_provider_get_gl_texture_id(texture);
    // GdkRectangle geometry = fl_backing_store_provider_get_geometry(texture);

    // FIXME: scale?

    // GLfloat x = geometry.x;
    // GLfloat y = geometry.y;
    // GLfloat width = geometry.width;
    // GLfloat height = geometry.height;
    // GLfloat vertices[12] = {x,         y,          x + width, y,
    //                          x + width, y + height, x,         y,
    //                          x + width, y + height, x,         y + height};
    // glVertexPointer(2, GL_FLOAT, sizeof(GLfloat) * 2, vertices);
    // glTexCoordPointer(2, GL_FLOAT, sizeof(GLfloat) * 2, tex_coords);
    // glEnableClientState(GL_VERTEX_ARRAY);
    // glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    static const GLfloat vertex_data[] = {-1, -1, 0, 0, 1,  1,  1, 1,
                                          -1, 1,  0, 1, -1, -1, 0, 0,
                                          1,  -1, 1, 0, 1,  1,  1, 1};

    glBindTexture(GL_TEXTURE_2D, texture_id);

    GLuint vao, vertex_buffer;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vertex_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_data), vertex_data,
                 GL_STATIC_DRAW);
    GLint position_index = glGetAttribLocation(self->program, "position");
    glEnableVertexAttribArray(position_index);
    glVertexAttribPointer(position_index, 2, GL_FLOAT, GL_FALSE,
                          sizeof(GLfloat) * 4, 0);
    GLint texcoord_index = glGetAttribLocation(self->program, "in_texcoord");
    glEnableVertexAttribArray(texcoord_index);
    glVertexAttribPointer(texcoord_index, 2, GL_FLOAT, GL_FALSE,
                          sizeof(GLfloat) * 4, (void*)(sizeof(GLfloat) * 2));

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vertex_buffer);
  }

  glFlush();

  return TRUE;
}

static void resize_cb(FlView* self, int width, int height) {
  gint scale_factor = gtk_widget_get_scale_factor(GTK_WIDGET(self));
  fl_engine_send_window_metrics_event(self->engine, width * scale_factor,
                                      height * scale_factor, scale_factor);
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
  // self->accessibility_plugin = fl_accessibility_plugin_new(self);
  init_scrolling(self);
  self->mouse_cursor_plugin = fl_mouse_cursor_plugin_new(messenger, self);
  self->platform_plugin = fl_platform_plugin_new(messenger);

  self->click_gesture = gtk_gesture_click_new();
  g_signal_connect_swapped(self->click_gesture, "pressed",
                           G_CALLBACK(primary_pressed_cb), self);
  g_signal_connect_swapped(self->click_gesture, "released",
                           G_CALLBACK(primary_released_cb), self);
  gtk_widget_add_controller(GTK_WIDGET(self),
                            GTK_EVENT_CONTROLLER(self->click_gesture));

  self->motion_controller = gtk_event_controller_motion_new();
  g_signal_connect_swapped(self->motion_controller, "enter",
                           G_CALLBACK(enter_cb), self);
  g_signal_connect_swapped(self->motion_controller, "leave",
                           G_CALLBACK(leave_cb), self);
  g_signal_connect_swapped(self->motion_controller, "motion",
                           G_CALLBACK(motion_cb), self);
  gtk_widget_add_controller(GTK_WIDGET(self), self->motion_controller);

  self->key_controller = gtk_event_controller_key_new();
  g_signal_connect_swapped(self->key_controller, "key-pressed",
                           G_CALLBACK(key_pressed_cb), self);
  g_signal_connect_swapped(self->key_controller, "key-released",
                           G_CALLBACK(key_released_cb), self);
  gtk_widget_add_controller(GTK_WIDGET(self), self->key_controller);
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
  // g_clear_object(&self->accessibility_plugin);
  g_clear_object(&self->mouse_cursor_plugin);
  g_clear_object(&self->platform_plugin);
  // g_clear_object(&self->click_gesture); // FIXME: Required?
  // g_clear_object(&self->motion_controller);
  // g_clear_object(&self->key_controller);
  g_clear_pointer(&self->textures, g_ptr_array_unref);

  G_OBJECT_CLASS(fl_view_parent_class)->dispose(object);
}

static void fl_view_class_init(FlViewClass* klass) {
  GObjectClass* object_class = G_OBJECT_CLASS(klass);
  object_class->constructed = fl_view_constructed;
  object_class->set_property = fl_view_set_property;
  object_class->get_property = fl_view_get_property;
  // object_class->notify = fl_view_notify;
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

  // FIXME: Don't need signals for these
  g_signal_connect_swapped(GTK_GL_AREA(self), "realize", G_CALLBACK(realize_cb),
                           self);
  g_signal_connect_swapped(GTK_GL_AREA(self), "unrealize",
                           G_CALLBACK(unrealize_cb), self);
  g_signal_connect_swapped(GTK_GL_AREA(self), "render", G_CALLBACK(render_cb),
                           self);
  g_signal_connect_swapped(GTK_GL_AREA(self), "resize", G_CALLBACK(resize_cb),
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

void fl_view_set_textures(FlView* self,
                          GdkGLContext* context,
                          GPtrArray* textures) {
  g_return_if_fail(FL_IS_VIEW(self));

  g_clear_pointer(&self->textures, g_ptr_array_unref);
  self->textures = g_ptr_array_ref(textures);

  gtk_widget_queue_draw(GTK_WIDGET(self));  // FIXME: queue_render?
}

GHashTable* fl_view_get_keyboard_state(FlView* self) {
  g_return_val_if_fail(FL_IS_VIEW(self), nullptr);

  return fl_keyboard_manager_get_pressed_state(self->keyboard_manager);
}
