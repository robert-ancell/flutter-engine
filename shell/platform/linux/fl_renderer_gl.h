// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_FL_RENDERER_GL_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_FL_RENDERER_GL_H_

#include "flutter/shell/platform/linux/fl_renderer.h"

G_BEGIN_DECLS

G_DECLARE_FINAL_TYPE(FlRendererGL, fl_renderer_gl, FL, RENDERER_GL, FlRenderer)

/**
 * FlRendererGL:
 *
 * #FlRendererGL is an implementation of #FlRenderer that renders by OpenGL ES.
 */

/**
 * fl_renderer_gl_new:
 * @window: the window being rendered to.
 *
 * Creates an object that allows Flutter to render by OpenGL ES.
 *
 * Returns: a new #FlRendererGL.
 */
FlRendererGL* fl_renderer_gl_new(GdkWindow* window);

/**
 * fl_renderer_gl_get_proc_address:
 * @renderer: an #FlRendererGL.
 * @name: a function name.
 *
 * Gets the rendering API function that matches the given name.
 *
 * Returns: a function pointer.
 */
void* fl_renderer_gl_get_proc_address(FlRendererGL* renderer, const char* name);

/**
 * fl_renderer_gl_get_fbo:
 * @renderer: an #FlRenderer.
 *
 * Gets the frame buffer object to render to.
 *
 * Returns: a frame buffer object index.
 */
guint32 fl_renderer_gl_get_fbo(FlRendererGL* renderer);

/**
 * fl_renderer_gl_make_current:
 * @renderer: an #FlRenderer.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL
 * to ignore.
 *
 * Makes the rendering context current.
 *
 * Returns %TRUE if successful.
 */
gboolean fl_renderer_gl_make_current(FlRendererGL* renderer, GError** error);

/**
 * fl_renderer_gl_make_resource_current:
 * @renderer: an #FlRenderer.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL
 * to ignore.
 *
 * Makes the resource rendering context current.
 *
 * Returns %TRUE if successful.
 */
gboolean fl_renderer_gl_make_resource_current(FlRendererGL* renderer,
                                              GError** error);

/**
 * fl_renderer_gl_clear_current:
 * @renderer: an #FlRenderer.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL
 * to ignore.
 *
 * Clears the current rendering context.
 *
 * Returns %TRUE if successful.
 */
gboolean fl_renderer_gl_clear_current(FlRendererGL* renderer, GError** error);

G_END_DECLS

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_FL_RENDERER_GL_H_
