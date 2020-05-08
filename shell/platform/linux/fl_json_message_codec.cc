// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/public/flutter_linux/fl_json_message_codec.h"

#include <gmodule.h>

G_DEFINE_QUARK(fl_json_message_codec_error_quark, fl_json_message_codec_error)

// JSON spec is at https://www.json.org/json-en.html

struct _FlJsonMessageCodec {
  FlMessageCodec parent_instance;
};

G_DEFINE_TYPE(FlJsonMessageCodec,
              fl_json_message_codec,
              fl_message_codec_get_type())

static FlValue* read_value(FlJsonMessageCodec* self,
                           GBytes* buffer,
                           size_t* offset,
                           GError** error);

// Returns true if the given character is JSON whitespace
static gboolean is_json_whitespace(gunichar value) {
  return value == ' ' || value == '\n' || value == '\r' || value == '\t';
}

// Converts a digit (0-9) to a character in UTF-8 encoding
static char json_int_to_digit(int value) {
  return '0' + value;
}

// Converts a hexadecimal digit (0-15) to a character in UTF-8 encoding
static char json_int_to_xdigit(int value) {
  return value < 10 ? '0' + value : 'a' + value - 10;
}

// Writes a single character to the buffer
static void write_char(GByteArray* buffer, gunichar value) {
  gchar data[6];
  gint data_length = g_unichar_to_utf8(value, data);
  g_byte_array_append(buffer, reinterpret_cast<const guint8*>(data),
                      data_length);
}

// Writes a string to the buffer
static void write_string(GByteArray* buffer, const gchar* value) {
  g_byte_array_append(buffer, reinterpret_cast<const guint8*>(value),
                      strlen(value));
}

// Writes an integer to the buffer
static void write_int(GByteArray* buffer, int64_t value) {
  // Special case the minimum value as it can't be inverted and fit in a signed
  // 64 bit value
  if (value == G_MININT64) {
    write_string(buffer, "-9223372036854775808");
    return;
  }

  if (value < 0) {
    write_char(buffer, '-');
    value = -value;
  }

  int64_t divisor = 1;
  while (value / divisor > 9)
    divisor *= 10;

  while (TRUE) {
    int64_t v = value / divisor;
    write_char(buffer, json_int_to_digit(v));
    if (divisor == 1)
      return;
    value -= v * divisor;
    divisor /= 10;
  }
}

