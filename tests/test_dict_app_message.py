"""Integration tests for #1: Dictionary, AppSync, AppMessage (0x0030), AppRunState (0x0034), and --proto-fd."""

from __future__ import annotations

import pathlib
import select
import shutil
import socket
import struct
import subprocess
import tempfile
import unittest

from tests.pbw_builder import build_pebble_app_bin

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
RUNNER = REPO_ROOT / "build" / "pebble-runner"
QEMU_ARM = shutil.which("qemu-arm-static") or shutil.which("qemu-arm") or "qemu-arm-static"

EP_APP_MESSAGE = 0x0030
EP_APP_RUN_STATE = 0x0034

APP_MSG_PUSH = 0x01
APP_MSG_ACK = 0xFF
APP_MSG_NACK = 0x7F

RUN_STATE_START = 0x01
RUN_STATE_STOP = 0x02
RUN_STATE_REQUEST = 0x03

COMMON_PEBBLE_DECLS = r"""
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Window Window;
typedef void (*WindowHandler)(Window *);
typedef struct { WindowHandler load, appear, disappear, unload; } WindowHandlers;
typedef struct AppTimer AppTimer;
typedef void (*AppTimerCallback)(void *data);

extern Window *window_create(void);
extern void window_destroy(Window *);
extern void window_set_window_handlers(Window *, WindowHandlers);
extern void window_stack_push(Window *, bool);
extern Window *window_stack_pop(bool);
extern void app_event_loop(void);
extern AppTimer *app_timer_register(uint32_t timeout_ms, AppTimerCallback cb, void *data);

typedef enum {
    DICT_OK = 0,
    DICT_NOT_ENOUGH_STORAGE = 1 << 1,
    DICT_INVALID_ARGS = 1 << 2,
    DICT_INTERNAL_INCONSISTENCY = 1 << 3,
    DICT_MALLOC_FAILED = 1 << 4,
} DictionaryResult;

typedef enum {
    TUPLE_BYTE_ARRAY = 0,
    TUPLE_CSTRING = 1,
    TUPLE_UINT = 2,
    TUPLE_INT = 3,
} TupleType;

typedef struct __attribute__((__packed__)) {
    uint32_t key;
    TupleType type : 8;
    uint16_t length;
    union {
        uint8_t data[0];
        char cstring[0];
        uint8_t uint8;
        uint16_t uint16;
        uint32_t uint32;
        int8_t int8;
        int16_t int16;
        int32_t int32;
    } value[];
} Tuple;

typedef struct __attribute__((__packed__)) {
    uint8_t count;
    Tuple head[];
} Dictionary;

typedef struct {
    Dictionary *dictionary;
    const void *end;
    Tuple *cursor;
} DictionaryIterator;

typedef struct {
    TupleType type;
    uint32_t key;
    union {
        struct { const uint8_t *data; uint16_t length; } bytes;
        struct { const char *data; uint16_t length; } cstring;
        struct { uint32_t storage; uint16_t width; } integer;
    };
} Tuplet;

typedef void (*DictionarySerializeCallback)(const uint8_t *const data, const uint16_t size, void *context);
typedef void (*DictionaryKeyUpdatedCallback)(const uint32_t key, const Tuple *new_tuple, const Tuple *old_tuple, void *context);

extern uint32_t dict_calc_buffer_size(const uint8_t tuple_count, ...);
extern uint32_t dict_calc_buffer_size_from_tuplets(const Tuplet *const tuplets, const uint8_t tuplets_count);
extern uint32_t dict_size(DictionaryIterator *iter);
extern DictionaryResult dict_write_begin(DictionaryIterator *iter, uint8_t *const buffer, const uint16_t size);
extern uint32_t dict_write_end(DictionaryIterator *iter);
extern DictionaryResult dict_write_data(DictionaryIterator *iter, const uint32_t key, const uint8_t *const data, const uint16_t size);
extern DictionaryResult dict_write_cstring(DictionaryIterator *iter, const uint32_t key, const char *const cstring);
extern DictionaryResult dict_write_int(DictionaryIterator *iter, const uint32_t key, const void *integer, const uint8_t width_bytes, const bool is_signed);
extern DictionaryResult dict_write_uint8(DictionaryIterator *iter, const uint32_t key, const uint8_t value);
extern DictionaryResult dict_write_uint16(DictionaryIterator *iter, const uint32_t key, const uint16_t value);
extern DictionaryResult dict_write_uint32(DictionaryIterator *iter, const uint32_t key, const uint32_t value);
extern DictionaryResult dict_write_int8(DictionaryIterator *iter, const uint32_t key, const int8_t value);
extern DictionaryResult dict_write_int16(DictionaryIterator *iter, const uint32_t key, const int16_t value);
extern DictionaryResult dict_write_int32(DictionaryIterator *iter, const uint32_t key, const int32_t value);
extern DictionaryResult dict_write_tuplet(DictionaryIterator *iter, const Tuplet *const tuplet);
extern Tuple *dict_read_begin_from_buffer(DictionaryIterator *iter, const uint8_t *const buffer, const uint16_t size);
extern Tuple *dict_read_first(DictionaryIterator *iter);
extern Tuple *dict_read_next(DictionaryIterator *iter);
extern Tuple *dict_find(const DictionaryIterator *iter, const uint32_t key);
extern DictionaryResult dict_merge(
    DictionaryIterator *dest,
    uint32_t *dest_max_size_in_out,
    DictionaryIterator *source,
    const bool update_existing_keys_only,
    const DictionaryKeyUpdatedCallback key_callback,
    void *context);
extern DictionaryResult dict_serialize_tuplets(
    DictionarySerializeCallback callback,
    void *context,
    const Tuplet *const tuplets,
    const uint8_t tuplets_count);
extern DictionaryResult dict_serialize_tuplets_to_buffer(
    const Tuplet *const tuplets,
    const uint8_t tuplets_count,
    uint8_t *buffer,
    uint32_t *size_in_out);
extern DictionaryResult dict_serialize_tuplets_to_buffer__deprecated(
    const uint8_t tuplets_count,
    const Tuplet *const tuplets,
    uint8_t *buffer,
    uint32_t *size_in_out);
extern DictionaryResult dict_serialize_tuplets_to_buffer_with_iter(
    DictionaryIterator *iter,
    const Tuplet *const tuplets,
    const uint8_t tuplets_count,
    uint8_t *buffer,
    uint32_t *size_in_out);

typedef enum {
    APP_MSG_OK = 0,
    APP_MSG_SEND_TIMEOUT = 1 << 1,
    APP_MSG_SEND_REJECTED = 1 << 2,
    APP_MSG_NOT_CONNECTED = 1 << 3,
    APP_MSG_APP_NOT_RUNNING = 1 << 4,
    APP_MSG_INVALID_ARGS = 1 << 5,
    APP_MSG_BUSY = 1 << 6,
    APP_MSG_BUFFER_OVERFLOW = 1 << 7,
    APP_MSG_OUT_OF_MEMORY = 1 << 12,
    APP_MSG_CLOSED = 1 << 13,
    APP_MSG_INTERNAL_ERROR = 1 << 14,
    APP_MSG_INVALID_STATE = 1 << 15,
} AppMessageResult;

typedef void (*AppMessageInboxReceived)(DictionaryIterator *iterator, void *context);
typedef void (*AppMessageInboxDropped)(AppMessageResult reason, void *context);
typedef void (*AppMessageOutboxSent)(DictionaryIterator *iterator, void *context);
typedef void (*AppMessageOutboxFailed)(DictionaryIterator *iterator, AppMessageResult reason, void *context);

extern AppMessageResult app_message_open(const uint32_t size_inbound, const uint32_t size_outbound);
extern void app_message_deregister_callbacks(void);
extern void *app_message_get_context(void);
extern void *app_message_set_context(void *context);
extern uint32_t app_message_inbox_size_maximum(void);
extern uint32_t app_message_outbox_size_maximum(void);
extern AppMessageInboxReceived app_message_register_inbox_received(AppMessageInboxReceived cb);
extern AppMessageInboxDropped app_message_register_inbox_dropped(AppMessageInboxDropped cb);
extern AppMessageOutboxSent app_message_register_outbox_sent(AppMessageOutboxSent cb);
extern AppMessageOutboxFailed app_message_register_outbox_failed(AppMessageOutboxFailed cb);
extern AppMessageResult app_message_outbox_begin(DictionaryIterator **iterator);
extern AppMessageResult app_message_outbox_send(void);

typedef void (*AppSyncTupleChangedCallback)(const uint32_t key, const Tuple *new_tuple, const Tuple *old_tuple, void *context);
typedef void (*AppSyncErrorCallback)(DictionaryResult dict_error, AppMessageResult app_message_error, void *context);

typedef struct AppSync {
    DictionaryIterator current_iter;
    union {
        Dictionary *current;
        uint8_t *buffer;
    };
    uint16_t buffer_size;
    struct {
        AppSyncTupleChangedCallback value_changed;
        AppSyncErrorCallback error;
        void *context;
    } callback;
    bool is_initialized;
} AppSync;

extern void app_sync_init(
    AppSync *s,
    uint8_t *buffer,
    const uint16_t buffer_size,
    const Tuplet *const keys_and_initial_values,
    const uint8_t count,
    const AppSyncTupleChangedCallback tuple_changed_callback,
    const AppSyncErrorCallback error_callback,
    void *context);
extern void app_sync_deinit(AppSync *s);
extern AppMessageResult app_sync_set(AppSync *s, const Tuplet *const keys_and_values_to_update, const uint8_t count);
extern const Tuple *app_sync_get(const AppSync *s, const uint32_t key);
"""


