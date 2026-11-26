#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pebble_internal.h"
#include "pebble_syscall_table_gen.h"

#define PEBBLE_APP_MSG_MAX_SIZE 8200u
#define PEBBLE_PROTO_HEADER_BYTES 4u
#define PEBBLE_PROTO_EP_APP_MESSAGE 0x0030u
#define PEBBLE_PROTO_EP_APP_RUN_STATE 0x0034u

#define PEBBLE_APP_MSG_CMD_PUSH 0x01u
#define PEBBLE_APP_MSG_CMD_ACK 0xffu
#define PEBBLE_APP_MSG_CMD_NACK 0x7fu

#define PEBBLE_APP_RUN_STATE_START 0x01u
#define PEBBLE_APP_RUN_STATE_STOP 0x02u
#define PEBBLE_APP_RUN_STATE_REQUEST 0x03u

#define PEBBLE_APP_MSG_PUSH_HDR_BYTES 18u /* cmd(1) + tx_id(1) + uuid(16) */
#define PEBBLE_APP_MSG_PROTO_OVERHEAD (PEBBLE_PROTO_HEADER_BYTES + PEBBLE_APP_MSG_PUSH_HDR_BYTES)
#define PEBBLE_APP_MSG_ACK_TIMEOUT_MS 3000u

typedef enum {
    OUTBOX_IDLE = 0,
    OUTBOX_WRITING = 1,
    OUTBOX_QUEUED = 2,
    OUTBOX_IN_FLIGHT = 3,
    OUTBOX_FAIL_PENDING = 4,
} outbox_state_t;

typedef struct {
    bool opened;
    bool in_event_loop;

    uint8_t *inbox_buf;
    uint32_t inbox_cap;
    pebble_dict_iterator_t inbox_iter;

    uint8_t *outbox_buf;
    uint32_t outbox_cap;
    pebble_dict_iterator_t outbox_iter;
    uint32_t outbox_len;
    outbox_state_t outbox_state;
    uint8_t next_tx_id;
    uint8_t inflight_tx_id;
    uint64_t ack_deadline_ms;
    uint32_t pending_fail_reason;

    uint8_t *proto_buf;
    size_t proto_cap;

    void *context;
    pebble_app_msg_inbox_received_t inbox_received;
    pebble_app_msg_inbox_dropped_t inbox_dropped;
    pebble_app_msg_outbox_sent_t outbox_sent;
    pebble_app_msg_outbox_failed_t outbox_failed;
} pebble_app_msg_state_t;

/* Single pointer in static .bss (4 bytes on ARM); state & buffers allocate transiently via HAL. */
static pebble_app_msg_state_t *g_am;

static void am_memset(uint8_t *dst, uint8_t val, size_t len) {
    for (size_t i = 0u; i < len; ++i) {
        dst[i] = val;
    }
}

static void am_memcpy(uint8_t *dst, const uint8_t *src, size_t len) {
    for (size_t i = 0u; i < len; ++i) {
        dst[i] = src[i];
    }
}

static bool am_memeq(const uint8_t *a, const uint8_t *b, size_t len) {
    for (size_t i = 0u; i < len; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

static uint16_t am_read_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void am_write_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xffu);
    p[1] = (uint8_t)(v & 0xffu);
}

static pebble_app_msg_state_t *ensure_state(void) {
    if (g_am != NULL) {
        return g_am;
    }
    g_am = (pebble_app_msg_state_t *)pebble_rt_alloc_data(sizeof(pebble_app_msg_state_t));
    if (g_am != NULL) {
        am_memset((uint8_t *)g_am, 0u, sizeof(pebble_app_msg_state_t));
    }
    return g_am;
}

static bool ensure_proto_buf(pebble_app_msg_state_t *am, size_t needed) {
    if (am == NULL) {
        return false;
    }
    if (am->proto_buf != NULL && am->proto_cap >= needed) {
        return true;
    }
    size_t target = needed;
    if (target < PEBBLE_APP_MSG_PROTO_OVERHEAD + PEBBLE_APP_MSG_MAX_SIZE) {
        target = PEBBLE_APP_MSG_PROTO_OVERHEAD + PEBBLE_APP_MSG_MAX_SIZE;
    }
    uint8_t *buf = (uint8_t *)pebble_rt_alloc_data(target);
    if (buf == NULL) {
        return false;
    }
    if (am->proto_buf != NULL) {
        pebble_rt_free_data(am->proto_buf, am->proto_cap);
    }
    am->proto_buf = buf;
    am->proto_cap = target;
    return true;
}

