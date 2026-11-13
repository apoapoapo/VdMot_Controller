/*
 * Payload codecs: field-by-field, little-endian (de)serialization of all
 * message bodies of protocol version 1.
 *
 * Encoders: return false if `cap` is too small or the input is invalid.
 * Decoders: validate length (exact) and value ranges. On VDM_ERR_RANGE or
 *           VDM_ERR_CHANNEL the index of the first offending field/entry is
 *           written to `*bad` (if `bad` is not NULL). The output struct may
 *           be partially written on error.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef VDM_CODEC_H
#define VDM_CODEC_H

#include "vdm/link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Encoded body sizes */
#define VDM_HELLO_REQ_LEN 2u
#define VDM_HELLO_LEN 34u
#define VDM_RESET_LEN 4u
#define VDM_SYSTEM_CONFIG_LEN 11u
#define VDM_MOTOR_PARAMS_LEN 15u
#define VDM_VALVE_STATE_LEN 10u
#define VDM_VALVE_DETAIL_LEN 51u
#define VDM_SENSOR_VALUE_LEN 16u
#define VDM_DIAG_LEN 36u
#define VDM_EVT_CALIBRATION_LEN 11u
#define VDM_EVT_SCAN_DONE_LEN 4u

/* ---------------------------------------------------------------------- */
/* Responses: [status] [detail]? [body]?                                   */
/* ---------------------------------------------------------------------- */

/* Builds a response payload. `detail` (the offending index) is only written
 * for VDM_ERR_RANGE and VDM_ERR_CHANNEL; `body` only for VDM_OK. */
bool vdm_enc_response(uint8_t status, uint8_t detail, const uint8_t *body, size_t body_len,
                      uint8_t *out, size_t cap, size_t *out_len);

/* Splits a response payload. Returns VDM_ERR_BAD_LEN if it is empty or an
 * error response lacks its detail byte, otherwise VDM_OK (the decoded
 * status itself is returned in `*status`). */
vdm_status_t vdm_dec_response(const uint8_t *p, size_t len, uint8_t *status, uint8_t *detail,
                              const uint8_t **body, size_t *body_len);

/* ---------------------------------------------------------------------- */
/* Small bodies                                                            */
/* ---------------------------------------------------------------------- */

/* u16 channel mask: CALIBRATE, STOP, SET_CHANNELS, DETECT */
bool vdm_enc_mask(uint16_t mask, uint8_t *out, size_t cap, size_t *out_len);
vdm_status_t vdm_dec_mask(const uint8_t *p, size_t len, uint16_t *mask);

/* u8 channel: GET_VALVE_DETAIL request */
bool vdm_enc_channel(uint8_t channel, uint8_t *out, size_t cap, size_t *out_len);
vdm_status_t vdm_dec_channel(const uint8_t *p, size_t len, uint8_t *channel);

/* u8 offset: GET_SENSORS request */
bool vdm_enc_offset(uint8_t offset, uint8_t *out, size_t cap, size_t *out_len);
vdm_status_t vdm_dec_offset(const uint8_t *p, size_t len, uint8_t *offset);

/* u16: config_crc response body, EVT_LINK_TIMEOUT payload (failsafe_pos) */
bool vdm_enc_u16(uint16_t v, uint8_t *out, size_t cap, size_t *out_len);
vdm_status_t vdm_dec_u16(const uint8_t *p, size_t len, uint16_t *v);

/* RESET: decoder checks the magic (VDM_ERR_RANGE otherwise) */
bool vdm_enc_reset(uint8_t *out, size_t cap, size_t *out_len);
vdm_status_t vdm_dec_reset(const uint8_t *p, size_t len);

/* ---------------------------------------------------------------------- */
/* Structured bodies                                                       */
/* ---------------------------------------------------------------------- */

bool vdm_enc_hello_req(const vdm_hello_req_t *in, uint8_t *out, size_t cap, size_t *out_len);
vdm_status_t vdm_dec_hello_req(const uint8_t *p, size_t len, vdm_hello_req_t *out);

