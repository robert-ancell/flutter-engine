// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/fl_keyboard_handler.h"

#include <cstring>
#include <vector>

#include "flutter/shell/platform/embedder/test_utils/key_codes.g.h"
#include "flutter/shell/platform/linux/fl_binary_messenger_private.h"
#include "flutter/shell/platform/linux/fl_method_codec_private.h"
#include "flutter/shell/platform/linux/key_mapping.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_json_message_codec.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_method_codec.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_standard_method_codec.h"
#include "flutter/shell/platform/linux/testing/fl_test.h"
#include "flutter/shell/platform/linux/testing/mock_binary_messenger.h"
#include "flutter/shell/platform/linux/testing/mock_keymap.h"
#include "flutter/shell/platform/linux/testing/mock_text_input_handler.h"
#include "flutter/testing/testing.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

// Define compound `expect` in macros. If they were defined in functions, the
// stacktrace wouldn't print where the function is called in the unit tests.

#define EXPECT_KEY_EVENT(RECORD, TYPE, PHYSICAL, LOGICAL, CHAR, SYNTHESIZED) \
  EXPECT_EQ((RECORD).type, CallRecord::kKeyCallEmbedder);                    \
  EXPECT_EQ((RECORD).event->type, (TYPE));                                   \
  EXPECT_EQ((RECORD).event->physical, (PHYSICAL));                           \
  EXPECT_EQ((RECORD).event->logical, (LOGICAL));                             \
  EXPECT_STREQ((RECORD).event->character, (CHAR));                           \
  EXPECT_EQ((RECORD).event->synthesized, (SYNTHESIZED));

#define VERIFY_DOWN(OUT_LOGICAL, OUT_CHAR)                          \
  EXPECT_EQ(call_records[0].type, CallRecord::kKeyCallEmbedder);    \
  EXPECT_EQ(call_records[0].event->type, kFlutterKeyEventTypeDown); \
  EXPECT_EQ(call_records[0].event->logical, (OUT_LOGICAL));         \
  EXPECT_STREQ(call_records[0].event->character, (OUT_CHAR));       \
  EXPECT_EQ(call_records[0].event->synthesized, false);             \
  call_records.clear()

namespace {
using ::flutter::testing::keycodes::kLogicalAltLeft;
using ::flutter::testing::keycodes::kLogicalBracketLeft;
using ::flutter::testing::keycodes::kLogicalComma;
using ::flutter::testing::keycodes::kLogicalControlLeft;
using ::flutter::testing::keycodes::kLogicalDigit1;
using ::flutter::testing::keycodes::kLogicalKeyA;
using ::flutter::testing::keycodes::kLogicalKeyB;
using ::flutter::testing::keycodes::kLogicalKeyM;
using ::flutter::testing::keycodes::kLogicalKeyQ;
using ::flutter::testing::keycodes::kLogicalMetaLeft;
using ::flutter::testing::keycodes::kLogicalMinus;
using ::flutter::testing::keycodes::kLogicalParenthesisRight;
using ::flutter::testing::keycodes::kLogicalSemicolon;
using ::flutter::testing::keycodes::kLogicalShiftLeft;
using ::flutter::testing::keycodes::kLogicalUnderscore;

using ::flutter::testing::keycodes::kPhysicalAltLeft;
using ::flutter::testing::keycodes::kPhysicalControlLeft;
using ::flutter::testing::keycodes::kPhysicalKeyA;
using ::flutter::testing::keycodes::kPhysicalKeyB;
using ::flutter::testing::keycodes::kPhysicalMetaLeft;
using ::flutter::testing::keycodes::kPhysicalShiftLeft;

// Hardware key codes.
typedef std::function<void(bool handled)> AsyncKeyCallback;
typedef std::function<void(AsyncKeyCallback callback)> ChannelCallHandler;
typedef std::function<void(const FlutterKeyEvent* event,
                           AsyncKeyCallback callback)>
    EmbedderCallHandler;
typedef std::function<void(FlKeyEvent*)> RedispatchHandler;

// A type that can record all kinds of effects that the keyboard handler
// triggers.
//
// An instance of `CallRecord` might not have all the fields filled.
typedef struct {
  enum {
    kKeyCallEmbedder,
    kKeyCallChannel,
  } type;

  AsyncKeyCallback callback;
  std::unique_ptr<FlutterKeyEvent> event;
  std::unique_ptr<char[]> event_character;
} CallRecord;

// Clone a C-string.
//
// Must be deleted by delete[].
char* cloneString(const char* source) {
  if (source == nullptr) {
    return nullptr;
  }
  size_t charLen = strlen(source);
  char* target = new char[charLen + 1];
  strncpy(target, source, charLen + 1);
  return target;
}

constexpr guint16 kKeyCodeKeyA = 0x26u;
constexpr guint16 kKeyCodeKeyB = 0x38u;
constexpr guint16 kKeyCodeDigit1 = 0x0au;
constexpr guint16 kKeyCodeMinus = 0x14u;

static constexpr char kKeyEventChannelName[] = "flutter/keyevent";
static constexpr char kKeyboardChannelName[] = "flutter/keyboard";
static constexpr char kGetKeyboardStateMethod[] = "getKeyboardState";
static constexpr uint64_t kMockPhysicalKey = 42;
static constexpr uint64_t kMockLogicalKey = 42;

G_BEGIN_DECLS

G_DECLARE_FINAL_TYPE(FlMockViewDelegate,
                     fl_mock_view_delegate,
                     FL,
                     MOCK_VIEW_DELEGATE,
                     GObject);

G_DECLARE_FINAL_TYPE(FlMockKeyBinaryMessenger,
                     fl_mock_key_binary_messenger,
                     FL,
                     MOCK_KEY_BINARY_MESSENGER,
                     GObject)

G_END_DECLS

MATCHER_P(MethodSuccessResponse, result, "") {
  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodResponse) response =
      fl_method_codec_decode_response(FL_METHOD_CODEC(codec), arg, nullptr);
  fl_method_response_get_result(response, nullptr);
  if (fl_value_equal(fl_method_response_get_result(response, nullptr),
                     result)) {
    return true;
  }
  *result_listener << ::testing::PrintToString(response);
  return false;
}

