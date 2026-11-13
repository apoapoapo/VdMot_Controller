/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "vdm/codec.h"

#include <string.h>

#include "vdm/crc16.h"

/* ---------------------------------------------------------------------- */
/* Defaults                                                                */
/* ---------------------------------------------------------------------- */

const vdm_system_config_t vdm_system_config_default = {
    .link_timeout_s = 300,
    .failsafe_pos = VDM_POS_MAX, /* fully open */
    .startup_pos = VDM_POS_HOLD, /* last stored target */
    .sensor_interval_s = 30,
    .recal_after_moves = 2000,
    .recal_after_days = 7,
};

const vdm_motor_params_t vdm_motor_params_default = {
    .endstop_factor_open = 17,
    .endstop_factor_close = 17,
    .endstop_abs_ma = 100,
    .inrush_blank_ms = 250,
    .opencircuit_ma10 = 20,
    .opencircuit_ms = 500,
    .move_timeout_s = 120,
    .min_stroke_counts = 2000,
    .max_calib_retries = 2,
};

/* ---------------------------------------------------------------------- */
/* Writer / reader                                                         */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool err;
} wr_t;

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t pos;
    bool err;
} rd_t;

static void w_u8(wr_t *w, uint8_t v)
{
    if (w->len >= w->cap) {
        w->err = true;
        return;
    }
    w->buf[w->len++] = v;
}

static void w_u16(wr_t *w, uint16_t v)
{
    w_u8(w, (uint8_t)(v & 0xFFu));
    w_u8(w, (uint8_t)(v >> 8));
}

static void w_u32(wr_t *w, uint32_t v)
{
    w_u16(w, (uint16_t)(v & 0xFFFFu));
    w_u16(w, (uint16_t)(v >> 16));
}

static void w_bytes(wr_t *w, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        w_u8(w, p[i]);
    }
}

static bool w_done(const wr_t *w, size_t *out_len)
{
    if (w->err) {
        return false;
    }
    *out_len = w->len;
    return true;
}

static uint8_t r_u8(rd_t *r)
{
    if (r->pos >= r->len) {
        r->err = true;
        return 0;
    }
    return r->buf[r->pos++];
}

static uint16_t r_u16(rd_t *r)
{
    uint16_t lo = r_u8(r);
    uint16_t hi = r_u8(r);
    return (uint16_t)(lo | (hi << 8));
}

static uint32_t r_u32(rd_t *r)
{
    uint32_t lo = r_u16(r);
    uint32_t hi = r_u16(r);
    return lo | (hi << 16);
}

static void r_bytes(rd_t *r, uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = r_u8(r);
    }
}

#define WR_INIT(out, cap)                                                                          \
    {                                                                                              \
        (out), (cap), 0, false                                                                     \
    }
#define RD_INIT(p, len)                                                                            \
    {                                                                                              \
        (p), (len), 0, false                                                                       \
    }

static vdm_status_t range_err(uint8_t *bad, uint8_t index)
{
    if (bad != NULL) {
        *bad = index;
    }
    return VDM_ERR_RANGE;
}

static vdm_status_t channel_err(uint8_t *bad, uint8_t index)
{
    if (bad != NULL) {
        *bad = index;
    }
    return VDM_ERR_CHANNEL;
}

static bool in_range(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v >= lo && v <= hi;
}

static bool is_pos_or(uint16_t v, uint16_t special)
{
    return v <= VDM_POS_MAX || v == special;
}

/* ---------------------------------------------------------------------- */
/* Responses                                                               */
/* ---------------------------------------------------------------------- */

static bool status_has_detail(uint8_t status)
{
    return status == VDM_ERR_RANGE || status == VDM_ERR_CHANNEL;
}

bool vdm_enc_response(uint8_t status, uint8_t detail, const uint8_t *body, size_t body_len,
                      uint8_t *out, size_t cap, size_t *out_len)
{
    wr_t w = WR_INIT(out, cap);
    w_u8(&w, status);
    if (status_has_detail(status)) {
        w_u8(&w, detail);
    }
    if (status == VDM_OK && body_len > 0) {
        if (body == NULL) {
            return false;
        }
        w_bytes(&w, body, body_len);
    }
    return w_done(&w, out_len);
}

