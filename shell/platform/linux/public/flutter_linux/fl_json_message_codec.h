// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_FL_JSON_MESSAGE_CODEC_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_FL_JSON_MESSAGE_CODEC_H_

#if !defined(__FLUTTER_LINUX_INSIDE__) && !defined(FLUTTER_LINUX_COMPILATION)
#error "Only <flutter_linux/flutter_linux.h> can be included directly."
#endif

#include "fl_message_codec.h"

G_BEGIN_DECLS

/**
 * FlJsonMessageCodecError:
 * @FL_JSON_MESSAGE_CODEC_ERROR_INVALID_UTF8: Message is not valid UTF-8.
 * @FL_JSON_MESSAGE_CODEC_ERROR_MISSING_COMMA: Missing a comma in a list or
 * object
 * @FL_JSON_MESSAGE_CODEC_ERROR_INVALID_NUMBER: Invalid number
 * @FL_JSON_MESSAGE_CODEC_ERROR_INVALID_STRING_CHARACTER: Invalid character in a
 * string
 * @FL_JSON_MESSAGE_CODEC_ERROR_INVALID_STRING_ESCAPE_SEQUENCE: Invalid string
 * escape sequencc
 * @FL_JSON_MESSAGE_CODEC_ERROR_INVALID_STRING_UNICODE_ESCAPE: Invalid unicode
 * escape sequence
 * @FL_JSON_MESSAGE_CODEC_ERROR_INVALID_OBJECT_KEY_TYPE: Invalid object key type
 *
 * Errors for #FlJsonMessageCodec objects to set on failures.
 */
#define FL_JSON_MESSAGE_CODEC_ERROR fl_json_message_codec_error_quark()

typedef enum {
  FL_JSON_MESSAGE_CODEC_ERROR_INVALID_UTF8,
  FL_JSON_MESSAGE_CODEC_ERROR_MISSING_COMMA,
  FL_JSON_MESSAGE_CODEC_ERROR_INVALID_NUMBER,
  FL_JSON_MESSAGE_CODEC_ERROR_INVALID_STRING_CHARACTER,
  FL_JSON_MESSAGE_CODEC_ERROR_INVALID_STRING_ESCAPE_SEQUENCE,
  FL_JSON_MESSAGE_CODEC_ERROR_INVALID_STRING_UNICODE_ESCAPE,
  FL_JSON_MESSAGE_CODEC_ERROR_INVALID_OBJECT_KEY_TYPE,
} FlJsonMessageCodecError;

GQuark fl_json_message_codec_error_quark(void) G_GNUC_CONST;

G_DECLARE_FINAL_TYPE(FlJsonMessageCodec,
                     fl_json_message_codec,
                     FL,
                     JSON_CODEC,
                     FlMessageCodec)

/**
 * FlJsonMessageCodec:
 *
 * #FlJsonMessageCodec is an #FlMessageCodec that implements the Flutter
 * standard message encoding. This encodes and decodes #FlValue of type
 * #FL_VALUE_TYPE_NULL, #FL_VALUE_TYPE_BOOL, #FL_VALUE_TYPE_INT,
 * #FL_VALUE_TYPE_FLOAT, #FL_VALUE_TYPE_STRING, #FL_VALUE_TYPE_UINT8_LIST,
 * #FL_VALUE_TYPE_INT32_LIST, #FL_VALUE_TYPE_INT64_LIST,
 * #FL_VALUE_TYPE_FLOAT_LIST, #FL_VALUE_TYPE_LIST, and #FL_VALUE_TYPE_MAP
 *
 * #FlJsonMessageCodec matches the JSONMessageCodec class in the Flutter
 * services library.
 */

/**
 * fl_json_message_codec_new:
 *
 * Creates a #FlJsonMessageCodec.
 *
 * Returns: a new #FlJsonMessageCodec
 */
FlJsonMessageCodec* fl_json_message_codec_new();

/**
 * fl_json_message_codec_encode:
 * @codec: a #FlJsonMessageCodec
 * @value: value to encode
 * @error: (allow-none): #GError location to store the error occurring, or %NULL
 *
 * Encode a value to a JSON string.
 *
 * Returns: a JSON representation of this value or %NULL on error.
 */
gchar* fl_json_message_codec_encode(FlJsonMessageCodec* codec,
                                    FlValue* value,
                                    GError** error);

/**
 * fl_json_message_codec_decode:
 * @codec: a #FlJsonMessageCodec
 * @text: UTF-8 text in JSON format
 * @error: (allow-none): #GError location to store the error occurring, or %NULL
 *
 * Decode a value from a JSON string.
 *
 * Returns: a #FlValue or %NULL on error
 */
FlValue* fl_json_message_codec_decode(FlJsonMessageCodec* codec,
                                      const gchar* text,
                                      GError** error);

G_END_DECLS

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_FL_JSON_MESSAGE_CODEC_H_
