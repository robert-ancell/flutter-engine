// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/fl_key_event.h"

FlKeyEvent* fl_key_event_new(guint32 time,
                             bool is_press,
                             guint16 keycode,
                             guint keyval,
                             GdkModifierType state,
                             guint group) {
  FlKeyEvent* result = g_new(FlKeyEvent, 1);

  result->time = time;
  result->is_press = is_press;
  result->keycode = keycode;
  result->keyval = keyval;
  result->state = state;
  result->group = group;

  return result;
}

void fl_key_event_dispose(FlKeyEvent* event) {
  g_free(event);
}

FlKeyEvent* fl_key_event_clone(const FlKeyEvent* event) {
  FlKeyEvent* new_event = g_new(FlKeyEvent, 1);
  *new_event = *event;
  return new_event;
}