vdm_status_t vdm_dec_response(const uint8_t *p, size_t len, uint8_t *status, uint8_t *detail,
                              const uint8_t **body, size_t *body_len)
{
    if (len == 0) {
        return VDM_ERR_BAD_LEN;
    }
    *status = p[0];
    *detail = 0;
    *body = NULL;
    *body_len = 0;
    if (status_has_detail(p[0])) {
        if (len != 2) {
            return VDM_ERR_BAD_LEN;
        }
        *detail = p[1];
    } else if (p[0] == VDM_OK) {
        *body = (len > 1) ? &p[1] : NULL;
        *body_len = len - 1;
    } else if (len != 1) {
        return VDM_ERR_BAD_LEN;
    }
    return VDM_OK;
}

/* ---------------------------------------------------------------------- */
/* Small bodies                                                            */
/* ---------------------------------------------------------------------- */

bool vdm_enc_mask(uint16_t mask, uint8_t *out, size_t cap, size_t *out_len)
{
    if (mask & ~VDM_CHANNEL_MASK_ALL) {
        return false;
    }
    return vdm_enc_u16(mask, out, cap, out_len);
}

vdm_status_t vdm_dec_mask(const uint8_t *p, size_t len, uint16_t *mask)
{
    vdm_status_t st = vdm_dec_u16(p, len, mask);
    if (st != VDM_OK) {
        return st;
    }
    return (*mask & ~VDM_CHANNEL_MASK_ALL) ? VDM_ERR_CHANNEL : VDM_OK;
}

bool vdm_enc_channel(uint8_t channel, uint8_t *out, size_t cap, size_t *out_len)
{
    if (channel >= VDM_MAX_CHANNELS) {
        return false;
    }
    wr_t w = WR_INIT(out, cap);
    w_u8(&w, channel);
    return w_done(&w, out_len);
}

vdm_status_t vdm_dec_channel(const uint8_t *p, size_t len, uint8_t *channel)
{
    if (len != 1) {
        return VDM_ERR_BAD_LEN;
    }
    *channel = p[0];
    return (*channel < VDM_MAX_CHANNELS) ? VDM_OK : VDM_ERR_CHANNEL;
}

bool vdm_enc_offset(uint8_t offset, uint8_t *out, size_t cap, size_t *out_len)
{
    wr_t w = WR_INIT(out, cap);
    w_u8(&w, offset);
    return w_done(&w, out_len);
}

vdm_status_t vdm_dec_offset(const uint8_t *p, size_t len, uint8_t *offset)
{
    if (len != 1) {
        return VDM_ERR_BAD_LEN;
    }
    *offset = p[0];
    return VDM_OK;
}

bool vdm_enc_u16(uint16_t v, uint8_t *out, size_t cap, size_t *out_len)
{
    wr_t w = WR_INIT(out, cap);
    w_u16(&w, v);
    return w_done(&w, out_len);
}

vdm_status_t vdm_dec_u16(const uint8_t *p, size_t len, uint16_t *v)
{
    if (len != 2) {
        return VDM_ERR_BAD_LEN;
    }
    rd_t r = RD_INIT(p, len);
    *v = r_u16(&r);
    return VDM_OK;
}

bool vdm_enc_reset(uint8_t *out, size_t cap, size_t *out_len)
{
    wr_t w = WR_INIT(out, cap);
    w_u32(&w, VDM_RESET_MAGIC);
    return w_done(&w, out_len);
}

vdm_status_t vdm_dec_reset(const uint8_t *p, size_t len)
{
    if (len != VDM_RESET_LEN) {
        return VDM_ERR_BAD_LEN;
    }
    rd_t r = RD_INIT(p, len);
    return (r_u32(&r) == VDM_RESET_MAGIC) ? VDM_OK : VDM_ERR_RANGE;
}

/* ---------------------------------------------------------------------- */
/* HELLO                                                                   */
/* ---------------------------------------------------------------------- */