/***** FlMockKeyBinaryMessenger *****/
/* Mock a binary messenger that only processes messages from the embedding on
 * the key event channel, and does so according to the callback set by
 * fl_mock_key_binary_messenger_set_callback_handler */

struct _FlMockKeyBinaryMessenger {
  GObject parent_instance;
};

struct FlMockKeyBinaryMessengerPrivate {
  ChannelCallHandler callback_handler;
};

static void fl_mock_key_binary_messenger_iface_init(
    FlBinaryMessengerInterface* iface);

G_DEFINE_TYPE_WITH_CODE(
    FlMockKeyBinaryMessenger,
    fl_mock_key_binary_messenger,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(fl_binary_messenger_get_type(),
                          fl_mock_key_binary_messenger_iface_init);
    G_ADD_PRIVATE(FlMockKeyBinaryMessenger))

#define FL_MOCK_KEY_BINARY_MESSENGER_GET_PRIVATE(obj)    \
  static_cast<FlMockKeyBinaryMessengerPrivate*>(         \
      fl_mock_key_binary_messenger_get_instance_private( \
          FL_MOCK_KEY_BINARY_MESSENGER(obj)))

static void fl_mock_key_binary_messenger_init(FlMockKeyBinaryMessenger* self) {
  FlMockKeyBinaryMessengerPrivate* priv =
      FL_MOCK_KEY_BINARY_MESSENGER_GET_PRIVATE(self);
  new (priv) FlMockKeyBinaryMessengerPrivate();
}

static void fl_mock_key_binary_messenger_dispose(GObject* object) {
  FL_MOCK_KEY_BINARY_MESSENGER_GET_PRIVATE(object)
      ->~FlMockKeyBinaryMessengerPrivate();

  G_OBJECT_CLASS(fl_mock_key_binary_messenger_parent_class)->dispose(object);
}

static void fl_mock_key_binary_messenger_class_init(
    FlMockKeyBinaryMessengerClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fl_mock_key_binary_messenger_dispose;
}

static void fl_mock_key_binary_messenger_send_on_channel(
    FlBinaryMessenger* messenger,
    const gchar* channel,
    GBytes* message,
    GCancellable* cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data) {
  FlMockKeyBinaryMessenger* self = FL_MOCK_KEY_BINARY_MESSENGER(messenger);

  if (callback != nullptr) {
    EXPECT_STREQ(channel, kKeyEventChannelName);
    FL_MOCK_KEY_BINARY_MESSENGER_GET_PRIVATE(self)->callback_handler(
        [self, cancellable, callback, user_data](bool handled) {
          g_autoptr(GTask) task =
              g_task_new(self, cancellable, callback, user_data);
          g_autoptr(FlValue) result = fl_value_new_map();
          fl_value_set_string_take(result, "handled",
                                   fl_value_new_bool(handled));
          g_autoptr(FlJsonMessageCodec) codec = fl_json_message_codec_new();
          g_autoptr(GError) error = nullptr;
          GBytes* data = fl_message_codec_encode_message(
              FL_MESSAGE_CODEC(codec), result, &error);

          g_task_return_pointer(
              task, data, reinterpret_cast<GDestroyNotify>(g_bytes_unref));
        });
  }
}

static GBytes* fl_mock_key_binary_messenger_send_on_channel_finish(
    FlBinaryMessenger* messenger,
    GAsyncResult* result,
    GError** error) {
  return static_cast<GBytes*>(g_task_propagate_pointer(G_TASK(result), error));
}

static void fl_mock_binary_messenger_resize_channel(
    FlBinaryMessenger* messenger,
    const gchar* channel,
    int64_t new_size) {
  // Mock implementation. Do nothing.
}

static void fl_mock_binary_messenger_set_warns_on_channel_overflow(
    FlBinaryMessenger* messenger,
    const gchar* channel,
    bool warns) {
  // Mock implementation. Do nothing.
}

