// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/fl_renderer_gdk.h"

struct _FlRendererGdk {
  FlRenderer parent_instance;

  // Window/surface being rendered on.
#if GTK_CHECK_VERSION(4, 0, 0)
  GdkSurface* surface;
#else
  GdkWindow* window;
#endif

  // OpenGL rendering context used by GDK.
  GdkGLContext* gdk_context;

  // Main OpenGL rendering context used by Flutter.
  GdkGLContext* main_context;

  // Secondary OpenGL rendering context used by Flutter.
  GdkGLContext* resource_context;
};

G_DEFINE_TYPE(FlRendererGdk, fl_renderer_gdk, fl_renderer_get_type())

static GdkGLContext* create_gl_context(FlRendererGdk* self, GError** error) {
#if GTK_CHECK_VERSION(4, 0, 0)
  return gdk_surface_create_gl_context(self->surface, error);
#else
  return gdk_window_create_gl_context(self->window, error);
#endif
}

// Implements FlRenderer::make_current.
static void fl_renderer_gdk_make_current(FlRenderer* renderer) {
  FlRendererGdk* self = FL_RENDERER_GDK(renderer);
  gdk_gl_context_make_current(self->main_context);
}

// Implements FlRenderer::make_resource_current.
static void fl_renderer_gdk_make_resource_current(FlRenderer* renderer) {
  FlRendererGdk* self = FL_RENDERER_GDK(renderer);
  gdk_gl_context_make_current(self->resource_context);
}

// Implements FlRenderer::clear_current.
static void fl_renderer_gdk_clear_current(FlRenderer* renderer) {
  gdk_gl_context_clear_current();
}

static gdouble fl_renderer_gdk_get_refresh_rate(FlRenderer* renderer) {
  FlRendererGdk* self = FL_RENDERER_GDK(renderer);
#if GTK_CHECK_VERSION(4, 0, 0)
  GdkDisplay* display = gdk_surface_get_display(self->surface);
  GdkMonitor* monitor =
      gdk_display_get_monitor_at_surface(display, self->surface);
#else
  GdkDisplay* display = gdk_window_get_display(self->window);
  GdkMonitor* monitor =
      gdk_display_get_monitor_at_window(display, self->window);
#endif
  if (monitor == nullptr) {
    return -1.0;
  }

  int refresh_rate = gdk_monitor_get_refresh_rate(monitor);
  if (refresh_rate <= 0) {
    return -1.0;
  }
  // the return value is in milli-hertz, convert to hertz
  return static_cast<gdouble>(refresh_rate) / 1000.0;
}

static void fl_renderer_gdk_dispose(GObject* object) {
  FlRendererGdk* self = FL_RENDERER_GDK(object);

  g_clear_object(&self->gdk_context);
  g_clear_object(&self->main_context);
  g_clear_object(&self->resource_context);

  G_OBJECT_CLASS(fl_renderer_gdk_parent_class)->dispose(object);
}

static void fl_renderer_gdk_class_init(FlRendererGdkClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fl_renderer_gdk_dispose;

  FL_RENDERER_CLASS(klass)->make_current = fl_renderer_gdk_make_current;
  FL_RENDERER_CLASS(klass)->make_resource_current =
      fl_renderer_gdk_make_resource_current;
  FL_RENDERER_CLASS(klass)->clear_current = fl_renderer_gdk_clear_current;
  FL_RENDERER_CLASS(klass)->get_refresh_rate = fl_renderer_gdk_get_refresh_rate;
}

static void fl_renderer_gdk_init(FlRendererGdk* self) {}

#if GTK_CHECK_VERSION(4, 0, 0)
FlRendererGdk* fl_renderer_gdk_new(GdkSurface* surface) {
#else
FlRendererGdk* fl_renderer_gdk_new(GdkWindow* window) {
#endif
  FlRendererGdk* self =
      FL_RENDERER_GDK(g_object_new(fl_renderer_gdk_get_type(), nullptr));
#if GTK_CHECK_VERSION(4, 0, 0)
  self->surface = surface;
#else
  self->window = window;
#endif
  return self;
}

gboolean fl_renderer_gdk_create_contexts(FlRendererGdk* self, GError** error) {
  self->gdk_context = create_gl_context(self, error);
  if (self->gdk_context == nullptr) {
    return FALSE;
  }
  if (!gdk_gl_context_realize(self->gdk_context, error)) {
    return FALSE;
  }

  self->main_context = create_gl_context(self, error);
  if (self->main_context == nullptr) {
    return FALSE;
  }
  if (!gdk_gl_context_realize(self->main_context, error)) {
    return FALSE;
  }

  self->resource_context = create_gl_context(self, error);
  if (self->resource_context == nullptr) {
    return FALSE;
  }
  if (!gdk_gl_context_realize(self->resource_context, error)) {
    return FALSE;
  }

  return TRUE;
}

GdkGLContext* fl_renderer_gdk_get_context(FlRendererGdk* self) {
  g_return_val_if_fail(FL_IS_RENDERER_GDK(self), nullptr);
  return self->gdk_context;
}