static bool send_app_run_state(uint8_t cmd) {
    const pebble_hal_t *hal = pebble_rt_hal();
    const pebble_uuid_t *uuid = pebble_rt_app_uuid();
    if (hal == NULL || hal->send_protocol_packet == NULL || uuid == NULL) {
        return false;
    }
    uint8_t pkt[PEBBLE_PROTO_HEADER_BYTES + 1u + 16u];
    am_write_be16(pkt, 17u);
    am_write_be16(pkt + 2u, PEBBLE_PROTO_EP_APP_RUN_STATE);
    pkt[4] = cmd;
    am_memcpy(pkt + 5u, uuid->bytes, 16u);
    return hal->send_protocol_packet(hal->ctx, pkt, sizeof(pkt));
}

static void send_ack_nack(uint8_t cmd, uint8_t tx_id) {
    const pebble_hal_t *hal = pebble_rt_hal();
    if (hal == NULL || hal->send_protocol_packet == NULL) {
        return;
    }
    uint8_t pkt[PEBBLE_PROTO_HEADER_BYTES + 2u];
    am_write_be16(pkt, 2u);
    am_write_be16(pkt + 2u, PEBBLE_PROTO_EP_APP_MESSAGE);
    pkt[4] = cmd;
    pkt[5] = tx_id;
    (void)hal->send_protocol_packet(hal->ctx, pkt, sizeof(pkt));
}

static bool flush_queued_outbox(pebble_app_msg_state_t *am, uint64_t now_ms) {
    if (am == NULL || am->outbox_state != OUTBOX_QUEUED) {
        return false;
    }
    const pebble_hal_t *hal = pebble_rt_hal();
    const pebble_uuid_t *uuid = pebble_rt_app_uuid();
    if (hal == NULL || hal->send_protocol_packet == NULL || uuid == NULL || am->outbox_buf == NULL) {
        am->outbox_state = OUTBOX_FAIL_PENDING;
        am->pending_fail_reason = PEBBLE_APP_MSG_NOT_CONNECTED;
        return false;
    }

    size_t total_len = PEBBLE_APP_MSG_PROTO_OVERHEAD + (size_t)am->outbox_len;
    if (!ensure_proto_buf(am, total_len)) {
        am->outbox_state = OUTBOX_FAIL_PENDING;
        am->pending_fail_reason = PEBBLE_APP_MSG_OUT_OF_MEMORY;
        return false;
    }

    uint8_t tx_id = am->next_tx_id++;
    uint16_t payload_len = (uint16_t)(PEBBLE_APP_MSG_PUSH_HDR_BYTES + am->outbox_len);
    am_write_be16(am->proto_buf, payload_len);
    am_write_be16(am->proto_buf + 2u, PEBBLE_PROTO_EP_APP_MESSAGE);
    am->proto_buf[4] = PEBBLE_APP_MSG_CMD_PUSH;
    am->proto_buf[5] = tx_id;
    am_memcpy(am->proto_buf + 6u, uuid->bytes, 16u);
    am_memcpy(am->proto_buf + PEBBLE_APP_MSG_PROTO_OVERHEAD, am->outbox_buf, am->outbox_len);

    if (!hal->send_protocol_packet(hal->ctx, am->proto_buf, total_len)) {
        am->outbox_state = OUTBOX_FAIL_PENDING;
        am->pending_fail_reason = PEBBLE_APP_MSG_NOT_CONNECTED;
        return false;
    }

    am->inflight_tx_id = tx_id;
    am->ack_deadline_ms = now_ms + PEBBLE_APP_MSG_ACK_TIMEOUT_MS;
    am->outbox_state = OUTBOX_IN_FLIGHT;
    return true;
}

static void deliver_outbox_sent(pebble_app_msg_state_t *am) {
    pebble_app_msg_outbox_sent_t cb = am->outbox_sent;
    void *ctx = am->context;
    pebble_dict_iterator_t iter;
    (void)pebble_dict_read_begin_internal(&iter, am->outbox_buf, (uint16_t)am->outbox_len);
    /* Commit IDLE before callback so the callback can immediately chain outbox_begin/send. */
    am->outbox_state = OUTBOX_IDLE;
    if (cb != NULL) {
        cb(&iter, ctx);
    }
}