static void fl_mock_key_binary_messenger_iface_init(
    FlBinaryMessengerInterface* iface) {
  iface->set_message_handler_on_channel =
      [](FlBinaryMessenger* messenger, const gchar* channel,
         FlBinaryMessengerMessageHandler handler, gpointer user_data,
         GDestroyNotify destroy_notify) {
        EXPECT_STREQ(channel, kKeyEventChannelName);
        // No need to mock. The key event channel expects no incoming messages
        // from the framework.
      };
  iface->send_response = [](FlBinaryMessenger* messenger,
                            FlBinaryMessengerResponseHandle* response_handle,
                            GBytes* response, GError** error) -> gboolean {
    // The key event channel expects no incoming messages from the framework,
    // hence no responses either.
    g_return_val_if_reached(TRUE);
    return TRUE;
  };
  iface->send_on_channel = fl_mock_key_binary_messenger_send_on_channel;
  iface->send_on_channel_finish =
      fl_mock_key_binary_messenger_send_on_channel_finish;
  iface->resize_channel = fl_mock_binary_messenger_resize_channel;
  iface->set_warns_on_channel_overflow =
      fl_mock_binary_messenger_set_warns_on_channel_overflow;
}

static FlMockKeyBinaryMessenger* fl_mock_key_binary_messenger_new() {
  FlMockKeyBinaryMessenger* self = FL_MOCK_KEY_BINARY_MESSENGER(
      g_object_new(fl_mock_key_binary_messenger_get_type(), NULL));

  // Added to stop compiler complaining about an unused function.
  FL_IS_MOCK_KEY_BINARY_MESSENGER(self);

  return self;
}

static void fl_mock_key_binary_messenger_set_callback_handler(
    FlMockKeyBinaryMessenger* self,
    ChannelCallHandler handler) {
  FL_MOCK_KEY_BINARY_MESSENGER_GET_PRIVATE(self)->callback_handler =
      std::move(handler);
}

/***** FlMockViewDelegate *****/

struct _FlMockViewDelegate {
  GObject parent_instance;
};

struct FlMockViewDelegatePrivate {
  FlMockKeyBinaryMessenger* messenger;
  EmbedderCallHandler embedder_handler;
  bool text_filter_result;
  RedispatchHandler redispatch_handler;
};

static void fl_mock_view_keyboard_delegate_iface_init(
    FlKeyboardViewDelegateInterface* iface);

G_DEFINE_TYPE_WITH_CODE(
    FlMockViewDelegate,
    fl_mock_view_delegate,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(fl_keyboard_view_delegate_get_type(),
                          fl_mock_view_keyboard_delegate_iface_init);
    G_ADD_PRIVATE(FlMockViewDelegate))

#define FL_MOCK_VIEW_DELEGATE_GET_PRIVATE(obj) \
  static_cast<FlMockViewDelegatePrivate*>(     \
      fl_mock_view_delegate_get_instance_private(FL_MOCK_VIEW_DELEGATE(obj)))

static void fl_mock_view_delegate_init(FlMockViewDelegate* self) {
  FlMockViewDelegatePrivate* priv = FL_MOCK_VIEW_DELEGATE_GET_PRIVATE(self);
  new (priv) FlMockViewDelegatePrivate();
}

static void fl_mock_view_delegate_dispose(GObject* object) {
  FlMockViewDelegatePrivate* priv = FL_MOCK_VIEW_DELEGATE_GET_PRIVATE(object);

  FL_MOCK_VIEW_DELEGATE_GET_PRIVATE(object)->~FlMockViewDelegatePrivate();
  g_clear_object(&priv->messenger);

  G_OBJECT_CLASS(fl_mock_view_delegate_parent_class)->dispose(object);
}

static void fl_mock_view_delegate_class_init(FlMockViewDelegateClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fl_mock_view_delegate_dispose;
}

static void fl_mock_view_keyboard_send_key_event(
    FlKeyboardViewDelegate* view_delegate,
    const FlutterKeyEvent* event,
    FlutterKeyEventCallback callback,
    void* user_data) {
  FlMockViewDelegatePrivate* priv =
      FL_MOCK_VIEW_DELEGATE_GET_PRIVATE(view_delegate);
  priv->embedder_handler(event, [callback, user_data](bool handled) {
    if (callback != nullptr) {
      callback(handled, user_data);
    }
  });
}

static gboolean fl_mock_view_keyboard_text_filter_key_press(
    FlKeyboardViewDelegate* view_delegate,
    FlKeyEvent* event) {
  FlMockViewDelegatePrivate* priv =
      FL_MOCK_VIEW_DELEGATE_GET_PRIVATE(view_delegate);
  return priv->text_filter_result;
}

static FlBinaryMessenger* fl_mock_view_keyboard_get_messenger(
    FlKeyboardViewDelegate* view_delegate) {
  FlMockViewDelegatePrivate* priv =
      FL_MOCK_VIEW_DELEGATE_GET_PRIVATE(view_delegate);
  return FL_BINARY_MESSENGER(priv->messenger);
}

static void fl_mock_view_keyboard_redispatch_event(
    FlKeyboardViewDelegate* view_delegate,
    FlKeyEvent* event) {
  FlMockViewDelegatePrivate* priv =
      FL_MOCK_VIEW_DELEGATE_GET_PRIVATE(view_delegate);
  if (priv->redispatch_handler) {
    priv->redispatch_handler(event);
  }
}

static GHashTable* fl_mock_view_keyboard_get_keyboard_state(
    FlKeyboardViewDelegate* view_delegate) {
  GHashTable* result = g_hash_table_new(g_direct_hash, g_direct_equal);
  g_hash_table_insert(result, reinterpret_cast<gpointer>(kMockPhysicalKey),
                      reinterpret_cast<gpointer>(kMockLogicalKey));

  return result;
}