bool vdm_enc_hello_req(const vdm_hello_req_t *in, uint8_t *out, size_t cap, size_t *out_len)
{
    wr_t w = WR_INIT(out, cap);
    w_u8(&w, in->esp_proto_version);
    w_u8(&w, in->flags);
    return w_done(&w, out_len);
}

vdm_status_t vdm_dec_hello_req(const uint8_t *p, size_t len, vdm_hello_req_t *out)
{
    if (len != VDM_HELLO_REQ_LEN) {
        return VDM_ERR_BAD_LEN;
    }
    out->esp_proto_version = p[0];
    out->flags = p[1];
    return VDM_OK;
}

bool vdm_enc_hello(const vdm_hello_t *in, uint8_t *out, size_t cap, size_t *out_len)
{
    wr_t w = WR_INIT(out, cap);
    w_u8(&w, in->proto_version);
    w_u8(&w, in->fw_major);
    w_u8(&w, in->fw_minor);
    w_u8(&w, in->fw_patch);
    w_u32(&w, in->fw_build);
    w_u8(&w, in->hw_rev);
    w_u8(&w, in->mcu);
    w_bytes(&w, in->mcu_uid, sizeof(in->mcu_uid));
    w_u8(&w, in->channel_count);
    w_u32(&w, in->capabilities);
    w_u8(&w, in->reset_cause);
    w_u32(&w, in->boot_count);
    w_u16(&w, in->config_crc);
    return w_done(&w, out_len);
}

vdm_status_t vdm_dec_hello(const uint8_t *p, size_t len, vdm_hello_t *out)
{
    if (len != VDM_HELLO_LEN) {
        return VDM_ERR_BAD_LEN;
    }
    rd_t r = RD_INIT(p, len);
    out->proto_version = r_u8(&r);
    out->fw_major = r_u8(&r);
    out->fw_minor = r_u8(&r);
    out->fw_patch = r_u8(&r);
    out->fw_build = r_u32(&r);
    out->hw_rev = r_u8(&r);
    out->mcu = r_u8(&r);
    r_bytes(&r, out->mcu_uid, sizeof(out->mcu_uid));
    out->channel_count = r_u8(&r);
    out->capabilities = r_u32(&r);
    out->reset_cause = r_u8(&r);
    out->boot_count = r_u32(&r);
    out->config_crc = r_u16(&r);
    return VDM_OK;
}

/* ---------------------------------------------------------------------- */
/* System config                                                           */
/* ---------------------------------------------------------------------- */

static vdm_status_t check_system_config(const vdm_system_config_t *c, uint8_t *bad)
{
    if (c->link_timeout_s != 0 && !in_range(c->link_timeout_s, 10, 3600)) {
        return range_err(bad, 0);
    }
    if (!is_pos_or(c->failsafe_pos, VDM_POS_HOLD)) {
        return range_err(bad, 1);
    }
    if (!is_pos_or(c->startup_pos, VDM_POS_HOLD)) {
        return range_err(bad, 2);
    }
    if (!in_range(c->sensor_interval_s, 5, 3600)) {
        return range_err(bad, 3);
    }
    if (c->recal_after_moves != 0 && !in_range(c->recal_after_moves, 50, 10000)) {
        return range_err(bad, 4);
    }
    if (c->recal_after_days > 60) {
        return range_err(bad, 5);
    }
    return VDM_OK;
}

bool vdm_enc_system_config(const vdm_system_config_t *in, uint8_t *out, size_t cap, size_t *out_len)
{
    if (check_system_config(in, NULL) != VDM_OK) {
        return false;
    }
    wr_t w = WR_INIT(out, cap);
    w_u16(&w, in->link_timeout_s);
    w_u16(&w, in->failsafe_pos);
    w_u16(&w, in->startup_pos);
    w_u16(&w, in->sensor_interval_s);
    w_u16(&w, in->recal_after_moves);
    w_u8(&w, in->recal_after_days);
    return w_done(&w, out_len);
}