def _pack_proto_frame(endpoint: int, payload: bytes) -> bytes:
    return struct.pack(">HH", len(payload), endpoint) + payload


def _recv_exact(sock: socket.socket, n: int, timeout: float = 5.0) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        r, _, _ = select.select([sock], [], [], timeout)
        if not r:
            raise TimeoutError(f"timed out waiting for {n} bytes (got {len(buf)})")
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError(f"unexpected EOF after {len(buf)}/{n} bytes")
        buf.extend(chunk)
    return bytes(buf)


def _recv_proto_frame(sock: socket.socket, timeout: float = 5.0) -> tuple[int, bytes]:
    hdr = _recv_exact(sock, 4, timeout=timeout)
    length, endpoint = struct.unpack(">HH", hdr)
    payload = _recv_exact(sock, length, timeout=timeout) if length > 0 else b""
    return endpoint, payload


def _pack_dict_tuples(items: list[tuple[int, int, bytes]]) -> bytes:
    out = bytearray([len(items)])
    for key, ttype, val in items:
        out.extend(struct.pack("<IBH", key, ttype, len(val)))
        out.extend(val)
    return bytes(out)


def _parse_app_rc(stdout: str) -> int:
    marker = "App exited cleanly (rc="
    idx = stdout.find(marker)
    if idx < 0:
        raise AssertionError(f"missing exit summary in output:\n{stdout}")
    rest = stdout[idx + len(marker) :]
    return int(rest.split(",", 1)[0])


class DictionaryAndAppMessageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        proc = subprocess.run(["make", "-C", str(REPO_ROOT), "runner"], capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(f"make runner failed:\n{proc.stdout}\n{proc.stderr}")

    def test_dictionary_read_write_merge_and_tuplet_serialization(self) -> None:
        c_src = COMMON_PEBBLE_DECLS + r"""
static int g_cb_count = 0;
static uint32_t g_cb_last_key = 0;
static int32_t g_cb_old_val = 0;
static int32_t g_cb_new_val = 0;
static uint16_t g_ser_cb_size = 0;

static void on_key_updated(const uint32_t key, const Tuple *new_t, const Tuple *old_t, void *ctx) {
    (void)ctx;
    g_cb_count++;
    g_cb_last_key = key;
    if (old_t != NULL && old_t->type == TUPLE_INT && old_t->length == 4) {
        g_cb_old_val = old_t->value->int32;
    }
    if (new_t != NULL && new_t->type == TUPLE_INT && new_t->length == 4) {
        g_cb_new_val = new_t->value->int32;
    }
}

static void on_serialized(const uint8_t *const data, const uint16_t size, void *ctx) {
    (void)ctx;
    if (data != NULL && data[0] == 2) {
        g_ser_cb_size = size;
    }
}

int main(void) {
    if (dict_calc_buffer_size(3, (size_t)1, (size_t)4, (size_t)6) != (1u + 21u + 11u)) {
        return 1;
    }

    uint8_t buf[128];
    DictionaryIterator iter;
    if (dict_write_begin(&iter, buf, sizeof(buf)) != DICT_OK) return 2;
    if (dict_write_uint8(&iter, 1, 0xAB) != DICT_OK) return 3;
    if (dict_write_uint16(&iter, 2, 0x1234) != DICT_OK) return 4;
    if (dict_write_uint32(&iter, 3, 0xDEADBEEFu) != DICT_OK) return 5;
    if (dict_write_int8(&iter, 4, -5) != DICT_OK) return 6;
    if (dict_write_int16(&iter, 5, -1234) != DICT_OK) return 7;
    if (dict_write_int32(&iter, 6, -987654) != DICT_OK) return 8;
    int32_t raw_i = 42;
    if (dict_write_int(&iter, 7, &raw_i, 4, true) != DICT_OK) return 9;
    if (dict_write_cstring(&iter, 8, "halo") != DICT_OK) return 10;
    const uint8_t blob[3] = {9, 8, 7};
    if (dict_write_data(&iter, 9, blob, 3) != DICT_OK) return 11;

    uint32_t written = dict_write_end(&iter);
    /* 1 + 9*7 + (1+2+4+1+2+4+4+5+3) = 1 + 63 + 26 = 90 */
    if (written != 90u) return 12;
    if (dict_size(&iter) != 90u) return 13;
    if (buf[0] != 9u) return 14;

    DictionaryIterator riter;
    Tuple *t = dict_read_begin_from_buffer(&riter, buf, (uint16_t)written);
    int seen = 0;
    while (t != NULL) {
        seen++;
        t = dict_read_next(&riter);
    }
    if (seen != 9) return 15;
    Tuple *first_again = dict_read_first(&riter);
    if (first_again == NULL || first_again->key != 1u) return 33;

    /* Exercise dict_write_tuplet explicitly */
    uint8_t tbuf[32];
    DictionaryIterator titer;
    Tuplet single;
    single.type = TUPLE_UINT;
    single.key = 55;
    single.integer.storage = 0x77u;
    single.integer.width = 1;
    if (dict_write_begin(&titer, tbuf, sizeof(tbuf)) != DICT_OK) return 34;
    if (dict_write_tuplet(&titer, &single) != DICT_OK) return 35;
    if (dict_write_end(&titer) != 9u) return 36;

    Tuple *f3 = dict_find(&riter, 3);
    if (f3 == NULL || f3->type != TUPLE_UINT || f3->length != 4 || f3->value->uint32 != 0xDEADBEEFu) return 16;
    Tuple *f6 = dict_find(&riter, 6);
    if (f6 == NULL || f6->type != TUPLE_INT || f6->value->int32 != -987654) return 17;
    Tuple *f8 = dict_find(&riter, 8);
    if (f8 == NULL || f8->type != TUPLE_CSTRING || f8->length != 5 || f8->value->cstring[0] != 'h' || f8->value->cstring[4] != '\0') return 18;
    Tuple *f9 = dict_find(&riter, 9);
    if (f9 == NULL || f9->type != TUPLE_BYTE_ARRAY || f9->length != 3 || f9->value->data[2] != 7) return 19;

    /* Tuplet serialization variants */
    Tuplet tuplets[2];
    tuplets[0].type = TUPLE_INT;
    tuplets[0].key = 100;
    tuplets[0].integer.storage = 10u;
    tuplets[0].integer.width = 4;
    tuplets[1].type = TUPLE_CSTRING;
    tuplets[1].key = 101;
    tuplets[1].cstring.data = "hi";
    tuplets[1].cstring.length = 3;

    uint32_t expected_sz = dict_calc_buffer_size_from_tuplets(tuplets, 2);
    if (expected_sz != 1u + 7u + 4u + 7u + 3u) return 20;

    uint8_t sbuf1[64], sbuf2[64], sbuf3[64];
    uint32_t sz1 = sizeof(sbuf1), sz2 = sizeof(sbuf2), sz3 = sizeof(sbuf3);
    if (dict_serialize_tuplets_to_buffer(tuplets, 2, sbuf1, &sz1) != DICT_OK || sz1 != expected_sz) return 21;
    if (dict_serialize_tuplets_to_buffer__deprecated(2, tuplets, sbuf2, &sz2) != DICT_OK || sz2 != expected_sz) return 22;
    DictionaryIterator siter;
    if (dict_serialize_tuplets_to_buffer_with_iter(&siter, tuplets, 2, sbuf3, &sz3) != DICT_OK || sz3 != expected_sz) return 23;
    if (dict_serialize_tuplets(on_serialized, NULL, tuplets, 2) != DICT_OK || g_ser_cb_size != expected_sz) return 24;

    /* dict_merge: update key 100 from 10 -> 99, add key 102 */
    uint8_t src_buf[64];
    DictionaryIterator src_iter;
    if (dict_write_begin(&src_iter, src_buf, sizeof(src_buf)) != DICT_OK) return 25;
    if (dict_write_int32(&src_iter, 100, 99) != DICT_OK) return 26;
    if (dict_write_cstring(&src_iter, 101, "hi") != DICT_OK) return 27; /* unchanged */
    if (dict_write_uint8(&src_iter, 102, 7) != DICT_OK) return 28;
    dict_write_end(&src_iter);

    uint32_t merge_max = sizeof(sbuf3);
    if (dict_merge(&siter, &merge_max, &src_iter, false, on_key_updated, NULL) != DICT_OK) return 29;
    if (g_cb_count != 2 || g_cb_old_val != 10 || g_cb_new_val != 99) return 30;
    Tuple *m100 = dict_find(&siter, 100);
    Tuple *m102 = dict_find(&siter, 102);
    if (m100 == NULL || m100->value->int32 != 99) return 31;
    if (m102 == NULL || m102->value->uint8 != 7) return 32;

    return 0;
}
"""
        app_bin = build_pebble_app_bin(c_src, app_name="DictTest")
        with tempfile.TemporaryDirectory() as tmp:
            bin_path = pathlib.Path(tmp) / "pebble-app.bin"
            bin_path.write_bytes(app_bin)
            proc = subprocess.run(
                [QEMU_ARM, str(RUNNER), "--max-frames", "1", str(bin_path)],
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(proc.returncode, 0, f"runner failed:\n{proc.stdout}\n{proc.stderr}")
            self.assertEqual(_parse_app_rc(proc.stdout), 0, proc.stdout)

    def test_app_message_push_ack_nack_chaining_and_app_run_state_over_proto_fd(self) -> None:
        app_uuid = bytes.fromhex("112233445566778899aabbccddeeff00")
        c_src = COMMON_PEBBLE_DECLS + r"""
static int g_ctx_tag = 77;
static int g_outbox_sent_count = 0;
static int g_outbox_failed_reason = 0;
static int g_inbox_dropped_reason = 0;
static int g_inbox_val = 0;
static int g_err = 0;

static void on_outbox_sent(DictionaryIterator *iter, void *ctx) {
    (void)iter;
    if (ctx != &g_ctx_tag) { g_err = 101; window_stack_pop(false); return; }
    g_outbox_sent_count++;
    /* Immediately chain a second outbox send inside outbox_sent (commit-before-callback). */
    if (g_outbox_sent_count == 1) {
        DictionaryIterator *next = NULL;
        if (app_message_outbox_begin(&next) != APP_MSG_OK) { g_err = 102; window_stack_pop(false); return; }
        dict_write_uint8(next, 2, 0x55);
        if (app_message_outbox_send() != APP_MSG_OK) { g_err = 103; window_stack_pop(false); return; }
    }
}

static void on_outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *ctx) {
    (void)iter;
    (void)ctx;
    g_outbox_failed_reason = (int)reason;
}

static void on_inbox_dropped(AppMessageResult reason, void *ctx) {
    (void)ctx;
    g_inbox_dropped_reason = (int)reason;
}

static void on_inbox_received(DictionaryIterator *iter, void *ctx) {
    (void)ctx;
    Tuple *t = dict_find(iter, 42);
    if (t != NULL && t->type == TUPLE_INT && t->length == 4) {
        g_inbox_val = (int)t->value->int32;
    }
    window_stack_pop(false);
}

int main(void) {
    if (app_message_inbox_size_maximum() != 8200u) return 1;
    if (app_message_outbox_size_maximum() != 8200u) return 2;

    /* Register callbacks BEFORE app_message_open to verify callback preservation. */
    app_message_set_context(&g_ctx_tag);
    if (app_message_get_context() != &g_ctx_tag) return 3;
    app_message_register_outbox_sent(on_outbox_sent);
    app_message_register_outbox_failed(on_outbox_failed);
    app_message_register_inbox_dropped(on_inbox_dropped);
    app_message_register_inbox_received(on_inbox_received);

    if (app_message_open(64, 64) != APP_MSG_OK) return 4;

    /* Queue an init()-time outbox message before entering app_event_loop(). */
    DictionaryIterator *iter = NULL;
    if (app_message_outbox_begin(&iter) != APP_MSG_OK) return 5;
    dict_write_uint32(iter, 1, 0x12345678u);
    if (app_message_outbox_send() != APP_MSG_OK) return 6;
    /* Outbox must report APP_MSG_BUSY while queued. */
    if (app_message_outbox_begin(&iter) != APP_MSG_BUSY) return 7;

    Window *w = window_create();
    window_stack_push(w, false);
    app_event_loop();
    window_destroy(w);

    if (g_err != 0) return g_err;
    if (g_outbox_sent_count != 1) return 20;
    if (g_outbox_failed_reason != APP_MSG_SEND_REJECTED) return 21;
    if (g_inbox_dropped_reason != APP_MSG_BUFFER_OVERFLOW) return 22;
    if (g_inbox_val != 1337) return 23;
    return 0;
}
"""
        app_bin = build_pebble_app_bin(c_src, app_name="AppMsgTest", uuid=app_uuid)
        parent_sock, child_sock = socket.socketpair()
        try:
            with tempfile.TemporaryDirectory() as tmp:
                bin_path = pathlib.Path(tmp) / "pebble-app.bin"
                bin_path.write_bytes(app_bin)
                proc = subprocess.Popen(
                    [
                        QEMU_ARM,
                        str(RUNNER),
                        "--proto-fd",
                        str(child_sock.fileno()),
                        "--max-frames",
                        "0",
                        str(bin_path),
                    ],
                    pass_fds=(child_sock.fileno(),),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                child_sock.close()

                # 1. AppRunState START arrives first
                ep, payload = _recv_proto_frame(parent_sock)
                self.assertEqual(ep, EP_APP_RUN_STATE)
                self.assertEqual(payload, bytes([RUN_STATE_START]) + app_uuid)

                # 2. init()-queued AppMessage PUSH arrives right behind START
                ep, payload = _recv_proto_frame(parent_sock)
                self.assertEqual(ep, EP_APP_MESSAGE)
                self.assertEqual(payload[0], APP_MSG_PUSH)
                tx1 = payload[1]
                self.assertEqual(payload[2:18], app_uuid)
                self.assertEqual(payload[18:], _pack_dict_tuples([(1, 2, struct.pack("<I", 0x12345678))]))

                # 3. AppRunState REQUEST -> replies with START + UUID
                parent_sock.sendall(_pack_proto_frame(EP_APP_RUN_STATE, bytes([RUN_STATE_REQUEST])))
                ep, payload = _recv_proto_frame(parent_sock)
                self.assertEqual(ep, EP_APP_RUN_STATE)
                self.assertEqual(payload, bytes([RUN_STATE_START]) + app_uuid)

                # 4. ACK first PUSH -> Watchapp chains second PUSH
                parent_sock.sendall(_pack_proto_frame(EP_APP_MESSAGE, bytes([APP_MSG_ACK, tx1])))
                ep, payload = _recv_proto_frame(parent_sock)
                self.assertEqual(ep, EP_APP_MESSAGE)
                self.assertEqual(payload[0], APP_MSG_PUSH)
                tx2 = payload[1]
                self.assertEqual(payload[18:], _pack_dict_tuples([(2, 2, bytes([0x55]))]))

                # 5. NACK second PUSH -> Watchapp gets APP_MSG_SEND_REJECTED
                parent_sock.sendall(_pack_proto_frame(EP_APP_MESSAGE, bytes([APP_MSG_NACK, tx2])))

                # 6. Inbound PUSH with wrong UUID -> NACK only
                bad_uuid = bytes([0xAA] * 16)
                parent_sock.sendall(
                    _pack_proto_frame(
                        EP_APP_MESSAGE,
                        bytes([APP_MSG_PUSH, 10]) + bad_uuid + _pack_dict_tuples([(42, 3, struct.pack("<i", 1))]),
                    )
                )
                ep, payload = _recv_proto_frame(parent_sock)
                self.assertEqual((ep, payload), (EP_APP_MESSAGE, bytes([APP_MSG_NACK, 10])))

                # 7. Inbound PUSH overflowing 64-byte inbox -> NACK + inbox_dropped(BUFFER_OVERFLOW)
                big_dict = _pack_dict_tuples([(99, 0, b"X" * 80)])
                parent_sock.sendall(
                    _pack_proto_frame(EP_APP_MESSAGE, bytes([APP_MSG_PUSH, 11]) + app_uuid + big_dict)
                )
                ep, payload = _recv_proto_frame(parent_sock)
                self.assertEqual((ep, payload), (EP_APP_MESSAGE, bytes([APP_MSG_NACK, 11])))

                # 8. Valid inbound PUSH split across two chunks (tests stream reassembly) -> ACK + exit
                good_frame = _pack_proto_frame(
                    EP_APP_MESSAGE,
                    bytes([APP_MSG_PUSH, 12]) + app_uuid + _pack_dict_tuples([(42, 3, struct.pack("<i", 1337))]),
                )
                parent_sock.sendall(good_frame[:7])
                parent_sock.sendall(good_frame[7:])
                ep, payload = _recv_proto_frame(parent_sock)
                self.assertEqual((ep, payload), (EP_APP_MESSAGE, bytes([APP_MSG_ACK, 12])))

                # 9. AppRunState STOP on loop exit
                ep, payload = _recv_proto_frame(parent_sock)
                self.assertEqual(ep, EP_APP_RUN_STATE)
                self.assertEqual(payload, bytes([RUN_STATE_STOP]) + app_uuid)

                stdout, stderr = proc.communicate(timeout=5)
                self.assertEqual(proc.returncode, 0, f"runner failed:\n{stdout}\n{stderr}")
                self.assertEqual(_parse_app_rc(stdout), 0, stdout)
        finally:
            parent_sock.close()

    def test_app_message_not_connected_and_async_dispatch(self) -> None:
        c_src = COMMON_PEBBLE_DECLS + r"""
static int g_failed_reason = 0;
static int g_in_send = 0;
static int g_reentrant = 0;

static void on_failed(DictionaryIterator *iter, AppMessageResult reason, void *ctx) {
    (void)iter;
    (void)ctx;
    if (g_in_send) g_reentrant = 1;
    g_failed_reason = (int)reason;
    window_stack_pop(false);
}

int main(void) {
    app_message_register_outbox_failed(on_failed);
    if (app_message_open(64, 64) != APP_MSG_OK) return 1;

    DictionaryIterator *iter = NULL;
    if (app_message_outbox_begin(&iter) != APP_MSG_OK) return 2;
    dict_write_uint8(iter, 1, 9);
    g_in_send = 1;
    if (app_message_outbox_send() != APP_MSG_OK) return 3;
    g_in_send = 0;

    Window *w = window_create();
    window_stack_push(w, false);
    app_event_loop();
    window_destroy(w);

    if (g_reentrant) return 10;
    if (g_failed_reason != APP_MSG_NOT_CONNECTED) return 11;
    return 0;
}
"""
        app_bin = build_pebble_app_bin(c_src, app_name="DisconnectedTest")
        with tempfile.TemporaryDirectory() as tmp:
            bin_path = pathlib.Path(tmp) / "pebble-app.bin"
            bin_path.write_bytes(app_bin)
            proc = subprocess.run(
                [QEMU_ARM, str(RUNNER), "--max-frames", "0", str(bin_path)],
                capture_output=True,
                text=True,
                timeout=5,
            )
            self.assertEqual(proc.returncode, 0, f"runner failed:\n{proc.stdout}\n{proc.stderr}")
            self.assertEqual(_parse_app_rc(proc.stdout), 0, proc.stdout)

    def test_app_message_ack_timeout(self) -> None:
        app_uuid = bytes.fromhex("aabbccddeeff00112233445566778899")
        c_src = COMMON_PEBBLE_DECLS + r"""
static int g_failed_reason = 0;

static void on_failed(DictionaryIterator *iter, AppMessageResult reason, void *ctx) {
    (void)iter;
    (void)ctx;
    g_failed_reason = (int)reason;
    window_stack_pop(false);
}

int main(void) {
    app_message_register_outbox_failed(on_failed);
    if (app_message_open(64, 64) != APP_MSG_OK) return 1;
    DictionaryIterator *iter = NULL;
    if (app_message_outbox_begin(&iter) != APP_MSG_OK) return 2;
    dict_write_uint8(iter, 1, 1);
    if (app_message_outbox_send() != APP_MSG_OK) return 3;

    Window *w = window_create();
    window_stack_push(w, false);
    app_event_loop();
    window_destroy(w);
    return (g_failed_reason == APP_MSG_SEND_TIMEOUT) ? 0 : 10;
}
"""
        app_bin = build_pebble_app_bin(c_src, app_name="TimeoutTest", uuid=app_uuid)
        parent_sock, child_sock = socket.socketpair()
        try:
            with tempfile.TemporaryDirectory() as tmp:
                bin_path = pathlib.Path(tmp) / "pebble-app.bin"
                bin_path.write_bytes(app_bin)
                proc = subprocess.Popen(
                    [QEMU_ARM, str(RUNNER), "--proto-fd", str(child_sock.fileno()), "--max-frames", "0", str(bin_path)],
                    pass_fds=(child_sock.fileno(),),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                child_sock.close()
                # Read START and PUSH, intentionally never send ACK -> ~3s timeout
                ep1, _ = _recv_proto_frame(parent_sock)
                ep2, _ = _recv_proto_frame(parent_sock)
                self.assertEqual((ep1, ep2), (EP_APP_RUN_STATE, EP_APP_MESSAGE))
                stdout, stderr = proc.communicate(timeout=10)
                self.assertEqual(proc.returncode, 0, f"runner failed:\n{stdout}\n{stderr}")
                self.assertEqual(_parse_app_rc(stdout), 0, stdout)
        finally:
            parent_sock.close()

    def test_app_sync_init_get_set_and_remote_update(self) -> None:
        app_uuid = bytes.fromhex("0102030405060708090a0b0c0d0e0f10")
        c_src = COMMON_PEBBLE_DECLS + r"""
static AppSync g_sync;
static uint8_t g_sync_buf[128];
static int g_init_changes = 0;
static int g_updated_old = 0;
static int g_updated_new = 0;
static int g_err_count = 0;

static void on_sync_change(const uint32_t key, const Tuple *new_t, const Tuple *old_t, void *ctx) {
    (void)ctx;
    if (old_t == NULL) {
        g_init_changes++;
        return;
    }
    if (key == 1 && new_t != NULL && new_t->type == TUPLE_INT) {
        g_updated_old = old_t->value->int32;
        g_updated_new = new_t->value->int32;
        const Tuple *cur = app_sync_get(&g_sync, 1);
        if (cur != NULL && cur->value->int32 == 77) {
            window_stack_pop(false);
        }
    }
}

static void on_sync_error(DictionaryResult derr, AppMessageResult aerr, void *ctx) {
    (void)derr; (void)aerr; (void)ctx;
    g_err_count++;
}

int main(void) {
    if (app_message_open(128, 128) != APP_MSG_OK) return 1;

    Tuplet init_vals[2];
    init_vals[0].type = TUPLE_INT;
    init_vals[0].key = 1;
    init_vals[0].integer.storage = 10u;
    init_vals[0].integer.width = 4;
    init_vals[1].type = TUPLE_CSTRING;
    init_vals[1].key = 2;
    init_vals[1].cstring.data = "init";
    init_vals[1].cstring.length = 5;

    app_sync_init(&g_sync, g_sync_buf, sizeof(g_sync_buf), init_vals, 2, on_sync_change, on_sync_error, NULL);
    if (g_init_changes != 2) return 2;
    const Tuple *t1 = app_sync_get(&g_sync, 1);
    if (t1 == NULL || t1->value->int32 != 10) return 3;

    Tuplet upd[1];
    upd[0].type = TUPLE_INT;
    upd[0].key = 1;
    upd[0].integer.storage = 20u;
    upd[0].integer.width = 4;
    if (app_sync_set(&g_sync, upd, 1) != APP_MSG_OK) return 4;

    Window *w = window_create();
    window_stack_push(w, false);
    app_event_loop();
    app_sync_deinit(&g_sync);
    window_destroy(w);

    if (g_err_count != 0) return 5;
    if (g_updated_old != 20 || g_updated_new != 77) return 6;
    return 0;
}
"""
        app_bin = build_pebble_app_bin(c_src, app_name="AppSyncTest", uuid=app_uuid)
        parent_sock, child_sock = socket.socketpair()
        try:
            with tempfile.TemporaryDirectory() as tmp:
                bin_path = pathlib.Path(tmp) / "pebble-app.bin"
                bin_path.write_bytes(app_bin)
                proc = subprocess.Popen(
                    [QEMU_ARM, str(RUNNER), "--proto-fd", str(child_sock.fileno()), "--max-frames", "0", str(bin_path)],
                    pass_fds=(child_sock.fileno(),),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                child_sock.close()
                ep, payload = _recv_proto_frame(parent_sock)
                self.assertEqual((ep, payload), (EP_APP_RUN_STATE, bytes([RUN_STATE_START]) + app_uuid))

                ep, payload = _recv_proto_frame(parent_sock)
                self.assertEqual(ep, EP_APP_MESSAGE)
                self.assertEqual(payload[0], APP_MSG_PUSH)
                tx = payload[1]
                self.assertEqual(payload[18:], _pack_dict_tuples([(1, 3, struct.pack("<i", 20))]))
                parent_sock.sendall(_pack_proto_frame(EP_APP_MESSAGE, bytes([APP_MSG_ACK, tx])))

                # Push remote update key=1 -> 77
                parent_sock.sendall(
                    _pack_proto_frame(
                        EP_APP_MESSAGE,
                        bytes([APP_MSG_PUSH, 5]) + app_uuid + _pack_dict_tuples([(1, 3, struct.pack("<i", 77))]),
                    )
                )
                ep, payload = _recv_proto_frame(parent_sock)
                self.assertEqual((ep, payload), (EP_APP_MESSAGE, bytes([APP_MSG_ACK, 5])))

                stdout, stderr = proc.communicate(timeout=5)
                self.assertEqual(proc.returncode, 0, f"runner failed:\n{stdout}\n{stderr}")
                self.assertEqual(_parse_app_rc(stdout), 0, stdout)
        finally:
            parent_sock.close()

    def test_loader_extracts_uuid_for_v8_v9_and_v16_headers(self) -> None:
        c_src = COMMON_PEBBLE_DECLS + r"""
int main(void) {
    Window *w = window_create();
    window_stack_push(w, false);
    app_event_loop();
    window_destroy(w);
    return 0;
}
"""
        for ver in (0x08, 0x09, 0x10):
            with self.subTest(struct_version=hex(ver)):
                app_uuid = bytes([(ver + i) & 0xFF for i in range(16)])
                app_bin = build_pebble_app_bin(c_src, struct_version_major=ver, uuid=app_uuid)
                parent_sock, child_sock = socket.socketpair()
                try:
                    with tempfile.TemporaryDirectory() as tmp:
                        bin_path = pathlib.Path(tmp) / "pebble-app.bin"
                        bin_path.write_bytes(app_bin)
                        proc = subprocess.Popen(
                            [QEMU_ARM, str(RUNNER), "--proto-fd", str(child_sock.fileno()), "--max-frames", "1", str(bin_path)],
                            pass_fds=(child_sock.fileno(),),
                            stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE,
                            text=True,
                        )
                        child_sock.close()
                        ep, payload = _recv_proto_frame(parent_sock)
                        self.assertEqual(ep, EP_APP_RUN_STATE)
                        self.assertEqual(payload, bytes([RUN_STATE_START]) + app_uuid)
                        proc.communicate(timeout=5)
                        self.assertEqual(proc.returncode, 0)
                finally:
                    parent_sock.close()

    def test_app_message_peer_eof_keeps_watchapp_alive_and_reports_not_connected(self) -> None:
        app_uuid = bytes.fromhex("fedcba98765432100123456789abcdef")
        c_src = COMMON_PEBBLE_DECLS + r"""
static int g_failed_reason = 0;

static void on_failed(DictionaryIterator *iter, AppMessageResult reason, void *ctx) {
    (void)iter;
    (void)ctx;
    g_failed_reason = (int)reason;
    window_stack_pop(false);
}

static void on_timer(void *data) {
    (void)data;
    DictionaryIterator *iter = NULL;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
        dict_write_uint8(iter, 1, 42);
        app_message_outbox_send();
    }
}

int main(void) {
    app_message_register_outbox_failed(on_failed);
    if (app_message_open(64, 64) != APP_MSG_OK) return 1;
    app_timer_register(150, on_timer, NULL);

    Window *w = window_create();
    window_stack_push(w, false);
    app_event_loop();
    app_message_deregister_callbacks();
    window_destroy(w);
    return (g_failed_reason == APP_MSG_NOT_CONNECTED) ? 0 : 10;
}
"""
        app_bin = build_pebble_app_bin(c_src, app_name="EofTest", uuid=app_uuid)
        parent_sock, child_sock = socket.socketpair()
        try:
            with tempfile.TemporaryDirectory() as tmp:
                bin_path = pathlib.Path(tmp) / "pebble-app.bin"
                bin_path.write_bytes(app_bin)
                proc = subprocess.Popen(
                    [QEMU_ARM, str(RUNNER), "--proto-fd", str(child_sock.fileno()), "--max-frames", "0", str(bin_path)],
                    pass_fds=(child_sock.fileno(),),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                child_sock.close()
                ep, payload = _recv_proto_frame(parent_sock)
                self.assertEqual((ep, payload), (EP_APP_RUN_STATE, bytes([RUN_STATE_START]) + app_uuid))
                # Close companion end immediately -> Watchapp must stay alive and fail timer send with NOT_CONNECTED
                parent_sock.close()
                stdout, stderr = proc.communicate(timeout=5)
                self.assertEqual(proc.returncode, 0, f"runner failed:\n{stdout}\n{stderr}")
                self.assertEqual(_parse_app_rc(stdout), 0, stdout)
        finally:
            parent_sock.close()


if __name__ == "__main__":
    unittest.main()
