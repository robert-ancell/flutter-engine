// Copyright 2013 The Flutter Authors. All rights reserved.

// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/testing/mock_keymap.h"

using namespace flutter::testing;

static MockKeymap* mock = nullptr;

MockKeymap::MockKeymap() {
  mock = this;
}

GdkKeymap* gdk_keymap_get_for_display(GdkDisplay* display) {
  return reinterpret_cast<GdkKeymap*>(mock);
}

guint gdk_keymap_lookup_key(GdkKeymap* keymap, const GdkKeymapKey* key) {
  return 0;
}