vdm_status_t vdm_dec_system_config(const uint8_t *p, size_t len, vdm_system_config_t *out,
                                   uint8_t *bad)
{
    if (len != VDM_SYSTEM_CONFIG_LEN) {
        return VDM_ERR_BAD_LEN;
    }
    rd_t r = RD_INIT(p, len);
    out->link_timeout_s = r_u16(&r);
    out->failsafe_pos = r_u16(&r);
    out->startup_pos = r_u16(&r);
    out->sensor_interval_s = r_u16(&r);
    out->recal_after_moves = r_u16(&r);
    out->recal_after_days = r_u8(&r);
    return check_system_config(out, bad);
}

/* ---------------------------------------------------------------------- */
/* Motor params                                                            */
/* ---------------------------------------------------------------------- */

static vdm_status_t check_motor_params(const vdm_motor_params_t *m, uint8_t *bad)
{
    if (!in_range(m->endstop_factor_open, 11, 50)) {
        return range_err(bad, 0);
    }
    if (!in_range(m->endstop_factor_close, 11, 50)) {
        return range_err(bad, 1);
    }
    if (!in_range(m->endstop_abs_ma, 20, 300)) {
        return range_err(bad, 2);
    }
    if (!in_range(m->inrush_blank_ms, 50, 1000)) {
        return range_err(bad, 3);
    }
    if (!in_range(m->opencircuit_ma10, 5, 100)) {
        return range_err(bad, 4);
    }
    if (!in_range(m->opencircuit_ms, 100, 5000)) {
        return range_err(bad, 5);
    }
    if (!in_range(m->move_timeout_s, 10, 600)) {
        return range_err(bad, 6);
    }
    if (!in_range(m->min_stroke_counts, 100, 60000)) {
        return range_err(bad, 7);
    }
    if (m->max_calib_retries > 5) {
        return range_err(bad, 8);
    }
    return VDM_OK;
}

bool vdm_enc_motor_params(const vdm_motor_params_t *in, uint8_t *out, size_t cap, size_t *out_len)
{
    if (check_motor_params(in, NULL) != VDM_OK) {
        return false;
    }
    wr_t w = WR_INIT(out, cap);
    w_u8(&w, in->endstop_factor_open);
    w_u8(&w, in->endstop_factor_close);
    w_u16(&w, in->endstop_abs_ma);
    w_u16(&w, in->inrush_blank_ms);
    w_u16(&w, in->opencircuit_ma10);
    w_u16(&w, in->opencircuit_ms);
    w_u16(&w, in->move_timeout_s);
    w_u16(&w, in->min_stroke_counts);
    w_u8(&w, in->max_calib_retries);
    return w_done(&w, out_len);
}

vdm_status_t vdm_dec_motor_params(const uint8_t *p, size_t len, vdm_motor_params_t *out,
                                  uint8_t *bad)
{
    if (len != VDM_MOTOR_PARAMS_LEN) {
        return VDM_ERR_BAD_LEN;
    }
    rd_t r = RD_INIT(p, len);
    out->endstop_factor_open = r_u8(&r);
    out->endstop_factor_close = r_u8(&r);
    out->endstop_abs_ma = r_u16(&r);
    out->inrush_blank_ms = r_u16(&r);
    out->opencircuit_ma10 = r_u16(&r);
    out->opencircuit_ms = r_u16(&r);
    out->move_timeout_s = r_u16(&r);
    out->min_stroke_counts = r_u16(&r);
    out->max_calib_retries = r_u8(&r);
    return check_motor_params(out, bad);
}

/* ---------------------------------------------------------------------- */
/* Valve state                                                             */
/* ---------------------------------------------------------------------- */

/* Field indices: 0 channel, 1 mode, 2 flags, 3 fault, 4 actual, 5 target */
static vdm_status_t check_valve_state(const vdm_valve_state_t *s, uint8_t *bad)
{
    if (s->channel >= VDM_MAX_CHANNELS) {
        return channel_err(bad, 0);
    }
    if (s->mode > VDM_MODE_LAST) {
        return range_err(bad, 1);
    }
    if (s->flags & ~VDM_VFLAG_MASK) {
        return range_err(bad, 2);
    }
    if (s->fault > VDM_FAULT_LAST) {
        return range_err(bad, 3);
    }
    if (!is_pos_or(s->actual, VDM_POS_UNKNOWN)) {
        return range_err(bad, 4);
    }
    if (s->target > VDM_POS_MAX) {
        return range_err(bad, 5);
    }
    return VDM_OK;
}

