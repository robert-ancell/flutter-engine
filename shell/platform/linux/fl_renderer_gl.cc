// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <epoxy/egl.h>

#include "flutter/shell/platform/linux/fl_renderer_gl.h"

#include "flutter/shell/platform/linux/fl_backing_store_provider.h"
#include "flutter/shell/platform/linux/fl_view_private.h"

struct _FlRendererGL {
  FlRenderer parent_instance;

  GdkWindow* window;

  GdkGLContext* main_context;
  GdkGLContext* resource_context;
};

G_DEFINE_TYPE(FlRendererGL, fl_renderer_gl, fl_renderer_get_type())

// Implements FlRenderer::start
static gboolean fl_renderer_gl_start(FlRenderer* renderer, GError** error) {
  FlRendererGL* self = FL_RENDERER_GL(renderer);

  self->main_context = gdk_window_create_gl_context(self->window, error);
  if (self->main_context == nullptr) {
    return FALSE;
  }

  self->resource_context = gdk_window_create_gl_context(self->window, error);
  if (self->resource_context == nullptr) {
    return FALSE;
  }

  if (!gdk_gl_context_realize(self->main_context, error) ||
      !gdk_gl_context_realize(self->resource_context, error)) {
    return FALSE;
  }

  return TRUE;
}

// Implements FlRenderer::create_backing_store.
static gboolean fl_renderer_gl_create_backing_store(
    FlRenderer* renderer,
    const FlutterBackingStoreConfig* config,
    FlutterBackingStore* backing_store_out) {
  FlRendererGL* self = FL_RENDERER_GL(renderer);

  g_autoptr(GError) error = nullptr;
  gboolean result = fl_renderer_gl_make_current(self, &error);
  if (!result) {
    g_warning("Failed to make renderer current when creating backing store: %s",
              error->message);
    return FALSE;
  }

  FlBackingStoreProvider* provider =
      fl_backing_store_provider_new(config->size.width, config->size.height);
  if (!provider) {
    g_warning("Failed to create backing store");
    return FALSE;
  }

  uint32_t name = fl_backing_store_provider_get_gl_framebuffer_id(provider);
  uint32_t format = fl_backing_store_provider_get_gl_format(provider);

  backing_store_out->type = kFlutterBackingStoreTypeOpenGL;
  backing_store_out->open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;
  backing_store_out->open_gl.framebuffer.user_data = provider;
  backing_store_out->open_gl.framebuffer.name = name;
  backing_store_out->open_gl.framebuffer.target = format;
  backing_store_out->open_gl.framebuffer.destruction_callback = [](void* p) {
    // Backing store destroyed in fl_renderer_gl_collect_backing_store(), set
    // on FlutterCompositor.collect_backing_store_callback during engine start.
  };

  return TRUE;
}

// Implements FlRenderer::collect_backing_store.
static gboolean fl_renderer_gl_collect_backing_store(
    FlRenderer* renderer,
    const FlutterBackingStore* backing_store) {
  FlRendererGL* self = FL_RENDERER_GL(renderer);

  g_autoptr(GError) error = nullptr;
  gboolean result = fl_renderer_gl_make_current(self, &error);
  if (!result) {
    g_warning(
        "Failed to make renderer current when collecting backing store: %s",
        error->message);
    return FALSE;
  }

  // OpenGL context is required when destroying #FlBackingStoreProvider.
  g_object_unref(backing_store->open_gl.framebuffer.user_data);
  return TRUE;
}

// Implements FlRenderer::present_layers.
static gboolean fl_renderer_gl_present_layers(FlRenderer* renderer,
                                              const FlutterLayer** layers,
                                              size_t layers_count) {
  FlRendererGL* self = FL_RENDERER_GL(renderer);

  FlView* view = fl_renderer_get_view(renderer);
  if (!view || !self->main_context) {
    return FALSE;
  }

  g_autoptr(GPtrArray) textures = g_ptr_array_new();
  for (size_t i = 0; i < layers_count; ++i) {
    const FlutterLayer* layer = layers[i];
    switch (layer->type) {
      case kFlutterLayerContentTypeBackingStore: {
        const FlutterBackingStore* backing_store = layer->backing_store;
        auto framebuffer = &backing_store->open_gl.framebuffer;
        g_ptr_array_add(textures, reinterpret_cast<FlBackingStoreProvider*>(
                                      framebuffer->user_data));
      } break;
      case kFlutterLayerContentTypePlatformView: {
        // Currently unsupported.
      } break;
    }
  }

  fl_view_set_textures(view, self->main_context, textures);

  return TRUE;
}

static void fl_renderer_gl_dispose(GObject* object) {
  FlRendererGL* self = FL_RENDERER_GL(object);

  g_clear_object(&self->main_context);
  g_clear_object(&self->resource_context);

  G_OBJECT_CLASS(fl_renderer_gl_parent_class)->dispose(object);
}

static void fl_renderer_gl_class_init(FlRendererGLClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fl_renderer_gl_dispose;

  FL_RENDERER_CLASS(klass)->start = fl_renderer_gl_start;
  FL_RENDERER_CLASS(klass)->create_backing_store =
      fl_renderer_gl_create_backing_store;
  FL_RENDERER_CLASS(klass)->collect_backing_store =
      fl_renderer_gl_collect_backing_store;
  FL_RENDERER_CLASS(klass)->present_layers = fl_renderer_gl_present_layers;
}

static void fl_renderer_gl_init(FlRendererGL* self) {}

FlRendererGL* fl_renderer_gl_new(GdkWindow* window) {
  FlRendererGL* self =
      FL_RENDERER_GL(g_object_new(fl_renderer_gl_get_type(), nullptr));
  self->window = window;
  return self;
}

void* fl_renderer_gl_get_proc_address(FlRendererGL* self, const char* name) {
  g_return_val_if_fail(FL_IS_RENDERER_GL(self), NULL);
  return reinterpret_cast<void*>(eglGetProcAddress(name));
}

guint32 fl_renderer_gl_get_fbo(FlRendererGL* self) {
  g_return_val_if_fail(FL_IS_RENDERER_GL(self), 0);
  // There is only one frame buffer object - always return that.
  return 0;
}

gboolean fl_renderer_gl_make_current(FlRendererGL* self, GError** error) {
  g_return_val_if_fail(FL_IS_RENDERER_GL(self), FALSE);

  if (self->main_context) {
    gdk_gl_context_make_current(self->main_context);
  }

  return TRUE;
}

gboolean fl_renderer_gl_make_resource_current(FlRendererGL* self,
                                              GError** error) {
  g_return_val_if_fail(FL_IS_RENDERER_GL(self), FALSE);

  if (self->resource_context) {
    gdk_gl_context_make_current(self->resource_context);
  }

  return TRUE;
}

gboolean fl_renderer_gl_clear_current(FlRendererGL* self, GError** error) {
  g_return_val_if_fail(FL_IS_RENDERER_GL(self), FALSE);

  gdk_gl_context_clear_current();
  return TRUE;
}