static void deliver_outbox_failed(pebble_app_msg_state_t *am, uint32_t reason) {
    pebble_app_msg_outbox_failed_t cb = am->outbox_failed;
    void *ctx = am->context;
    pebble_dict_iterator_t iter;
    (void)pebble_dict_read_begin_internal(&iter, am->outbox_buf, (uint16_t)am->outbox_len);
    /* Commit IDLE before callback so the callback can immediately chain outbox_begin/send. */
    am->outbox_state = OUTBOX_IDLE;
    if (cb != NULL) {
        cb(&iter, reason, ctx);
    }
}

static void handle_inbound_packet(pebble_app_msg_state_t *am, const uint8_t *pkt, size_t pkt_len) {
    if (pkt == NULL || pkt_len < PEBBLE_PROTO_HEADER_BYTES) {
        return;
    }
    uint16_t payload_len = am_read_be16(pkt);
    uint16_t endpoint = am_read_be16(pkt + 2u);
    if ((size_t)payload_len + PEBBLE_PROTO_HEADER_BYTES > pkt_len) {
        return;
    }
    const uint8_t *payload = pkt + PEBBLE_PROTO_HEADER_BYTES;

    if (endpoint == PEBBLE_PROTO_EP_APP_RUN_STATE) {
        if (payload_len >= 1u && payload[0] == PEBBLE_APP_RUN_STATE_REQUEST) {
            (void)send_app_run_state(PEBBLE_APP_RUN_STATE_START);
        }
        return;
    }

    if (endpoint != PEBBLE_PROTO_EP_APP_MESSAGE || payload_len < 2u) {
        return;
    }

    uint8_t cmd = payload[0];
    uint8_t tx_id = payload[1];

    if (cmd == PEBBLE_APP_MSG_CMD_ACK) {
        if (am != NULL && am->outbox_state == OUTBOX_IN_FLIGHT && tx_id == am->inflight_tx_id) {
            deliver_outbox_sent(am);
        }
        return;
    }

    if (cmd == PEBBLE_APP_MSG_CMD_NACK) {
        if (am != NULL && am->outbox_state == OUTBOX_IN_FLIGHT && tx_id == am->inflight_tx_id) {
            deliver_outbox_failed(am, PEBBLE_APP_MSG_SEND_REJECTED);
        }
        return;
    }

    if (cmd == PEBBLE_APP_MSG_CMD_PUSH) {
        if (payload_len < PEBBLE_APP_MSG_PUSH_HDR_BYTES) {
            send_ack_nack(PEBBLE_APP_MSG_CMD_NACK, tx_id);
            return;
        }
        const pebble_uuid_t *app_uuid = pebble_rt_app_uuid();
        if (app_uuid == NULL || !am_memeq(payload + 2u, app_uuid->bytes, 16u)) {
            send_ack_nack(PEBBLE_APP_MSG_CMD_NACK, tx_id);
            return;
        }
        if (am == NULL || !am->opened || am->inbox_buf == NULL || am->inbox_cap == 0u) {
            send_ack_nack(PEBBLE_APP_MSG_CMD_NACK, tx_id);
            return;
        }

        const uint8_t *dict_ptr = payload + PEBBLE_APP_MSG_PUSH_HDR_BYTES;
        uint32_t dict_len = (uint32_t)(payload_len - PEBBLE_APP_MSG_PUSH_HDR_BYTES);
        if (dict_len > am->inbox_cap) {
            send_ack_nack(PEBBLE_APP_MSG_CMD_NACK, tx_id);
            if (am->inbox_dropped != NULL) {
                am->inbox_dropped(PEBBLE_APP_MSG_BUFFER_OVERFLOW, am->context);
            }
            return;
        }

        uint32_t actual_len = 0u;
        if (!pebble_dict_validate_buffer(dict_ptr, dict_len, &actual_len) || actual_len > am->inbox_cap) {
            send_ack_nack(PEBBLE_APP_MSG_CMD_NACK, tx_id);
            if (am->inbox_dropped != NULL) {
                am->inbox_dropped(PEBBLE_APP_MSG_INVALID_ARGS, am->context);
            }
            return;
        }

        am_memcpy(am->inbox_buf, dict_ptr, actual_len);
        send_ack_nack(PEBBLE_APP_MSG_CMD_ACK, tx_id);

        (void)pebble_dict_read_begin_internal(&am->inbox_iter, am->inbox_buf, (uint16_t)actual_len);
        if (am->inbox_received != NULL) {
            am->inbox_received(&am->inbox_iter, am->context);
        }
    }
}