// Writes a floating point number to the buffer
static gboolean write_double(GByteArray* buffer, double value, GError** error) {
  if (!isfinite(value)) {
    g_set_error(error, FL_JSON_MESSAGE_CODEC_ERROR,
                FL_JSON_MESSAGE_CODEC_ERROR_INVALID_NUMBER,
                "Can't encode NaN or Inf in JSON");
    return FALSE;
  }

  char text[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_dtostr(text, G_ASCII_DTOSTR_BUF_SIZE, value);
  write_string(buffer, text);

  // Add .0 if no decimal point so not confused with an integer
  if (strchr(text, '.') == nullptr)
    write_string(buffer, ".0");

  return TRUE;
}

// Writes a Unicode escape sequence for a JSON string
static void write_unicode_escape(GByteArray* buffer, gunichar c) {
  write_string(buffer, "\\u");
  write_char(buffer, json_int_to_xdigit((c >> 24) & 0xF));
  write_char(buffer, json_int_to_xdigit((c >> 16) & 0xF));
  write_char(buffer, json_int_to_xdigit((c >> 8) & 0xF));
  write_char(buffer, json_int_to_xdigit((c >> 0) & 0xF));
}

// Writes a #FlValue to @buffer or returns an error
static gboolean write_value(FlJsonMessageCodec* self,
                            GByteArray* buffer,
                            FlValue* value,
                            GError** error) {
  if (value == nullptr) {
    write_string(buffer, "null");
    return TRUE;
  }

  switch (fl_value_get_type(value)) {
    case FL_VALUE_TYPE_NULL:
      write_string(buffer, "null");
      break;
    case FL_VALUE_TYPE_BOOL:
      if (fl_value_get_bool(value))
        write_string(buffer, "true");
      else
        write_string(buffer, "false");
      break;
    case FL_VALUE_TYPE_INT:
      write_int(buffer, fl_value_get_int(value));
      break;
    case FL_VALUE_TYPE_FLOAT:
      if (!write_double(buffer, fl_value_get_float(value), error))
        return FALSE;
      break;
    case FL_VALUE_TYPE_STRING: {
      const gchar* string = fl_value_get_string(value);
      write_char(buffer, '\"');
      for (const gchar* s = string; *s != '\0'; s = g_utf8_next_char(s)) {
        gunichar c = g_utf8_get_char(s);
        if (c == '"')
          write_string(buffer, "\\\"");
        else if (c == '\\')
          write_string(buffer, "\\\\");
        else if (c == '\b')
          write_string(buffer, "\\b");
        else if (c == '\f')
          write_string(buffer, "\\f");
        else if (c == '\n')
          write_string(buffer, "\\n");
        else if (c == '\r')
          write_string(buffer, "\\r");
        else if (c == '\t')
          write_string(buffer, "\\t");
        else if (c < 0x20)
          write_unicode_escape(buffer, c);
        else
          write_char(buffer, c);
      }
      write_char(buffer, '\"');
      break;
    }
    case FL_VALUE_TYPE_UINT8_LIST: {
      write_char(buffer, '[');
      const uint8_t* values = fl_value_get_uint8_list(value);
      for (size_t i = 0; i < fl_value_get_length(value); i++) {
        if (i != 0)
          write_char(buffer, ',');
        write_int(buffer, values[i]);
      }
      write_char(buffer, ']');
      break;
    }
    case FL_VALUE_TYPE_INT32_LIST: {
      write_char(buffer, '[');
      const int32_t* values = fl_value_get_int32_list(value);
      for (size_t i = 0; i < fl_value_get_length(value); i++) {
        if (i != 0)
          write_char(buffer, ',');
        write_int(buffer, values[i]);
      }
      write_char(buffer, ']');
      break;
    }
    case FL_VALUE_TYPE_INT64_LIST: {
      write_char(buffer, '[');
      const int64_t* values = fl_value_get_int64_list(value);
      for (size_t i = 0; i < fl_value_get_length(value); i++) {
        if (i != 0)
          write_char(buffer, ',');
        write_int(buffer, values[i]);
      }
      write_char(buffer, ']');
      break;
    }
    case FL_VALUE_TYPE_FLOAT_LIST: {
      write_char(buffer, '[');
      const double* values = fl_value_get_float_list(value);
      for (size_t i = 0; i < fl_value_get_length(value); i++) {
        if (i != 0)
          write_char(buffer, ',');
        if (!write_double(buffer, values[i], error))
          return FALSE;
      }
      write_char(buffer, ']');
      break;
    }
    case FL_VALUE_TYPE_LIST:
      write_char(buffer, '[');
      for (size_t i = 0; i < fl_value_get_length(value); i++) {
        if (i != 0)
          write_char(buffer, ',');
        if (!write_value(self, buffer, fl_value_get_list_value(value, i),
                         error))
          return FALSE;
      }
      write_char(buffer, ']');
      break;
    case FL_VALUE_TYPE_MAP:
      write_char(buffer, '{');
      for (size_t i = 0; i < fl_value_get_length(value); i++) {
        if (i != 0)
          write_char(buffer, ',');

        FlValue* key = fl_value_get_map_key(value, i);
        if (fl_value_get_type(key) != FL_VALUE_TYPE_STRING) {
          g_set_error(error, FL_JSON_MESSAGE_CODEC_ERROR,
                      FL_JSON_MESSAGE_CODEC_ERROR_INVALID_OBJECT_KEY_TYPE,
                      "Invalid object key type");
          return FALSE;
        }
        if (!write_value(self, buffer, key, error))
          return FALSE;
        write_char(buffer, ':');

        if (!write_value(self, buffer, fl_value_get_map_value(value, i), error))
          return FALSE;
      }
      write_char(buffer, '}');
      break;
  }

  return TRUE;
}

// Returns the current character a the read location
static gunichar current_char(GBytes* buffer, size_t* offset) {
  gsize data_length;
  const gchar* data =
      static_cast<const char*>(g_bytes_get_data(buffer, &data_length));
  if (*offset >= data_length)
    return '\0';
  else
    return g_utf8_get_char(data + *offset);
}

// Moves to the next character in the buffer
static void next_char(GBytes* buffer, size_t* offset) {
  const gchar* data =
      static_cast<const char*>(g_bytes_get_data(buffer, nullptr));
  const gchar* c = g_utf8_next_char(data + *offset);
  *offset = c - data;
}

// Move the read pointer to the next non-whitespace location
static void read_whitespace(GBytes* buffer, size_t* offset) {
  while (is_json_whitespace(current_char(buffer, offset)))
    next_char(buffer, offset);
}

// Reads a JSON word from @buffer (e.g. 'true') or returns an error
static gboolean read_word(GBytes* buffer,
                          size_t* offset,
                          const gchar* word,
                          GError** error) {
  for (const gchar* s = word; *s != '\0'; s = g_utf8_next_char(s)) {
    gunichar c = current_char(buffer, offset);
    if (c != g_utf8_get_char(s)) {
      g_set_error(error, FL_MESSAGE_CODEC_ERROR, FL_MESSAGE_CODEC_ERROR_FAILED,
                  "Expected word %s not present", word);
      return FALSE;
    }
    next_char(buffer, offset);
  }

  return TRUE;
}

// Reads the JSON true value ('true') or returns an error
static FlValue* read_json_true(GBytes* buffer, size_t* offset, GError** error) {
  if (!read_word(buffer, offset, "true", error))
    return nullptr;
  return fl_value_new_bool(TRUE);
}

// Reads the JSON false value ('false') or returns an error
static FlValue* read_json_false(GBytes* buffer,
                                size_t* offset,
                                GError** error) {
  if (!read_word(buffer, offset, "false", error))
    return nullptr;
  return fl_value_new_bool(FALSE);
}

// Reads the JSON null value ('null') or returns an error
static FlValue* read_json_null(GBytes* buffer, size_t* offset, GError** error) {
  if (!read_word(buffer, offset, "null", error))
    return nullptr;
  return fl_value_new_null();
}

// Read a comma or return an error
static gboolean read_comma(GBytes* buffer, size_t* offset, GError** error) {
  gunichar c = current_char(buffer, offset);
  if (c != ',') {
    g_set_error(error, FL_JSON_MESSAGE_CODEC_ERROR,
                FL_JSON_MESSAGE_CODEC_ERROR_MISSING_COMMA,
                "Expected comma, got %02x", c);
    return FALSE;
  }
  next_char(buffer, offset);
  return TRUE;
}

// Reads a JSON unichar code (e.g. '0065') from @buffer or returns an error
static gboolean read_json_unichar_code(GBytes* buffer,
                                       size_t* offset,
                                       gunichar* value,
                                       GError** error) {
  gunichar wc = 0;
  for (int i = 0; i < 4; i++) {
    gunichar c = current_char(buffer, offset);
    int xdigit = g_ascii_xdigit_value(c);
    if (xdigit < 0) {
      g_set_error(error, FL_JSON_MESSAGE_CODEC_ERROR,
                  FL_JSON_MESSAGE_CODEC_ERROR_INVALID_STRING_UNICODE_ESCAPE,
                  "Missing hex digit in JSON unicode character");
      return FALSE;
    }
    wc = (wc << 4) + xdigit;
    next_char(buffer, offset);
  }

  *value = wc;
  return TRUE;
}

// Reads a JSON unicode escape sequence (e.g. '\u0065') from @buffer or returns
// an error
static gboolean read_json_string_escape(GBytes* buffer,
                                        size_t* offset,
                                        gunichar* value,
                                        GError** error) {
  gunichar c = current_char(buffer, offset);
  if (c == 'u') {
    next_char(buffer, offset);
    return read_json_unichar_code(buffer, offset, value, error);
  }

  if (c == '\"')
    *value = '\"';
  else if (c == '\\')
    *value = '\\';
  else if (c == '/')
    *value = '/';
  else if (c == 'b')
    *value = '\b';
  else if (c == 'f')
    *value = '\f';
  else if (c == 'n')
    *value = '\n';
  else if (c == 'r')
    *value = '\r';
  else if (c == 't')
    *value = '\t';
  else {
    g_set_error(error, FL_JSON_MESSAGE_CODEC_ERROR,
                FL_JSON_MESSAGE_CODEC_ERROR_INVALID_STRING_ESCAPE_SEQUENCE,
                "Unknown string escape character 0x%02x", c);
    return FALSE;
  }

  next_char(buffer, offset);
  return TRUE;
}

// Reads a JSON string (e.g. '"hello"') from @buffer or returns an error
static FlValue* read_json_string(GBytes* buffer,
                                 size_t* offset,
                                 GError** error) {
  g_assert(current_char(buffer, offset) == '\"');
  next_char(buffer, offset);

  g_autoptr(GString) text = g_string_new("");
  while (TRUE) {
    gunichar c = current_char(buffer, offset);
    if (c == '\"') {
      next_char(buffer, offset);
      return fl_value_new_string(text->str);
    } else if (c == '\\') {
      next_char(buffer, offset);
      gunichar wc = 0;
      if (!read_json_string_escape(buffer, offset, &wc, error))
        return nullptr;
      g_string_append_unichar(text, wc);
      continue;
    } else if (c == '\0') {
      g_set_error(error, FL_MESSAGE_CODEC_ERROR,
                  FL_MESSAGE_CODEC_ERROR_OUT_OF_DATA, "Unterminated string");
      return nullptr;
    } else if (c < 0x20) {
      g_set_error(error, FL_JSON_MESSAGE_CODEC_ERROR,
                  FL_JSON_MESSAGE_CODEC_ERROR_INVALID_STRING_CHARACTER,
                  "Invalid character in string");
      return nullptr;
    } else {
      g_string_append_unichar(text, c);
      next_char(buffer, offset);
    }
  }
}

// Reads a sequence of decimal digits (e.g. '1234') from @buffer or returns an
// error
static int64_t read_json_digits(GBytes* buffer,
                                size_t* offset,
                                int64_t* divisor) {
  int64_t value = 0;
  if (divisor != nullptr)
    *divisor = 1;

  while (TRUE) {
    gunichar c = current_char(buffer, offset);
    if (g_ascii_digit_value(c) < 0)
      return value;
    value = value * 10 + g_ascii_digit_value(c);
    if (divisor != nullptr)
      (*divisor) *= 10;
    next_char(buffer, offset);
  }
}

// Reads a JSON number (e.g. '-42', '3.16765e5') from @buffer or returns an
// error
static FlValue* read_json_number(GBytes* buffer,
                                 size_t* offset,
                                 GError** error) {
  gunichar c = current_char(buffer, offset);
  int64_t sign = 1;
  if (c == '-') {
    sign = -1;
    next_char(buffer, offset);
    c = current_char(buffer, offset);
    if (g_ascii_digit_value(c) < 0) {
      g_set_error(error, FL_JSON_MESSAGE_CODEC_ERROR,
                  FL_JSON_MESSAGE_CODEC_ERROR_INVALID_NUMBER,
                  "Mising digits after negative sign");
      return nullptr;
    }
  }

  int64_t value = 0;
  if (c == '0')
    next_char(buffer, offset);
  else
    value = read_json_digits(buffer, offset, nullptr);

  gboolean is_floating = FALSE;

  int64_t fraction = 0;
  int64_t divisor = 1;
  c = current_char(buffer, offset);
  if (c == '.') {
    is_floating = TRUE;
    next_char(buffer, offset);
    if (g_ascii_digit_value(current_char(buffer, offset)) < 0) {
      g_set_error(error, FL_JSON_MESSAGE_CODEC_ERROR,
                  FL_JSON_MESSAGE_CODEC_ERROR_INVALID_NUMBER,
                  "Mising digits after decimal point");
      return nullptr;
    }
    fraction = read_json_digits(buffer, offset, &divisor);
  }

  int64_t exponent = 0;
  int64_t exponent_sign = 1;
  if (c == 'E' || c == 'e') {
    is_floating = TRUE;
    next_char(buffer, offset);

    c = current_char(buffer, offset);
    if (c == '-') {
      exponent_sign = -1;
      next_char(buffer, offset);
    } else if (c == '+') {
      exponent_sign = 1;
      next_char(buffer, offset);
    }

    if (g_ascii_digit_value(current_char(buffer, offset)) < 0) {
      g_set_error(error, FL_JSON_MESSAGE_CODEC_ERROR,
                  FL_JSON_MESSAGE_CODEC_ERROR_INVALID_NUMBER,
                  "Mising digits in exponent");
      return nullptr;
    }
    exponent = read_json_digits(buffer, offset, nullptr);
  }

  if (is_floating)
    return fl_value_new_float(sign * (value + (double)fraction / divisor) *
                              pow(10, exponent_sign * exponent));
  else
    return fl_value_new_int(sign * value);
}

// Reads a JSON object (e.g. '{"name": count, "value": 42}' from @buffer or
// returns an error
static FlValue* read_json_object(FlJsonMessageCodec* self,
                                 GBytes* buffer,
                                 size_t* offset,
                                 GError** error) {
  g_assert(current_char(buffer, offset) == '{');
  next_char(buffer, offset);

  g_autoptr(FlValue) map = fl_value_new_map();
  while (TRUE) {
    read_whitespace(buffer, offset);

    gunichar c = current_char(buffer, offset);
    if (c == '\0') {
      g_set_error(error, FL_MESSAGE_CODEC_ERROR,
                  FL_MESSAGE_CODEC_ERROR_OUT_OF_DATA,
                  "Unterminated JSON object");
      return nullptr;
    }

    if (c == '}') {
      next_char(buffer, offset);
      return fl_value_ref(map);
    }

    if (fl_value_get_length(map) != 0) {
      if (!read_comma(buffer, offset, error))
        return nullptr;
      read_whitespace(buffer, offset);
    }

    c = current_char(buffer, offset);
    if (c != '\"') {
      g_set_error(error, FL_JSON_MESSAGE_CODEC_ERROR,
                  FL_JSON_MESSAGE_CODEC_ERROR_INVALID_OBJECT_KEY_TYPE,
                  "Missing string key in JSON object");
      return nullptr;
    }

    g_autoptr(FlValue) key = read_json_string(buffer, offset, error);
    if (key == nullptr)
      return nullptr;
    read_whitespace(buffer, offset);

    c = current_char(buffer, offset);
    if (c != ':') {
      g_set_error(error, FL_MESSAGE_CODEC_ERROR, FL_MESSAGE_CODEC_ERROR_FAILED,
                  "Missing colon after JSON object key");
      return nullptr;
    }
    next_char(buffer, offset);

    g_autoptr(FlValue) value = read_value(self, buffer, offset, error);
    if (value == nullptr)
      return nullptr;

    fl_value_set(map, key, value);
  }
}

// Reads a JSON array (e.g. '[1, 2, 3]') from @buffer or returns an error
static FlValue* read_json_array(FlJsonMessageCodec* self,
                                GBytes* buffer,
                                size_t* offset,
                                GError** error) {
  g_assert(current_char(buffer, offset) == '[');
  next_char(buffer, offset);

  g_autoptr(FlValue) value = fl_value_new_list();
  while (TRUE) {
    read_whitespace(buffer, offset);

    gunichar c = current_char(buffer, offset);
    if (c == '\0') {
      g_set_error(error, FL_MESSAGE_CODEC_ERROR,
                  FL_MESSAGE_CODEC_ERROR_OUT_OF_DATA,
                  "Unterminated JSON array");
      return nullptr;
    }

    if (c == ']') {
      next_char(buffer, offset);
      return fl_value_ref(value);
    }

    if (fl_value_get_length(value) != 0) {
      if (!read_comma(buffer, offset, error))
        return nullptr;
      read_whitespace(buffer, offset);
    }

    g_autoptr(FlValue) child = read_value(self, buffer, offset, error);
    if (child == nullptr)
      return nullptr;

    fl_value_append(value, child);
  }
}

// Reads a #FlValue from @buffer or returns an error
static FlValue* read_value(FlJsonMessageCodec* self,
                           GBytes* buffer,
                           size_t* offset,
                           GError** error) {
  read_whitespace(buffer, offset);

  gunichar c = current_char(buffer, offset);
  g_autoptr(FlValue) value = nullptr;
  if (c == '{')
    value = read_json_object(self, buffer, offset, error);
  else if (c == '[')
    value = read_json_array(self, buffer, offset, error);
  else if (c == '\"')
    value = read_json_string(buffer, offset, error);
  else if (c == '-' || g_ascii_digit_value(c) >= 0)
    value = read_json_number(buffer, offset, error);
  else if (c == 't')
    value = read_json_true(buffer, offset, error);
  else if (c == 'f')
    value = read_json_false(buffer, offset, error);
  else if (c == 'n')
    value = read_json_null(buffer, offset, error);
  else if (c == '\0') {
    g_set_error(error, FL_MESSAGE_CODEC_ERROR,
                FL_MESSAGE_CODEC_ERROR_OUT_OF_DATA,
                "Out of data looking for JSON value");
    return nullptr;
  } else {
    g_set_error(error, FL_MESSAGE_CODEC_ERROR, FL_MESSAGE_CODEC_ERROR_FAILED,
                "Unexpected value 0x%02x when decoding JSON value", c);
    return nullptr;
  }

  if (value == nullptr)
    return nullptr;

  read_whitespace(buffer, offset);

  return fl_value_ref(value);
}

// Implements FlMessageCodec:encode_message
static GBytes* fl_json_message_codec_encode_message(FlMessageCodec* codec,
                                                    FlValue* message,
                                                    GError** error) {
  FlJsonMessageCodec* self = reinterpret_cast<FlJsonMessageCodec*>(codec);

  g_autoptr(GByteArray) buffer = g_byte_array_new();
  if (!write_value(self, buffer, message, error))
    return nullptr;
  return g_byte_array_free_to_bytes(
      static_cast<GByteArray*>(g_steal_pointer(&buffer)));
}

// Implements FlMessageCodec:decode_message
static FlValue* fl_json_message_codec_decode_message(FlMessageCodec* codec,
                                                     GBytes* message,
                                                     GError** error) {
  FlJsonMessageCodec* self = reinterpret_cast<FlJsonMessageCodec*>(codec);

  gsize data_length;
  const gchar* data =
      static_cast<const char*>(g_bytes_get_data(message, &data_length));
  if (!g_utf8_validate(data, data_length, nullptr)) {
    g_set_error(error, FL_JSON_MESSAGE_CODEC_ERROR,
                FL_JSON_MESSAGE_CODEC_ERROR_INVALID_UTF8,
                "Message is not valid UTF-8");
    return nullptr;
  }

  size_t offset = 0;
  g_autoptr(FlValue) value = read_value(self, message, &offset, error);
  if (value == nullptr)
    return nullptr;

  if (offset != g_bytes_get_size(message)) {
    g_set_error(error, FL_MESSAGE_CODEC_ERROR,
                FL_MESSAGE_CODEC_ERROR_ADDITIONAL_DATA,
                "Unused %zi bytes after JSON message",
                g_bytes_get_size(message) - offset);
    return nullptr;
  }

  return fl_value_ref(value);
}

static void fl_json_message_codec_class_init(FlJsonMessageCodecClass* klass) {
  FL_MESSAGE_CODEC_CLASS(klass)->encode_message =
      fl_json_message_codec_encode_message;
  FL_MESSAGE_CODEC_CLASS(klass)->decode_message =
      fl_json_message_codec_decode_message;
}

static void fl_json_message_codec_init(FlJsonMessageCodec* self) {}

G_MODULE_EXPORT FlJsonMessageCodec* fl_json_message_codec_new() {
  return static_cast<FlJsonMessageCodec*>(
      g_object_new(fl_json_message_codec_get_type(), nullptr));
}

G_MODULE_EXPORT gchar* fl_json_message_codec_encode(FlJsonMessageCodec* codec,
                                                    FlValue* value,
                                                    GError** error) {
  g_return_val_if_fail(FL_IS_JSON_CODEC(codec), nullptr);

  g_autoptr(GByteArray) buffer = g_byte_array_new();
  if (!write_value(codec, buffer, value, error))
    return nullptr;
  guint8 nul = '\0';
  g_byte_array_append(buffer, &nul, 1);
  return reinterpret_cast<gchar*>(g_byte_array_free(
      static_cast<GByteArray*>(g_steal_pointer(&buffer)), FALSE));
}

G_MODULE_EXPORT FlValue* fl_json_message_codec_decode(FlJsonMessageCodec* codec,
                                                      const gchar* text,
                                                      GError** error) {
  g_return_val_if_fail(FL_IS_JSON_CODEC(codec), nullptr);

  g_autoptr(GBytes) data = g_bytes_new_static(text, strlen(text));
  g_autoptr(FlValue) value = fl_json_message_codec_decode_message(
      FL_MESSAGE_CODEC(codec), data, error);
  if (value == nullptr)
    return nullptr;

  return fl_value_ref(value);
}
