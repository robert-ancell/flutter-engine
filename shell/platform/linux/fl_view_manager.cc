// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/fl_view_manager.h"

struct _FlViewManager {
  GObject parent_instance;
};

G_DEFINE_TYPE(FlViewManager, fl_view_manager, G_TYPE_OBJECT)

static void fl_view_manager_class_init(FlViewManagerClass* klass) {}

static void fl_view_manager_init(FlViewManager* self) {}

FlViewManager* fl_view_manager_new() {
  return FL_VIEW_MANAGER(g_object_new(fl_view_manager_get_type(), nullptr));
}