void pebble_app_message_reset(void) {
    pebble_app_message_deinit();
}

void pebble_app_message_deinit(void) {
    if (g_am == NULL) {
        return;
    }
    if (g_am->inbox_buf != NULL) {
        pebble_rt_free_data(g_am->inbox_buf, g_am->inbox_cap);
    }
    if (g_am->outbox_buf != NULL) {
        pebble_rt_free_data(g_am->outbox_buf, g_am->outbox_cap);
    }
    if (g_am->proto_buf != NULL) {
        pebble_rt_free_data(g_am->proto_buf, g_am->proto_cap);
    }
    pebble_rt_free_data(g_am, sizeof(pebble_app_msg_state_t));
    g_am = NULL;
}

void pebble_app_message_on_loop_enter(void) {
    const pebble_hal_t *hal = pebble_rt_hal();
    if (hal != NULL && (hal->send_protocol_packet != NULL || hal->recv_protocol_packet != NULL)) {
        pebble_app_msg_state_t *am = ensure_state();
        if (am != NULL) {
            am->in_event_loop = true;
            (void)send_app_run_state(PEBBLE_APP_RUN_STATE_START);
            if (am->outbox_state == OUTBOX_QUEUED) {
                (void)flush_queued_outbox(am, pebble_rt_mono_ms());
            }
        }
        return;
    }
    if (g_am != NULL) {
        g_am->in_event_loop = true;
        if (g_am->outbox_state == OUTBOX_QUEUED) {
            (void)flush_queued_outbox(g_am, pebble_rt_mono_ms());
        }
    }
}

void pebble_app_message_on_loop_exit(void) {
    if (g_am != NULL) {
        g_am->in_event_loop = false;
    }
    (void)send_app_run_state(PEBBLE_APP_RUN_STATE_STOP);
}

void pebble_app_message_on_protocol_rx(void) {
    const pebble_hal_t *hal = pebble_rt_hal();
    if (hal == NULL || hal->recv_protocol_packet == NULL) {
        return;
    }
    pebble_app_msg_state_t *am = ensure_state();
    if (am == NULL || !ensure_proto_buf(am, PEBBLE_APP_MSG_PROTO_OVERHEAD + PEBBLE_APP_MSG_MAX_SIZE)) {
        return;
    }
    for (uint32_t budget = 0u; budget < 16u; ++budget) {
        size_t n = hal->recv_protocol_packet(hal->ctx, am->proto_buf, am->proto_cap);
        if (n == 0u) {
            break;
        }
        handle_inbound_packet(am, am->proto_buf, n);
    }
}

void pebble_app_message_process(uint64_t now_ms) {
    pebble_app_message_on_protocol_rx();
    if (g_am == NULL) {
        return;
    }

    if (g_am->outbox_state == OUTBOX_QUEUED) {
        (void)flush_queued_outbox(g_am, now_ms);
    }

    if (g_am->outbox_state == OUTBOX_FAIL_PENDING) {
        uint32_t reason = g_am->pending_fail_reason;
        deliver_outbox_failed(g_am, reason);
        return;
    }

    if (g_am->outbox_state == OUTBOX_IN_FLIGHT && now_ms >= g_am->ack_deadline_ms) {
        deliver_outbox_failed(g_am, PEBBLE_APP_MSG_SEND_TIMEOUT);
    }
}

/* --- PebbleOS AppMessage Syscall Shims (12 symbols) --- */