static void put_valve_state(wr_t *w, const vdm_valve_state_t *s)
{
    w_u8(w, s->channel);
    w_u8(w, s->mode);
    w_u8(w, s->flags);
    w_u8(w, s->fault);
    w_u16(w, s->actual);
    w_u16(w, s->target);
    w_u16(w, s->current_ma10);
}

static void get_valve_state(rd_t *r, vdm_valve_state_t *s)
{
    s->channel = r_u8(r);
    s->mode = r_u8(r);
    s->flags = r_u8(r);
    s->fault = r_u8(r);
    s->actual = r_u16(r);
    s->target = r_u16(r);
    s->current_ma10 = r_u16(r);
}

bool vdm_enc_valve_state(const vdm_valve_state_t *in, uint8_t *out, size_t cap, size_t *out_len)
{
    if (check_valve_state(in, NULL) != VDM_OK) {
        return false;
    }
    wr_t w = WR_INIT(out, cap);
    put_valve_state(&w, in);
    return w_done(&w, out_len);
}

vdm_status_t vdm_dec_valve_state(const uint8_t *p, size_t len, vdm_valve_state_t *out, uint8_t *bad)
{
    if (len != VDM_VALVE_STATE_LEN) {
        return VDM_ERR_BAD_LEN;
    }
    rd_t r = RD_INIT(p, len);
    get_valve_state(&r, out);
    return check_valve_state(out, bad);
}

/* For lists, `bad` receives the entry index. */
bool vdm_enc_valve_states(const vdm_valve_states_t *in, uint8_t *out, size_t cap, size_t *out_len)
{
    if (in->count > VDM_MAX_CHANNELS) {
        return false;
    }
    wr_t w = WR_INIT(out, cap);
    w_u8(&w, in->count);
    for (uint8_t i = 0; i < in->count; i++) {
        if (check_valve_state(&in->valves[i], NULL) != VDM_OK) {
            return false;
        }
        put_valve_state(&w, &in->valves[i]);
    }
    return w_done(&w, out_len);
}

vdm_status_t vdm_dec_valve_states(const uint8_t *p, size_t len, vdm_valve_states_t *out,
                                  uint8_t *bad)
{
    if (len < 1) {
        return VDM_ERR_BAD_LEN;
    }
    uint8_t count = p[0];
    if (count > VDM_MAX_CHANNELS) {
        return range_err(bad, 0);
    }
    if (len != 1u + (size_t)count * VDM_VALVE_STATE_LEN) {
        return VDM_ERR_BAD_LEN;
    }
    rd_t r = RD_INIT(p, len);
    out->count = r_u8(&r);
    for (uint8_t i = 0; i < count; i++) {
        get_valve_state(&r, &out->valves[i]);
        vdm_status_t st = check_valve_state(&out->valves[i], NULL);
        if (st == VDM_ERR_CHANNEL) {
            return channel_err(bad, i);
        }
        if (st != VDM_OK) {
            return range_err(bad, i);
        }
    }
    return VDM_OK;
}

/* ---------------------------------------------------------------------- */
/* SET_TARGETS                                                             */
/* ---------------------------------------------------------------------- */

bool vdm_enc_set_targets(const vdm_set_targets_t *in, uint8_t *out, size_t cap, size_t *out_len)
{
    if (in->count < 1 || in->count > VDM_MAX_CHANNELS) {
        return false;
    }
    wr_t w = WR_INIT(out, cap);
    w_u8(&w, in->count);
    for (uint8_t i = 0; i < in->count; i++) {
        if (in->targets[i].channel >= VDM_MAX_CHANNELS || in->targets[i].target > VDM_POS_MAX) {
            return false;
        }
        w_u8(&w, in->targets[i].channel);
        w_u16(&w, in->targets[i].target);
    }
    return w_done(&w, out_len);
}