static void fl_mock_view_keyboard_delegate_iface_init(
    FlKeyboardViewDelegateInterface* iface) {
  iface->send_key_event = fl_mock_view_keyboard_send_key_event;
  iface->text_filter_key_press = fl_mock_view_keyboard_text_filter_key_press;
  iface->get_messenger = fl_mock_view_keyboard_get_messenger;
  iface->redispatch_event = fl_mock_view_keyboard_redispatch_event;
  iface->get_keyboard_state = fl_mock_view_keyboard_get_keyboard_state;
}

static FlMockViewDelegate* fl_mock_view_delegate_new() {
  FlMockViewDelegate* self = FL_MOCK_VIEW_DELEGATE(
      g_object_new(fl_mock_view_delegate_get_type(), nullptr));

  // Added to stop compiler complaining about an unused function.
  FL_IS_MOCK_VIEW_DELEGATE(self);

  FlMockViewDelegatePrivate* priv = FL_MOCK_VIEW_DELEGATE_GET_PRIVATE(self);
  priv->messenger = fl_mock_key_binary_messenger_new();

  return self;
}

static void fl_mock_view_set_embedder_handler(FlMockViewDelegate* self,
                                              EmbedderCallHandler handler) {
  FlMockViewDelegatePrivate* priv = FL_MOCK_VIEW_DELEGATE_GET_PRIVATE(self);
  priv->embedder_handler = std::move(handler);
}

static void fl_mock_view_set_text_filter_result(FlMockViewDelegate* self,
                                                bool result) {
  FlMockViewDelegatePrivate* priv = FL_MOCK_VIEW_DELEGATE_GET_PRIVATE(self);
  priv->text_filter_result = result;
}

static void fl_mock_view_set_redispatch_handler(FlMockViewDelegate* self,
                                                RedispatchHandler handler) {
  FlMockViewDelegatePrivate* priv = FL_MOCK_VIEW_DELEGATE_GET_PRIVATE(self);
  priv->redispatch_handler = std::move(handler);
}

/***** End FlMockViewDelegate *****/

class KeyboardTester {
 public:
  KeyboardTester() {
    ::testing::NiceMock<flutter::testing::MockBinaryMessenger> messenger;

    view_ = fl_mock_view_delegate_new();
    respondToEmbedderCallsWith(false);
    respondToChannelCallsWith(false);
    respondToTextInputWith(false);

    handler_ =
        fl_keyboard_handler_new(messenger, FL_KEYBOARD_VIEW_DELEGATE(view_));
  }

  ~KeyboardTester() {
    g_clear_object(&view_);
    g_clear_object(&handler_);
    g_clear_pointer(&redispatched_events_, g_ptr_array_unref);
  }

  FlKeyboardHandler* handler() { return handler_; }

  // Block until all GdkMainLoop messages are processed, which is basically
  // used only for channel messages.
  void flushChannelMessages() {
    GMainLoop* loop = g_main_loop_new(nullptr, 0);
    g_idle_add(_flushChannelMessagesCb, loop);
    g_main_loop_run(loop);
  }

  // Dispatch each of the given events, expect their results to be false
  // (unhandled), and clear the event array.
  //
  // Returns the number of events redispatched. If any result is unexpected
  // (handled), return a minus number `-x` instead, where `x` is the index of
  // the first unexpected redispatch.
  int redispatchEventsAndClear(GPtrArray* events) {
    guint event_count = events->len;
    int first_error = -1;
    during_redispatch_ = true;
    for (guint event_id = 0; event_id < event_count; event_id += 1) {
      FlKeyEvent* event = FL_KEY_EVENT(g_ptr_array_index(events, event_id));
      bool handled = fl_keyboard_handler_handle_event(handler_, event);
      EXPECT_FALSE(handled);
      if (handled) {
        first_error = first_error == -1 ? event_id : first_error;
      }
    }
    during_redispatch_ = false;
    g_ptr_array_set_size(events, 0);
    return first_error < 0 ? event_count : -first_error;
  }

  void respondToEmbedderCallsWith(bool response) {
    fl_mock_view_set_embedder_handler(
        view_, [response, this](const FlutterKeyEvent* event,
                                const AsyncKeyCallback& callback) {
          EXPECT_FALSE(during_redispatch_);
          callback(response);
        });
  }

  void recordEmbedderCallsTo(std::vector<CallRecord>& storage) {
    fl_mock_view_set_embedder_handler(
        view_, [&storage, this](const FlutterKeyEvent* event,
                                AsyncKeyCallback callback) {
          EXPECT_FALSE(during_redispatch_);
          auto new_event = std::make_unique<FlutterKeyEvent>(*event);
          char* new_event_character = cloneString(event->character);
          new_event->character = new_event_character;
          storage.push_back(CallRecord{
              .type = CallRecord::kKeyCallEmbedder,
              .callback = std::move(callback),
              .event = std::move(new_event),
              .event_character = std::unique_ptr<char[]>(new_event_character),
          });
        });
  }