static uint32_t app_message_open_internal(uint32_t size_inbound, uint32_t size_outbound) {
    pebble_app_msg_state_t *am = ensure_state();
    if (am == NULL) {
        return PEBBLE_APP_MSG_OUT_OF_MEMORY;
    }

    uint32_t in_sz = (size_inbound > PEBBLE_APP_MSG_MAX_SIZE) ? PEBBLE_APP_MSG_MAX_SIZE : size_inbound;
    uint32_t out_sz = (size_outbound > PEBBLE_APP_MSG_MAX_SIZE) ? PEBBLE_APP_MSG_MAX_SIZE : size_outbound;

    uint8_t *new_in = NULL;
    uint8_t *new_out = NULL;
    if (in_sz > 0u) {
        new_in = (uint8_t *)pebble_rt_alloc_data(in_sz);
        if (new_in == NULL) {
            return PEBBLE_APP_MSG_OUT_OF_MEMORY;
        }
    }
    if (out_sz > 0u) {
        new_out = (uint8_t *)pebble_rt_alloc_data(out_sz);
        if (new_out == NULL) {
            pebble_rt_free_data(new_in, in_sz);
            return PEBBLE_APP_MSG_OUT_OF_MEMORY;
        }
    }

    if (am->inbox_buf != NULL) {
        pebble_rt_free_data(am->inbox_buf, am->inbox_cap);
    }
    if (am->outbox_buf != NULL) {
        pebble_rt_free_data(am->outbox_buf, am->outbox_cap);
    }

    am->inbox_buf = new_in;
    am->inbox_cap = in_sz;
    am->outbox_buf = new_out;
    am->outbox_cap = out_sz;
    am->outbox_len = 0u;
    am->outbox_state = OUTBOX_IDLE;
    am->opened = true;
    return PEBBLE_APP_MSG_OK;
}

static uint32_t shim_app_message_open(uint32_t size_inbound, uint32_t size_outbound) {
    pebble_rt_record_syscall(PEBBLE_SYS_APP_MESSAGE_OPEN, true);
    return app_message_open_internal(size_inbound, size_outbound);
}

static void app_message_deregister_callbacks_internal(void) {
    if (g_am == NULL) {
        return;
    }
    g_am->inbox_received = NULL;
    g_am->inbox_dropped = NULL;
    g_am->outbox_sent = NULL;
    g_am->outbox_failed = NULL;
    g_am->context = NULL;
}

static void shim_app_message_deregister_callbacks(void) {
    pebble_rt_record_syscall(PEBBLE_SYS_APP_MESSAGE_DEREGISTER_CALLBACKS, true);
    app_message_deregister_callbacks_internal();
}

static void *shim_app_message_get_context(void) {
    pebble_rt_record_syscall(PEBBLE_SYS_APP_MESSAGE_GET_CONTEXT, true);
    return (g_am != NULL) ? g_am->context : NULL;
}

static void *app_message_set_context_internal(void *context) {
    pebble_app_msg_state_t *am = ensure_state();
    if (am == NULL) {
        return NULL;
    }
    void *prev = am->context;
    am->context = context;
    return prev;
}

static void *shim_app_message_set_context(void *context) {
    pebble_rt_record_syscall(PEBBLE_SYS_APP_MESSAGE_SET_CONTEXT, true);
    return app_message_set_context_internal(context);
}

static uint32_t shim_app_message_inbox_size_maximum(void) {
    pebble_rt_record_syscall(PEBBLE_SYS_APP_MESSAGE_INBOX_SIZE_MAXIMUM, true);
    return PEBBLE_APP_MSG_MAX_SIZE;
}

static uint32_t shim_app_message_outbox_size_maximum(void) {
    pebble_rt_record_syscall(PEBBLE_SYS_APP_MESSAGE_OUTBOX_SIZE_MAXIMUM, true);
    return PEBBLE_APP_MSG_MAX_SIZE;
}

static pebble_app_msg_inbox_received_t app_message_register_inbox_received_internal(
    pebble_app_msg_inbox_received_t cb) {
    pebble_app_msg_state_t *am = ensure_state();
    if (am == NULL) {
        return NULL;
    }
    pebble_app_msg_inbox_received_t prev = am->inbox_received;
    am->inbox_received = cb;
    return prev;
}

static pebble_app_msg_inbox_received_t shim_app_message_register_inbox_received(
    pebble_app_msg_inbox_received_t cb) {
    pebble_rt_record_syscall(PEBBLE_SYS_APP_MESSAGE_REGISTER_INBOX_RECEIVED, true);
    return app_message_register_inbox_received_internal(cb);
}

static pebble_app_msg_inbox_dropped_t app_message_register_inbox_dropped_internal(
    pebble_app_msg_inbox_dropped_t cb) {
    pebble_app_msg_state_t *am = ensure_state();
    if (am == NULL) {
        return NULL;
    }
    pebble_app_msg_inbox_dropped_t prev = am->inbox_dropped;
    am->inbox_dropped = cb;
    return prev;
}