vdm_status_t vdm_dec_set_targets(const uint8_t *p, size_t len, vdm_set_targets_t *out, uint8_t *bad)
{
    if (len < 1) {
        return VDM_ERR_BAD_LEN;
    }
    uint8_t count = p[0];
    if (count < 1 || count > VDM_MAX_CHANNELS) {
        return range_err(bad, 0);
    }
    if (len != 1u + (size_t)count * 3u) {
        return VDM_ERR_BAD_LEN;
    }
    rd_t r = RD_INIT(p, len);
    out->count = r_u8(&r);
    for (uint8_t i = 0; i < count; i++) {
        out->targets[i].channel = r_u8(&r);
        out->targets[i].target = r_u16(&r);
        if (out->targets[i].channel >= VDM_MAX_CHANNELS) {
            return channel_err(bad, i);
        }
        if (out->targets[i].target > VDM_POS_MAX) {
            return range_err(bad, i);
        }
    }
    return VDM_OK;
}

/* ---------------------------------------------------------------------- */
/* Valve detail                                                            */
/* ---------------------------------------------------------------------- */

bool vdm_enc_valve_detail(const vdm_valve_detail_t *in, uint8_t *out, size_t cap, size_t *out_len)
{
    if (check_valve_state(&in->state, NULL) != VDM_OK || in->last_fault > VDM_FAULT_LAST) {
        return false;
    }
    wr_t w = WR_INIT(out, cap);
    w_u8(&w, in->state.channel);
    put_valve_state(&w, &in->state);
    w_u32(&w, in->open_counts);
    w_u32(&w, in->close_counts);
    w_u32(&w, (uint32_t)in->deadzone_counts);
    w_u32(&w, in->calib_age_h);
    w_u32(&w, in->moves_since_calib);
    w_u32(&w, in->moves_total);
    w_u16(&w, in->mean_current_open_ma10);
    w_u16(&w, in->mean_current_close_ma10);
    w_u16(&w, in->peak_current_ma10);
    w_u32(&w, (uint32_t)in->last_move_counts);
    w_u32(&w, in->last_move_ms);
    w_u8(&w, in->calib_retries);
    w_u8(&w, in->last_fault);
    return w_done(&w, out_len);
}

vdm_status_t vdm_dec_valve_detail(const uint8_t *p, size_t len, vdm_valve_detail_t *out,
                                  uint8_t *bad)
{
    if (len != VDM_VALVE_DETAIL_LEN) {
        return VDM_ERR_BAD_LEN;
    }
    rd_t r = RD_INIT(p, len);
    uint8_t channel = r_u8(&r);
    get_valve_state(&r, &out->state);
    out->open_counts = r_u32(&r);
    out->close_counts = r_u32(&r);
    out->deadzone_counts = (int32_t)r_u32(&r);
    out->calib_age_h = r_u32(&r);
    out->moves_since_calib = r_u32(&r);
    out->moves_total = r_u32(&r);
    out->mean_current_open_ma10 = r_u16(&r);
    out->mean_current_close_ma10 = r_u16(&r);
    out->peak_current_ma10 = r_u16(&r);
    out->last_move_counts = (int32_t)r_u32(&r);
    out->last_move_ms = r_u32(&r);
    out->calib_retries = r_u8(&r);
    out->last_fault = r_u8(&r);

    if (channel != out->state.channel) {
        return channel_err(bad, 0);
    }
    vdm_status_t st = check_valve_state(&out->state, bad);
    if (st != VDM_OK) {
        return st;
    }
    if (out->last_fault > VDM_FAULT_LAST) {
        return range_err(bad, 6);
    }
    return VDM_OK;
}

/* ---------------------------------------------------------------------- */
/* Sensors                                                                 */
/* ---------------------------------------------------------------------- */

static bool sensor_valid(const vdm_sensor_value_t *s)
{
    return in_range(s->kind, VDM_SENSOR_TEMP_DS18B20, VDM_SENSOR_TEMP_DS2438) &&
           s->status <= VDM_SSTAT_OUT_OF_RANGE;
}