  void respondToEmbedderCallsWithAndRecordsTo(
      bool response,
      std::vector<CallRecord>& storage) {
    fl_mock_view_set_embedder_handler(
        view_, [&storage, response, this](const FlutterKeyEvent* event,
                                          const AsyncKeyCallback& callback) {
          EXPECT_FALSE(during_redispatch_);
          auto new_event = std::make_unique<FlutterKeyEvent>(*event);
          char* new_event_character = cloneString(event->character);
          new_event->character = new_event_character;
          storage.push_back(CallRecord{
              .type = CallRecord::kKeyCallEmbedder,
              .event = std::move(new_event),
              .event_character = std::unique_ptr<char[]>(new_event_character),
          });
          callback(response);
        });
  }

  void respondToChannelCallsWith(bool response) {
    FlMockViewDelegatePrivate* priv = FL_MOCK_VIEW_DELEGATE_GET_PRIVATE(view_);

    fl_mock_key_binary_messenger_set_callback_handler(
        priv->messenger, [response, this](const AsyncKeyCallback& callback) {
          EXPECT_FALSE(during_redispatch_);
          callback(response);
        });
  }

  void recordChannelCallsTo(std::vector<CallRecord>& storage) {
    FlMockViewDelegatePrivate* priv = FL_MOCK_VIEW_DELEGATE_GET_PRIVATE(view_);

    fl_mock_key_binary_messenger_set_callback_handler(
        priv->messenger, [&storage, this](AsyncKeyCallback callback) {
          EXPECT_FALSE(during_redispatch_);
          storage.push_back(CallRecord{
              .type = CallRecord::kKeyCallChannel,
              .callback = std::move(callback),
          });
        });
  }

  void respondToTextInputWith(bool response) {
    fl_mock_view_set_text_filter_result(view_, response);
  }

  void recordRedispatchedEventsTo(GPtrArray* storage) {
    redispatched_events_ = g_ptr_array_ref(storage);
    fl_mock_view_set_redispatch_handler(view_, [this](FlKeyEvent* key) {
      g_ptr_array_add(redispatched_events_, g_object_ref(key));
    });
  }

 private:
  FlMockViewDelegate* view_;
  FlKeyboardHandler* handler_ = nullptr;
  GPtrArray* redispatched_events_ = nullptr;
  bool during_redispatch_ = false;

  static gboolean _flushChannelMessagesCb(gpointer data) {
    g_autoptr(GMainLoop) loop = reinterpret_cast<GMainLoop*>(data);
    g_main_loop_quit(loop);
    return FALSE;
  }
};

// Make sure that the keyboard can be disposed without crashes when there are
// unresolved pending events.
TEST(FlKeyboardHandlerTest, DisposeWithUnresolvedPends) {
  KeyboardTester tester;
  std::vector<CallRecord> call_records;

  // Record calls so that they aren't responded.
  tester.recordEmbedderCallsTo(call_records);
  g_autoptr(FlKeyEvent) event1 = fl_key_event_new(
      0, TRUE, kKeyCodeKeyA, GDK_KEY_a, static_cast<GdkModifierType>(0), 0);
  fl_keyboard_handler_handle_event(tester.handler(), event1);

  tester.respondToEmbedderCallsWith(true);
  g_autoptr(FlKeyEvent) event2 = fl_key_event_new(
      0, FALSE, kKeyCodeKeyA, GDK_KEY_a, static_cast<GdkModifierType>(0), 0);
  fl_keyboard_handler_handle_event(tester.handler(), event2);

  tester.flushChannelMessages();

  // Passes if the cleanup does not crash.
}