static pebble_app_msg_inbox_dropped_t shim_app_message_register_inbox_dropped(
    pebble_app_msg_inbox_dropped_t cb) {
    pebble_rt_record_syscall(PEBBLE_SYS_APP_MESSAGE_REGISTER_INBOX_DROPPED, true);
    return app_message_register_inbox_dropped_internal(cb);
}

static pebble_app_msg_outbox_sent_t app_message_register_outbox_sent_internal(
    pebble_app_msg_outbox_sent_t cb) {
    pebble_app_msg_state_t *am = ensure_state();
    if (am == NULL) {
        return NULL;
    }
    pebble_app_msg_outbox_sent_t prev = am->outbox_sent;
    am->outbox_sent = cb;
    return prev;
}

static pebble_app_msg_outbox_sent_t shim_app_message_register_outbox_sent(
    pebble_app_msg_outbox_sent_t cb) {
    pebble_rt_record_syscall(PEBBLE_SYS_APP_MESSAGE_REGISTER_OUTBOX_SENT, true);
    return app_message_register_outbox_sent_internal(cb);
}

static pebble_app_msg_outbox_failed_t app_message_register_outbox_failed_internal(
    pebble_app_msg_outbox_failed_t cb) {
    pebble_app_msg_state_t *am = ensure_state();
    if (am == NULL) {
        return NULL;
    }
    pebble_app_msg_outbox_failed_t prev = am->outbox_failed;
    am->outbox_failed = cb;
    return prev;
}

static pebble_app_msg_outbox_failed_t shim_app_message_register_outbox_failed(
    pebble_app_msg_outbox_failed_t cb) {
    pebble_rt_record_syscall(PEBBLE_SYS_APP_MESSAGE_REGISTER_OUTBOX_FAILED, true);
    return app_message_register_outbox_failed_internal(cb);
}

static uint32_t app_message_outbox_begin_internal(pebble_dict_iterator_t **iterator) {
    if (iterator == NULL) {
        return PEBBLE_APP_MSG_INVALID_ARGS;
    }
    if (g_am == NULL || !g_am->opened || g_am->outbox_buf == NULL || g_am->outbox_cap == 0u) {
        return PEBBLE_APP_MSG_CLOSED;
    }
    if (g_am->outbox_state == OUTBOX_QUEUED ||
        g_am->outbox_state == OUTBOX_IN_FLIGHT ||
        g_am->outbox_state == OUTBOX_FAIL_PENDING) {
        return PEBBLE_APP_MSG_BUSY;
    }
    uint32_t dres = pebble_dict_write_begin_internal(
        &g_am->outbox_iter, g_am->outbox_buf, (uint16_t)g_am->outbox_cap);
    if (dres != PEBBLE_DICT_OK) {
        return PEBBLE_APP_MSG_INTERNAL_ERROR;
    }
    g_am->outbox_state = OUTBOX_WRITING;
    *iterator = &g_am->outbox_iter;
    return PEBBLE_APP_MSG_OK;
}

static uint32_t shim_app_message_outbox_begin(pebble_dict_iterator_t **iterator) {
    pebble_rt_record_syscall(PEBBLE_SYS_APP_MESSAGE_OUTBOX_BEGIN, true);
    return app_message_outbox_begin_internal(iterator);
}

static uint32_t app_message_outbox_send_internal(void) {
    if (g_am == NULL || !g_am->opened || g_am->outbox_buf == NULL) {
        return PEBBLE_APP_MSG_CLOSED;
    }
    if (g_am->outbox_state != OUTBOX_WRITING) {
        return PEBBLE_APP_MSG_INVALID_STATE;
    }
    g_am->outbox_len = pebble_dict_write_end_internal(&g_am->outbox_iter);
    if (g_am->outbox_len == 0u) {
        g_am->outbox_state = OUTBOX_IDLE;
        return PEBBLE_APP_MSG_INVALID_ARGS;
    }
    /* Always queue for asynchronous dispatch in app_event_loop() (never re-entrant). */
    g_am->outbox_state = OUTBOX_QUEUED;
    return PEBBLE_APP_MSG_OK;
}

