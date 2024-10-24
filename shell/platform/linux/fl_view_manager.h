// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_FL_VIEW_MANAGER_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_FL_VIEW_MANAGER_H_

#include <glib-object.h>

#include "flutter/shell/platform/embedder/embedder.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_engine.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_view.h"

G_BEGIN_DECLS

G_DECLARE_FINAL_TYPE(FlViewManager, fl_view_manager, FL, VIEW_MANAGER, GObject)

/**
 * fl_view_manager_new:
 *
 * Creates new view manager.
 *
 * Returns: a new #FlViewManager.
 */
FlViewManager* fl_view_manager_new();

/**
 * fl_view_manager_add_view:
 * @manager: an #FlViewManager
 * @view: an #FlView.
 *
 * Adds a view to the manager.
 */
void fl_view_manager_add_view(FlViewManager* manager, FlView* view);

/**
 * fl_view_manager_get_view:
 * @manager: an #FlViewManager
 * @view_id: a Flutter view ID.
 *
 * Gets the view with the given ID.
 *
 * Returns: (allow-none): an #FlView or %NULL if no view with this ID.
 */
FlView* fl_view_manager_get_view(FlViewManager* manager, FlutterViewId view_id);

G_END_DECLS

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_FL_VIEW_MANAGER_H_