TEST(FlKeyboardHandlerTest, SingleDelegateWithAsyncResponds) {
  KeyboardTester tester;
  std::vector<CallRecord> call_records;
  g_autoptr(GPtrArray) redispatched =
      g_ptr_array_new_with_free_func(g_object_unref);

  gboolean handler_handled = false;

  /// Test 1: One event that is handled by the framework
  tester.recordEmbedderCallsTo(call_records);
  tester.recordRedispatchedEventsTo(redispatched);

  // Dispatch a key event
  g_autoptr(FlKeyEvent) event1 = fl_key_event_new(
      0, TRUE, kKeyCodeKeyA, GDK_KEY_a, static_cast<GdkModifierType>(0), 0);
  handler_handled = fl_keyboard_handler_handle_event(tester.handler(), event1);
  tester.flushChannelMessages();
  EXPECT_EQ(handler_handled, true);
  EXPECT_EQ(redispatched->len, 0u);
  EXPECT_EQ(call_records.size(), 1u);
  EXPECT_KEY_EVENT(call_records[0], kFlutterKeyEventTypeDown, kPhysicalKeyA,
                   kLogicalKeyA, "a", false);

  call_records[0].callback(true);
  tester.flushChannelMessages();
  EXPECT_EQ(redispatched->len, 0u);
  EXPECT_TRUE(fl_keyboard_handler_is_state_clear(tester.handler()));
  call_records.clear();

  /// Test 2: Two events that are unhandled by the framework
  g_autoptr(FlKeyEvent) event2 = fl_key_event_new(
      0, FALSE, kKeyCodeKeyA, GDK_KEY_a, static_cast<GdkModifierType>(0), 0);
  handler_handled = fl_keyboard_handler_handle_event(tester.handler(), event2);
  tester.flushChannelMessages();
  EXPECT_EQ(handler_handled, true);
  EXPECT_EQ(redispatched->len, 0u);
  EXPECT_EQ(call_records.size(), 1u);
  EXPECT_KEY_EVENT(call_records[0], kFlutterKeyEventTypeUp, kPhysicalKeyA,
                   kLogicalKeyA, nullptr, false);

  // Dispatch another key event
  g_autoptr(FlKeyEvent) event3 = fl_key_event_new(
      0, TRUE, kKeyCodeKeyB, GDK_KEY_b, static_cast<GdkModifierType>(0), 0);
  handler_handled = fl_keyboard_handler_handle_event(tester.handler(), event3);
  tester.flushChannelMessages();
  EXPECT_EQ(handler_handled, true);
  EXPECT_EQ(redispatched->len, 0u);
  EXPECT_EQ(call_records.size(), 2u);
  EXPECT_KEY_EVENT(call_records[1], kFlutterKeyEventTypeDown, kPhysicalKeyB,
                   kLogicalKeyB, "b", false);

  // Resolve the second event first to test out-of-order response
  call_records[1].callback(false);
  EXPECT_EQ(redispatched->len, 1u);
  EXPECT_EQ(
      fl_key_event_get_keyval(FL_KEY_EVENT(g_ptr_array_index(redispatched, 0))),
      0x62u);
  call_records[0].callback(false);
  tester.flushChannelMessages();
  EXPECT_EQ(redispatched->len, 2u);
  EXPECT_EQ(
      fl_key_event_get_keyval(FL_KEY_EVENT(g_ptr_array_index(redispatched, 1))),
      0x61u);

  EXPECT_FALSE(fl_keyboard_handler_is_state_clear(tester.handler()));
  call_records.clear();

  // Resolve redispatches
  EXPECT_EQ(tester.redispatchEventsAndClear(redispatched), 2);
  tester.flushChannelMessages();
  EXPECT_EQ(call_records.size(), 0u);
  EXPECT_TRUE(fl_keyboard_handler_is_state_clear(tester.handler()));

  /// Test 3: Dispatch the same event again to ensure that prevention from
  /// redispatching only works once.
  g_autoptr(FlKeyEvent) event4 = fl_key_event_new(
      0, FALSE, kKeyCodeKeyA, GDK_KEY_a, static_cast<GdkModifierType>(0), 0);
  handler_handled = fl_keyboard_handler_handle_event(tester.handler(), event4);
  tester.flushChannelMessages();
  EXPECT_EQ(handler_handled, true);
  EXPECT_EQ(redispatched->len, 0u);
  EXPECT_EQ(call_records.size(), 1u);

  call_records[0].callback(true);
  EXPECT_TRUE(fl_keyboard_handler_is_state_clear(tester.handler()));
}

TEST(FlKeyboardHandlerTest, SingleDelegateWithSyncResponds) {
  KeyboardTester tester;
  gboolean handler_handled = false;
  std::vector<CallRecord> call_records;
  g_autoptr(GPtrArray) redispatched =
      g_ptr_array_new_with_free_func(g_object_unref);

  /// Test 1: One event that is handled by the framework
  tester.respondToEmbedderCallsWithAndRecordsTo(true, call_records);
  tester.recordRedispatchedEventsTo(redispatched);

  // Dispatch a key event
  g_autoptr(FlKeyEvent) event1 = fl_key_event_new(
      0, TRUE, kKeyCodeKeyA, GDK_KEY_a, static_cast<GdkModifierType>(0), 0);
  handler_handled = fl_keyboard_handler_handle_event(tester.handler(), event1);
  tester.flushChannelMessages();
  EXPECT_EQ(handler_handled, true);
  EXPECT_EQ(call_records.size(), 1u);
  EXPECT_KEY_EVENT(call_records[0], kFlutterKeyEventTypeDown, kPhysicalKeyA,
                   kLogicalKeyA, "a", false);
  EXPECT_EQ(redispatched->len, 0u);
  call_records.clear();

  EXPECT_TRUE(fl_keyboard_handler_is_state_clear(tester.handler()));
  g_ptr_array_set_size(redispatched, 0);

  /// Test 2: An event unhandled by the framework
  tester.respondToEmbedderCallsWithAndRecordsTo(false, call_records);
  g_autoptr(FlKeyEvent) event2 = fl_key_event_new(
      0, FALSE, kKeyCodeKeyA, GDK_KEY_a, static_cast<GdkModifierType>(0), 0);
  handler_handled = fl_keyboard_handler_handle_event(tester.handler(), event2);
  tester.flushChannelMessages();
  EXPECT_EQ(handler_handled, true);
  EXPECT_EQ(call_records.size(), 1u);
  EXPECT_KEY_EVENT(call_records[0], kFlutterKeyEventTypeUp, kPhysicalKeyA,
                   kLogicalKeyA, nullptr, false);
  EXPECT_EQ(redispatched->len, 1u);
  call_records.clear();

  EXPECT_FALSE(fl_keyboard_handler_is_state_clear(tester.handler()));

  EXPECT_EQ(tester.redispatchEventsAndClear(redispatched), 1);
  EXPECT_EQ(call_records.size(), 0u);

  EXPECT_TRUE(fl_keyboard_handler_is_state_clear(tester.handler()));
}