static uint32_t shim_app_message_outbox_send(void) {
    pebble_rt_record_syscall(PEBBLE_SYS_APP_MESSAGE_OUTBOX_SEND, true);
    return app_message_outbox_send_internal();
}

/* --- PebbleOS AppSync Syscall Shims (4 symbols) --- */

static void app_sync_merge_dict(pebble_app_sync_t *s, pebble_dict_iterator_t *iterator) {
    if (s == NULL || !s->is_initialized || iterator == NULL) {
        return;
    }
    uint32_t max_sz = s->buffer_size;
    uint32_t dres = pebble_dict_merge_internal(
        &s->current_iter,
        &max_sz,
        iterator,
        false,
        s->callback.value_changed,
        s->callback.context);
    if (dres != PEBBLE_DICT_OK && s->callback.error != NULL) {
        s->callback.error(dres, PEBBLE_APP_MSG_OK, s->callback.context);
    }
}

static void app_sync_inbox_received(pebble_dict_iterator_t *iterator, void *context) {
    app_sync_merge_dict((pebble_app_sync_t *)context, iterator);
}

static void app_sync_outbox_sent(pebble_dict_iterator_t *iterator, void *context) {
    app_sync_merge_dict((pebble_app_sync_t *)context, iterator);
}

static void app_sync_inbox_dropped(uint32_t reason, void *context) {
    pebble_app_sync_t *s = (pebble_app_sync_t *)context;
    if (s != NULL && s->is_initialized && s->callback.error != NULL) {
        s->callback.error(PEBBLE_DICT_OK, reason, s->callback.context);
    }
}

static void app_sync_outbox_failed(pebble_dict_iterator_t *iterator, uint32_t reason, void *context) {
    (void)iterator;
    pebble_app_sync_t *s = (pebble_app_sync_t *)context;
    if (s != NULL && s->is_initialized && s->callback.error != NULL) {
        s->callback.error(PEBBLE_DICT_OK, reason, s->callback.context);
    }
}

static void shim_app_sync_init(
    pebble_app_sync_t *s,
    uint8_t *buffer,
    uint16_t buffer_size,
    const pebble_tuplet_t *keys_and_initial_values,
    uint8_t count,
    pebble_app_sync_tuple_changed_cb_t tuple_changed_callback,
    pebble_app_sync_error_cb_t error_callback,
    void *context) {
    pebble_rt_record_syscall(PEBBLE_SYS_APP_SYNC_INIT, true);
    if (s == NULL || buffer == NULL || buffer_size < 1u) {
        return;
    }
    am_memset((uint8_t *)s, 0u, sizeof(*s));
    s->buffer = buffer;
    s->buffer_size = buffer_size;
    s->callback.value_changed = tuple_changed_callback;
    s->callback.error = error_callback;
    s->callback.context = context;

    uint32_t dres = pebble_dict_write_begin_internal(&s->current_iter, buffer, buffer_size);
    if (dres == PEBBLE_DICT_OK && keys_and_initial_values != NULL) {
        for (uint8_t i = 0u; i < count; ++i) {
            dres = pebble_dict_write_tuplet_internal(&s->current_iter, &keys_and_initial_values[i]);
            if (dres != PEBBLE_DICT_OK) {
                break;
            }
        }
    }
    if (dres != PEBBLE_DICT_OK) {
        if (error_callback != NULL) {
            error_callback(dres, PEBBLE_APP_MSG_OK, context);
        }
        return;
    }
    uint32_t written = pebble_dict_write_end_internal(&s->current_iter);
    (void)pebble_dict_read_begin_internal(&s->current_iter, buffer, (uint16_t)written);
    s->is_initialized = true;

    (void)app_message_set_context_internal(s);
    (void)app_message_register_inbox_received_internal(app_sync_inbox_received);
    (void)app_message_register_inbox_dropped_internal(app_sync_inbox_dropped);
    (void)app_message_register_outbox_sent_internal(app_sync_outbox_sent);
    (void)app_message_register_outbox_failed_internal(app_sync_outbox_failed);

    if (tuple_changed_callback != NULL) {
        pebble_dict_iterator_t scan = {
            .dictionary = s->current_iter.dictionary,
            .end = s->current_iter.end,
            .cursor = NULL,
        };
        pebble_tuple_t *t = pebble_dict_read_first_internal(&scan);
        while (t != NULL) {
            uint32_t key = pebble_read_u32_le((const uint8_t *)t);
            tuple_changed_callback(key, t, NULL, context);
            t = pebble_dict_read_next_internal(&scan);
        }
    }
}