/* HELLO response body and EVT_BOOT payload */
bool vdm_enc_hello(const vdm_hello_t *in, uint8_t *out, size_t cap, size_t *out_len);
vdm_status_t vdm_dec_hello(const uint8_t *p, size_t len, vdm_hello_t *out);

bool vdm_enc_system_config(const vdm_system_config_t *in, uint8_t *out, size_t cap,
                           size_t *out_len);
vdm_status_t vdm_dec_system_config(const uint8_t *p, size_t len, vdm_system_config_t *out,
                                   uint8_t *bad);

bool vdm_enc_motor_params(const vdm_motor_params_t *in, uint8_t *out, size_t cap, size_t *out_len);
vdm_status_t vdm_dec_motor_params(const uint8_t *p, size_t len, vdm_motor_params_t *out,
                                  uint8_t *bad);

/* EVT_VALVE payload */
bool vdm_enc_valve_state(const vdm_valve_state_t *in, uint8_t *out, size_t cap, size_t *out_len);
vdm_status_t vdm_dec_valve_state(const uint8_t *p, size_t len, vdm_valve_state_t *out,
                                 uint8_t *bad);

/* GET_VALVE_STATES response body */
bool vdm_enc_valve_states(const vdm_valve_states_t *in, uint8_t *out, size_t cap, size_t *out_len);
vdm_status_t vdm_dec_valve_states(const uint8_t *p, size_t len, vdm_valve_states_t *out,
                                  uint8_t *bad);

bool vdm_enc_set_targets(const vdm_set_targets_t *in, uint8_t *out, size_t cap, size_t *out_len);
vdm_status_t vdm_dec_set_targets(const uint8_t *p, size_t len, vdm_set_targets_t *out,
                                 uint8_t *bad);

bool vdm_enc_valve_detail(const vdm_valve_detail_t *in, uint8_t *out, size_t cap, size_t *out_len);
vdm_status_t vdm_dec_valve_detail(const uint8_t *p, size_t len, vdm_valve_detail_t *out,
                                  uint8_t *bad);

/* GET_SENSORS response body and EVT_SENSORS payload */
bool vdm_enc_sensors(const vdm_sensors_t *in, uint8_t *out, size_t cap, size_t *out_len);
vdm_status_t vdm_dec_sensors(const uint8_t *p, size_t len, vdm_sensors_t *out, uint8_t *bad);

bool vdm_enc_diag(const vdm_diag_t *in, uint8_t *out, size_t cap, size_t *out_len);
vdm_status_t vdm_dec_diag(const uint8_t *p, size_t len, vdm_diag_t *out);

bool vdm_enc_evt_calibration(const vdm_evt_calibration_t *in, uint8_t *out, size_t cap,
                             size_t *out_len);
vdm_status_t vdm_dec_evt_calibration(const uint8_t *p, size_t len, vdm_evt_calibration_t *out,
                                     uint8_t *bad);

bool vdm_enc_evt_scan_done(const vdm_evt_scan_done_t *in, uint8_t *out, size_t cap,
                           size_t *out_len);
vdm_status_t vdm_dec_evt_scan_done(const uint8_t *p, size_t len, vdm_evt_scan_done_t *out);

/* EVT_LOG: `text` need not be null-terminated, `text_len` <= VDM_LOG_TEXT_MAX */
bool vdm_enc_evt_log(uint8_t level, const char *text, size_t text_len, uint8_t *out, size_t cap,
                     size_t *out_len);
vdm_status_t vdm_dec_evt_log(const uint8_t *p, size_t len, vdm_evt_log_t *out);

/* ---------------------------------------------------------------------- */
/* Helpers                                                                 */
/* ---------------------------------------------------------------------- */

/* CRC-16 over the encoded system config followed by the encoded motor
 * params; reported as `config_crc` in HELLO and SET_* responses. */
uint16_t vdm_config_crc(const vdm_system_config_t *sys, const vdm_motor_params_t *motor);

#ifdef __cplusplus
}
#endif

#endif /* VDM_CODEC_H */