TEST(FlKeyboardHandlerTest, WithTwoAsyncDelegates) {
  KeyboardTester tester;
  std::vector<CallRecord> call_records;
  g_autoptr(GPtrArray) redispatched =
      g_ptr_array_new_with_free_func(g_object_unref);

  gboolean handler_handled = false;

  tester.recordEmbedderCallsTo(call_records);
  tester.recordChannelCallsTo(call_records);
  tester.recordRedispatchedEventsTo(redispatched);

  /// Test 1: One delegate responds true, the other false

  g_autoptr(FlKeyEvent) event1 = fl_key_event_new(
      0, TRUE, kKeyCodeKeyA, GDK_KEY_a, static_cast<GdkModifierType>(0), 0);
  handler_handled = fl_keyboard_handler_handle_event(tester.handler(), event1);

  EXPECT_EQ(handler_handled, true);
  EXPECT_EQ(redispatched->len, 0u);
  EXPECT_EQ(call_records.size(), 2u);

  EXPECT_EQ(call_records[0].type, CallRecord::kKeyCallEmbedder);
  EXPECT_EQ(call_records[1].type, CallRecord::kKeyCallChannel);

  call_records[0].callback(true);
  call_records[1].callback(false);
  tester.flushChannelMessages();
  EXPECT_EQ(redispatched->len, 0u);

  EXPECT_TRUE(fl_keyboard_handler_is_state_clear(tester.handler()));
  call_records.clear();

  /// Test 2: All delegates respond false
  g_autoptr(FlKeyEvent) event2 = fl_key_event_new(
      0, FALSE, kKeyCodeKeyA, GDK_KEY_a, static_cast<GdkModifierType>(0), 0);
  handler_handled = fl_keyboard_handler_handle_event(tester.handler(), event2);

  EXPECT_EQ(handler_handled, true);
  EXPECT_EQ(redispatched->len, 0u);
  EXPECT_EQ(call_records.size(), 2u);

  EXPECT_EQ(call_records[0].type, CallRecord::kKeyCallEmbedder);
  EXPECT_EQ(call_records[1].type, CallRecord::kKeyCallChannel);

  call_records[0].callback(false);
  call_records[1].callback(false);

  call_records.clear();

  // Resolve redispatch
  tester.flushChannelMessages();
  EXPECT_EQ(redispatched->len, 1u);
  EXPECT_EQ(tester.redispatchEventsAndClear(redispatched), 1);
  EXPECT_EQ(call_records.size(), 0u);

  EXPECT_TRUE(fl_keyboard_handler_is_state_clear(tester.handler()));
}

TEST(FlKeyboardHandlerTest, TextInputHandlerReturnsFalse) {
  KeyboardTester tester;
  g_autoptr(GPtrArray) redispatched =
      g_ptr_array_new_with_free_func(g_object_unref);
  gboolean handler_handled = false;
  tester.recordRedispatchedEventsTo(redispatched);
  tester.respondToTextInputWith(false);

  // Dispatch a key event.
  g_autoptr(FlKeyEvent) event = fl_key_event_new(
      0, TRUE, kKeyCodeKeyA, GDK_KEY_a, static_cast<GdkModifierType>(0), 0);
  handler_handled = fl_keyboard_handler_handle_event(tester.handler(), event);
  tester.flushChannelMessages();
  EXPECT_EQ(handler_handled, true);
  // The event was redispatched because no one handles it.
  EXPECT_EQ(redispatched->len, 1u);

  // Resolve redispatched event.
  EXPECT_EQ(tester.redispatchEventsAndClear(redispatched), 1);

  EXPECT_TRUE(fl_keyboard_handler_is_state_clear(tester.handler()));
}

TEST(FlKeyboardHandlerTest, TextInputHandlerReturnsTrue) {
  KeyboardTester tester;
  g_autoptr(GPtrArray) redispatched =
      g_ptr_array_new_with_free_func(g_object_unref);
  gboolean handler_handled = false;
  tester.recordRedispatchedEventsTo(redispatched);
  tester.respondToTextInputWith(true);

  // Dispatch a key event.
  g_autoptr(FlKeyEvent) event = fl_key_event_new(
      0, TRUE, kKeyCodeKeyA, GDK_KEY_a, static_cast<GdkModifierType>(0), 0);
  handler_handled = fl_keyboard_handler_handle_event(tester.handler(), event);
  tester.flushChannelMessages();
  EXPECT_EQ(handler_handled, true);
  // The event was not redispatched because handler handles it.
  EXPECT_EQ(redispatched->len, 0u);

  EXPECT_TRUE(fl_keyboard_handler_is_state_clear(tester.handler()));
}