static void shim_app_sync_deinit(pebble_app_sync_t *s) {
    pebble_rt_record_syscall(PEBBLE_SYS_APP_SYNC_DEINIT, true);
    if (s == NULL || !s->is_initialized) {
        return;
    }
    app_message_deregister_callbacks_internal();
    s->is_initialized = false;
}

static uint32_t shim_app_sync_set(
    pebble_app_sync_t *s,
    const pebble_tuplet_t *keys_and_values_to_update,
    uint8_t count) {
    pebble_rt_record_syscall(PEBBLE_SYS_APP_SYNC_SET, true);
    if (s == NULL || !s->is_initialized || keys_and_values_to_update == NULL) {
        return PEBBLE_APP_MSG_INVALID_ARGS;
    }
    pebble_dict_iterator_t *iter = NULL;
    uint32_t res = app_message_outbox_begin_internal(&iter);
    if (res != PEBBLE_APP_MSG_OK) {
        return res;
    }
    for (uint8_t i = 0u; i < count; ++i) {
        uint32_t dres = pebble_dict_write_tuplet_internal(iter, &keys_and_values_to_update[i]);
        if (dres != PEBBLE_DICT_OK) {
            g_am->outbox_state = OUTBOX_IDLE;
            return (dres == PEBBLE_DICT_NOT_ENOUGH_STORAGE)
                       ? PEBBLE_APP_MSG_BUFFER_OVERFLOW
                       : PEBBLE_APP_MSG_INVALID_ARGS;
        }
    }
    return app_message_outbox_send_internal();
}

static const pebble_tuple_t *shim_app_sync_get(const pebble_app_sync_t *s, uint32_t key) {
    pebble_rt_record_syscall(PEBBLE_SYS_APP_SYNC_GET, true);
    if (s == NULL || !s->is_initialized) {
        return NULL;
    }
    return pebble_dict_find_internal(&s->current_iter, key);
}

void pebble_app_message_register_syscalls(void **table) {
    table[PEBBLE_SYS_APP_MESSAGE_OPEN] = (void *)shim_app_message_open;
    table[PEBBLE_SYS_APP_MESSAGE_DEREGISTER_CALLBACKS] =
        (void *)shim_app_message_deregister_callbacks;
    table[PEBBLE_SYS_APP_MESSAGE_GET_CONTEXT] = (void *)shim_app_message_get_context;
    table[PEBBLE_SYS_APP_MESSAGE_SET_CONTEXT] = (void *)shim_app_message_set_context;
    table[PEBBLE_SYS_APP_MESSAGE_INBOX_SIZE_MAXIMUM] =
        (void *)shim_app_message_inbox_size_maximum;
    table[PEBBLE_SYS_APP_MESSAGE_OUTBOX_SIZE_MAXIMUM] =
        (void *)shim_app_message_outbox_size_maximum;
    table[PEBBLE_SYS_APP_MESSAGE_REGISTER_INBOX_RECEIVED] =
        (void *)shim_app_message_register_inbox_received;
    table[PEBBLE_SYS_APP_MESSAGE_REGISTER_INBOX_DROPPED] =
        (void *)shim_app_message_register_inbox_dropped;
    table[PEBBLE_SYS_APP_MESSAGE_REGISTER_OUTBOX_SENT] =
        (void *)shim_app_message_register_outbox_sent;
    table[PEBBLE_SYS_APP_MESSAGE_REGISTER_OUTBOX_FAILED] =
        (void *)shim_app_message_register_outbox_failed;
    table[PEBBLE_SYS_APP_MESSAGE_OUTBOX_BEGIN] = (void *)shim_app_message_outbox_begin;
    table[PEBBLE_SYS_APP_MESSAGE_OUTBOX_SEND] = (void *)shim_app_message_outbox_send;

    table[PEBBLE_SYS_APP_SYNC_INIT] = (void *)shim_app_sync_init;
    table[PEBBLE_SYS_APP_SYNC_DEINIT] = (void *)shim_app_sync_deinit;
    table[PEBBLE_SYS_APP_SYNC_SET] = (void *)shim_app_sync_set;
    table[PEBBLE_SYS_APP_SYNC_GET] = (void *)shim_app_sync_get;
}