bool vdm_enc_sensors(const vdm_sensors_t *in, uint8_t *out, size_t cap, size_t *out_len)
{
    if (in->count > VDM_SENSORS_PER_MSG) {
        return false;
    }
    wr_t w = WR_INIT(out, cap);
    w_u8(&w, in->total);
    w_u8(&w, in->offset);
    w_u8(&w, in->count);
    for (uint8_t i = 0; i < in->count; i++) {
        const vdm_sensor_value_t *s = &in->sensors[i];
        if (!sensor_valid(s)) {
            return false;
        }
        w_bytes(&w, s->rom, sizeof(s->rom));
        w_u8(&w, s->kind);
        w_u8(&w, s->status);
        w_u16(&w, s->age_s);
        w_u32(&w, (uint32_t)s->value);
    }
    return w_done(&w, out_len);
}

vdm_status_t vdm_dec_sensors(const uint8_t *p, size_t len, vdm_sensors_t *out, uint8_t *bad)
{
    if (len < 3) {
        return VDM_ERR_BAD_LEN;
    }
    uint8_t count = p[2];
    if (count > VDM_SENSORS_PER_MSG) {
        return range_err(bad, 0);
    }
    if (len != 3u + (size_t)count * VDM_SENSOR_VALUE_LEN) {
        return VDM_ERR_BAD_LEN;
    }
    rd_t r = RD_INIT(p, len);
    out->total = r_u8(&r);
    out->offset = r_u8(&r);
    out->count = r_u8(&r);
    for (uint8_t i = 0; i < count; i++) {
        vdm_sensor_value_t *s = &out->sensors[i];
        r_bytes(&r, s->rom, sizeof(s->rom));
        s->kind = r_u8(&r);
        s->status = r_u8(&r);
        s->age_s = r_u16(&r);
        s->value = (int32_t)r_u32(&r);
        if (!sensor_valid(s)) {
            return range_err(bad, i);
        }
    }
    return VDM_OK;
}

/* ---------------------------------------------------------------------- */
/* Diagnostics                                                             */
/* ---------------------------------------------------------------------- */

bool vdm_enc_diag(const vdm_diag_t *in, uint8_t *out, size_t cap, size_t *out_len)
{
    wr_t w = WR_INIT(out, cap);
    w_u32(&w, in->uptime_s);
    w_u32(&w, in->rx_frames_ok);
    w_u16(&w, in->rx_crc_err);
    w_u16(&w, in->rx_malformed);
    w_u16(&w, in->rx_overflow);
    w_u16(&w, in->rx_bad_version);
    w_u16(&w, in->rx_duplicate);
    w_u16(&w, in->uart_hw_err);
    w_u16(&w, in->ow_crc_err);
    w_u16(&w, in->ow_bus_err);
    w_u32(&w, in->eeprom_writes);
    w_u16(&w, in->eeprom_err);
    w_u32(&w, in->loop_max_us);
    w_u16(&w, (uint16_t)in->mcu_temp_c10);
    return w_done(&w, out_len);
}

vdm_status_t vdm_dec_diag(const uint8_t *p, size_t len, vdm_diag_t *out)
{
    if (len != VDM_DIAG_LEN) {
        return VDM_ERR_BAD_LEN;
    }
    rd_t r = RD_INIT(p, len);
    out->uptime_s = r_u32(&r);
    out->rx_frames_ok = r_u32(&r);
    out->rx_crc_err = r_u16(&r);
    out->rx_malformed = r_u16(&r);
    out->rx_overflow = r_u16(&r);
    out->rx_bad_version = r_u16(&r);
    out->rx_duplicate = r_u16(&r);
    out->uart_hw_err = r_u16(&r);
    out->ow_crc_err = r_u16(&r);
    out->ow_bus_err = r_u16(&r);
    out->eeprom_writes = r_u32(&r);
    out->eeprom_err = r_u16(&r);
    out->loop_max_us = r_u32(&r);
    out->mcu_temp_c10 = (int16_t)r_u16(&r);
    return VDM_OK;
}

/* ---------------------------------------------------------------------- */
/* Events                                                                  */
/* ---------------------------------------------------------------------- */