TEST(FlKeyboardHandlerTest, CorrectLogicalKeyForLayouts) {
  ::testing::NiceMock<flutter::testing::MockKeymap> keymap;
  KeyboardTester tester;

  ON_CALL(keymap, gdk_keymap_lookup_key).WillByDefault(::testing::Return(0));

  std::vector<CallRecord> call_records;
  tester.recordEmbedderCallsTo(call_records);

  auto sendTap = [&](guint8 keycode, guint keyval, guint8 group) {
    g_autoptr(FlKeyEvent) event1 = fl_key_event_new(
        0, TRUE, keycode, keyval, static_cast<GdkModifierType>(0), group);
    fl_keyboard_handler_handle_event(tester.handler(), event1);
    g_autoptr(FlKeyEvent) event2 = fl_key_event_new(
        0, FALSE, keycode, keyval, static_cast<GdkModifierType>(0), group);
    fl_keyboard_handler_handle_event(tester.handler(), event2);
  };

  /* US keyboard layout */

  sendTap(kKeyCodeKeyA, GDK_KEY_a, 0);  // KeyA
  VERIFY_DOWN(kLogicalKeyA, "a");

  sendTap(kKeyCodeKeyA, GDK_KEY_A, 0);  // Shift-KeyA
  VERIFY_DOWN(kLogicalKeyA, "A");

  sendTap(kKeyCodeDigit1, GDK_KEY_1, 0);  // Digit1
  VERIFY_DOWN(kLogicalDigit1, "1");

  sendTap(kKeyCodeDigit1, GDK_KEY_exclam, 0);  // Shift-Digit1
  VERIFY_DOWN(kLogicalDigit1, "!");

  sendTap(kKeyCodeMinus, GDK_KEY_minus, 0);  // Minus
  VERIFY_DOWN(kLogicalMinus, "-");

  sendTap(kKeyCodeMinus, GDK_KEY_underscore, 0);  // Shift-Minus
  VERIFY_DOWN(kLogicalUnderscore, "_");
}

TEST(FlKeyboardHandlerTest, SynthesizeModifiersIfNeeded) {
  KeyboardTester tester;
  std::vector<CallRecord> call_records;
  tester.recordEmbedderCallsTo(call_records);

  auto verifyModifierIsSynthesized = [&](GdkModifierType mask,
                                         uint64_t physical, uint64_t logical) {
    // Modifier is pressed.
    guint state = mask;
    fl_keyboard_handler_sync_modifier_if_needed(tester.handler(), state, 1000);
    EXPECT_EQ(call_records.size(), 1u);
    EXPECT_KEY_EVENT(call_records[0], kFlutterKeyEventTypeDown, physical,
                     logical, NULL, true);
    // Modifier is released.
    state = state ^ mask;
    fl_keyboard_handler_sync_modifier_if_needed(tester.handler(), state, 1001);
    EXPECT_EQ(call_records.size(), 2u);
    EXPECT_KEY_EVENT(call_records[1], kFlutterKeyEventTypeUp, physical, logical,
                     NULL, true);
    call_records.clear();
  };

  // No modifiers pressed.
  guint state = 0;
  fl_keyboard_handler_sync_modifier_if_needed(tester.handler(), state, 1000);
  EXPECT_EQ(call_records.size(), 0u);
  call_records.clear();

  // Press and release each modifier once.
  verifyModifierIsSynthesized(GDK_CONTROL_MASK, kPhysicalControlLeft,
                              kLogicalControlLeft);
  verifyModifierIsSynthesized(GDK_META_MASK, kPhysicalMetaLeft,
                              kLogicalMetaLeft);
  verifyModifierIsSynthesized(GDK_MOD1_MASK, kPhysicalAltLeft, kLogicalAltLeft);
  verifyModifierIsSynthesized(GDK_SHIFT_MASK, kPhysicalShiftLeft,
                              kLogicalShiftLeft);
}

TEST(FlKeyboardHandlerTest, GetPressedState) {
  KeyboardTester tester;
  tester.respondToTextInputWith(true);

  // Dispatch a key event.
  g_autoptr(FlKeyEvent) event = fl_key_event_new(
      0, TRUE, kKeyCodeKeyA, GDK_KEY_a, static_cast<GdkModifierType>(0), 0);
  fl_keyboard_handler_handle_event(tester.handler(), event);

  GHashTable* pressedState =
      fl_keyboard_handler_get_pressed_state(tester.handler());
  EXPECT_EQ(g_hash_table_size(pressedState), 1u);

  gpointer physical_key =
      g_hash_table_lookup(pressedState, uint64_to_gpointer(kPhysicalKeyA));
  EXPECT_EQ(gpointer_to_uint64(physical_key), kLogicalKeyA);
}

TEST(FlKeyboardHandlerTest, KeyboardChannelGetPressedState) {
  ::testing::NiceMock<flutter::testing::MockBinaryMessenger> messenger;

  g_autoptr(FlKeyboardHandler) handler = fl_keyboard_handler_new(
      messenger, FL_KEYBOARD_VIEW_DELEGATE(fl_mock_view_delegate_new()));
  EXPECT_NE(handler, nullptr);

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(GBytes) message = fl_method_codec_encode_method_call(
      FL_METHOD_CODEC(codec), kGetKeyboardStateMethod, nullptr, nullptr);

  g_autoptr(FlValue) response = fl_value_new_map();
  fl_value_set_take(response, fl_value_new_int(kMockPhysicalKey),
                    fl_value_new_int(kMockLogicalKey));
  EXPECT_CALL(messenger,
              fl_binary_messenger_send_response(
                  ::testing::Eq<FlBinaryMessenger*>(messenger), ::testing::_,
                  MethodSuccessResponse(response), ::testing::_))
      .WillOnce(::testing::Return(true));

  messenger.ReceiveMessage(kKeyboardChannelName, message);
}

}  // namespace
