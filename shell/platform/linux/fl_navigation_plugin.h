// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_NAVIGATION_LINUX_FL_NAVIGATION_PLUGIN_H_
#define FLUTTER_SHELL_NAVIGATION_LINUX_FL_NAVIGATION_PLUGIN_H_

#include "flutter/shell/platform/linux/public/flutter_linux/fl_binary_messenger.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_value.h"

G_BEGIN_DECLS

G_DECLARE_FINAL_TYPE(FlNavigationPlugin,
                     fl_navigation_plugin,
                     FL,
                     NAVIGATION_PLUGIN,
                     GObject);

/**
 * FlNavigationPlugin:
 *
 * #FlNavigationPlugin is a plugin that implements the shell side
 * of SystemChannels.navigation from the Flutter services library.
 */

/**
 * fl_navigation_plugin_new:
 * @messenger: an #FlBinaryMessenger.
 *
 * Creates a new plugin that implements SystemChannels.navigation from the
 * Flutter services library.
 *
 * Returns: a new #FlNavigationPlugin
 */
FlNavigationPlugin* fl_navigation_plugin_new(FlBinaryMessenger* messenger);

/**
 * fl_navigation_plugin_pop_route:
 * @plugin: an #FlNavigationPlugin.
 *
 * FIXME.
 */
void fl_navigation_plugin_pop_route(FlNavigationPlugin* plugin);

/**
 * fl_navigation_plugin_push_route
 * @plugin: an #FlNavigationPlugin.
 * @route: the route to navigate to.
 *
 * FIXME
 */
void fl_navigation_plugin_push_route(FlNavigationPlugin* plugin,
                                     const gchar* route);

/**
 * fl_navigation_plugin_push_route_information:
 * @plugin: an #FlNavigationPlugin.
 * @location: FIXME
 * @state: FIXME
 *
 * FIXME
 */
void fl_navigation_plugin_push_route_information(FlNavigationPlugin* plugin,
                                                 const gchar* location,
                                                 FlValue* state);

G_END_DECLS

#endif  // FLUTTER_SHELL_NAVIGATION_LINUX_FL_NAVIGATION_PLUGIN_H_