bool vdm_enc_evt_calibration(const vdm_evt_calibration_t *in, uint8_t *out, size_t cap,
                             size_t *out_len)
{
    if (in->channel >= VDM_MAX_CHANNELS || in->result > VDM_FAULT_LAST) {
        return false;
    }
    wr_t w = WR_INIT(out, cap);
    w_u8(&w, in->channel);
    w_u8(&w, in->result);
    w_u32(&w, in->open_counts);
    w_u32(&w, in->close_counts);
    w_u8(&w, in->retries);
    return w_done(&w, out_len);
}

vdm_status_t vdm_dec_evt_calibration(const uint8_t *p, size_t len, vdm_evt_calibration_t *out,
                                     uint8_t *bad)
{
    if (len != VDM_EVT_CALIBRATION_LEN) {
        return VDM_ERR_BAD_LEN;
    }
    rd_t r = RD_INIT(p, len);
    out->channel = r_u8(&r);
    out->result = r_u8(&r);
    out->open_counts = r_u32(&r);
    out->close_counts = r_u32(&r);
    out->retries = r_u8(&r);
    if (out->channel >= VDM_MAX_CHANNELS) {
        return channel_err(bad, 0);
    }
    if (out->result > VDM_FAULT_LAST) {
        return range_err(bad, 1);
    }
    return VDM_OK;
}

bool vdm_enc_evt_scan_done(const vdm_evt_scan_done_t *in, uint8_t *out, size_t cap, size_t *out_len)
{
    wr_t w = WR_INIT(out, cap);
    w_u8(&w, in->total);
    w_u8(&w, in->ds18_count);
    w_u8(&w, in->ds2438_count);
    w_u8(&w, in->bus_ok ? 1u : 0u);
    return w_done(&w, out_len);
}

vdm_status_t vdm_dec_evt_scan_done(const uint8_t *p, size_t len, vdm_evt_scan_done_t *out)
{
    if (len != VDM_EVT_SCAN_DONE_LEN) {
        return VDM_ERR_BAD_LEN;
    }
    out->total = p[0];
    out->ds18_count = p[1];
    out->ds2438_count = p[2];
    out->bus_ok = p[3] ? 1u : 0u;
    return VDM_OK;
}

bool vdm_enc_evt_log(uint8_t level, const char *text, size_t text_len, uint8_t *out, size_t cap,
                     size_t *out_len)
{
    if (level > VDM_LOG_DEBUG || text_len > VDM_LOG_TEXT_MAX || (text_len > 0 && text == NULL)) {
        return false;
    }
    wr_t w = WR_INIT(out, cap);
    w_u8(&w, level);
    w_bytes(&w, (const uint8_t *)text, text_len);
    return w_done(&w, out_len);
}

vdm_status_t vdm_dec_evt_log(const uint8_t *p, size_t len, vdm_evt_log_t *out)
{
    if (len < 1 || len > 1u + VDM_LOG_TEXT_MAX) {
        return VDM_ERR_BAD_LEN;
    }
    if (p[0] > VDM_LOG_DEBUG) {
        return VDM_ERR_RANGE;
    }
    out->level = p[0];
    out->len = (uint8_t)(len - 1);
    memcpy(out->text, &p[1], out->len);
    out->text[out->len] = '\0';
    return VDM_OK;
}

/* ---------------------------------------------------------------------- */
/* Helpers                                                                 */
/* ---------------------------------------------------------------------- */

uint16_t vdm_config_crc(const vdm_system_config_t *sys, const vdm_motor_params_t *motor)
{
    uint8_t buf[VDM_SYSTEM_CONFIG_LEN + VDM_MOTOR_PARAMS_LEN];
    size_t a = 0;
    size_t b = 0;
    /* Invalid configs never become active, so encoding cannot fail here;
     * if it does anyway, the CRC simply will not match. */
    (void)vdm_enc_system_config(sys, buf, VDM_SYSTEM_CONFIG_LEN, &a);
    (void)vdm_enc_motor_params(motor, &buf[a], VDM_MOTOR_PARAMS_LEN, &b);
    return vdm_crc16(buf, a + b);
}
