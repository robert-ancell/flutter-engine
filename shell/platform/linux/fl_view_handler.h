// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_FL_VIEW_HANDLER_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_FL_VIEW_HANDLER_H_

#include "flutter/shell/platform/linux/public/flutter_linux/fl_engine.h"

G_BEGIN_DECLS

G_MODULE_EXPORT
G_DECLARE_FINAL_TYPE(FlViewHandler, fl_view_handler, FL, VIEW_HANDLER, GObject)

/**
 * FlViewHandler:
 *
 * #FlViewHandler creates and destroy views as requested by Flutter.
 */

/**
 * fl_view_handler_new:
 * @engine: an #FlEngine.
 *
 * Creates a handler to add and remove views.
 *
 * Returns: a new #FlViewHandler.
 */
FlViewHandler* fl_view_handler_new(FlEngine* engine);

G_END_DECLS

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_FL_VIEW_HANDLER_H_
