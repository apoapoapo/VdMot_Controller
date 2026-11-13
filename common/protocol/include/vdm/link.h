/*
 * VdMot Controller - ESP32 <-> STM32 link protocol, version 1
 *
 * Constants and data types shared by both firmwares.
 * Specification: docs/protocol/stm32-esp32-link.md
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef VDM_LINK_H
#define VDM_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------- */
/* Framing                                                                 */
/* ---------------------------------------------------------------------- */

#define VDM_PROTO_VERSION 1u

#define VDM_HEADER_LEN 4u /* ver, kind, seq, msg */
#define VDM_CRC_LEN 2u
#define VDM_PAYLOAD_MAX 240u
#define VDM_FRAME_MIN (VDM_HEADER_LEN + VDM_CRC_LEN)                   /* 6   */
#define VDM_FRAME_MAX (VDM_HEADER_LEN + VDM_PAYLOAD_MAX + VDM_CRC_LEN) /* 246 */
/* COBS adds one byte per started block of 254 bytes, plus the 0x00 delimiter. */
#define VDM_ENCODED_MAX (VDM_FRAME_MAX + (VDM_FRAME_MAX / 254u) + 1u) /* 247 */
#define VDM_WIRE_MAX (VDM_ENCODED_MAX + 1u)                           /* 248 */

typedef enum {
    VDM_KIND_REQUEST = 0x01,
    VDM_KIND_RESPONSE = 0x02,
    VDM_KIND_EVENT = 0x03,
} vdm_kind_t;

/* ---------------------------------------------------------------------- */
/* Message ids                                                             */
/* ---------------------------------------------------------------------- */

typedef enum {
    /* system */
    VDM_MSG_HELLO = 0x01,
    VDM_MSG_PING = 0x02,
    VDM_MSG_RESET = 0x03,
    VDM_MSG_SET_SYSTEM_CONFIG = 0x04,
    VDM_MSG_GET_SYSTEM_CONFIG = 0x05,
    /* valves */
    VDM_MSG_GET_VALVE_STATES = 0x10,
    VDM_MSG_SET_TARGETS = 0x11,
    VDM_MSG_CALIBRATE = 0x12,
    VDM_MSG_STOP = 0x13,
    VDM_MSG_SET_CHANNELS = 0x14,
    VDM_MSG_DETECT = 0x15,
    VDM_MSG_GET_VALVE_DETAIL = 0x16,
    VDM_MSG_SET_MOTOR_PARAMS = 0x20,
    VDM_MSG_GET_MOTOR_PARAMS = 0x21,
    /* sensors */
    VDM_MSG_SENSOR_SCAN = 0x30,
    VDM_MSG_GET_SENSORS = 0x31,
    /* diagnostics */
    VDM_MSG_GET_DIAG = 0x40,
    VDM_MSG_CLEAR_DIAG = 0x41,
    /* events */
    VDM_EVT_BOOT = 0x80,
    VDM_EVT_VALVE = 0x81,
    VDM_EVT_CALIBRATION = 0x82,
    VDM_EVT_SENSORS = 0x83,
    VDM_EVT_SENSOR_SCAN_DONE = 0x84,
    VDM_EVT_LINK_TIMEOUT = 0x85,
    VDM_EVT_LOG = 0x86,
} vdm_msg_t;

/* ---------------------------------------------------------------------- */
/* Response status codes (first payload byte of every response)            */
/* ---------------------------------------------------------------------- */

typedef enum {
    VDM_OK = 0x00,
    VDM_ERR_UNKNOWN_MSG = 0x01,
    VDM_ERR_BAD_LEN = 0x02,
    VDM_ERR_RANGE = 0x03,
    VDM_ERR_CHANNEL = 0x04,
    VDM_ERR_BUSY = 0x05,
    VDM_ERR_STATE = 0x06,
    VDM_ERR_STORAGE = 0x07,
    VDM_ERR_NOT_READY = 0x08,
    VDM_ERR_INTERNAL = 0xFF,
} vdm_status_t;

/* ---------------------------------------------------------------------- */
/* Limits and special values                                               */
/* ---------------------------------------------------------------------- */

#define VDM_MAX_CHANNELS 12u
#define VDM_CHANNEL_MASK_ALL 0x0FFFu
#define VDM_POS_MAX 1000u       /* 0.1 % units: 1000 = fully open */
#define VDM_POS_UNKNOWN 0xFFFFu /* actual position unknown       */
#define VDM_POS_HOLD 0xFFFFu    /* failsafe/startup: keep position */
#define VDM_PING_MAX 16u
#define VDM_RESET_MAGIC 0x52535431u /* "RST1" */
#define VDM_SENSORS_PER_MSG 12u
#define VDM_LOG_TEXT_MAX 200u

/* ---------------------------------------------------------------------- */
/* Enumerations                                                            */
/* ---------------------------------------------------------------------- */

typedef enum {
    VDM_HW_C1 = 1,
    VDM_HW_C2 = 2, /* C2 and C3 share the schematic */
} vdm_hw_rev_t;

typedef enum {
    VDM_MCU_F401 = 1,
    VDM_MCU_F411 = 2,
} vdm_mcu_t;

#define VDM_CAP_DS18B20 (1u << 0)
#define VDM_CAP_DS2438 (1u << 1)
#define VDM_CAP_RS485 (1u << 2)
#define VDM_CAP_CAN (1u << 3)

typedef enum {
    VDM_RESET_UNKNOWN = 0,
    VDM_RESET_POWER_ON = 1,
    VDM_RESET_PIN = 2,
    VDM_RESET_SOFTWARE = 3,
    VDM_RESET_IWDG = 4,
    VDM_RESET_BROWN_OUT = 5,
    VDM_RESET_LOW_POWER = 6,
} vdm_reset_cause_t;

typedef enum {
    VDM_MODE_DISABLED = 0,
    VDM_MODE_UNKNOWN = 1,
    VDM_MODE_NOT_CONNECTED = 2,
    VDM_MODE_IDLE = 3,
    VDM_MODE_OPENING = 4,
    VDM_MODE_CLOSING = 5,
    VDM_MODE_CALIBRATING = 6,
    VDM_MODE_QUEUED = 7,
    VDM_MODE_FAULT = 8,
} vdm_valve_mode_t;
#define VDM_MODE_LAST VDM_MODE_FAULT

#define VDM_VFLAG_CALIBRATED (1u << 0)
#define VDM_VFLAG_CALIB_PENDING (1u << 1)
#define VDM_VFLAG_POS_UNCERTAIN (1u << 2)
#define VDM_VFLAG_AT_ENDSTOP (1u << 3)
#define VDM_VFLAG_FAILSAFE (1u << 4)
#define VDM_VFLAG_MASK 0x1Fu

typedef enum {
    VDM_FAULT_NONE = 0,
    VDM_FAULT_OPEN_CIRCUIT = 1,
    VDM_FAULT_BLOCKED = 2,
    VDM_FAULT_TIMEOUT = 3,
    VDM_FAULT_OVERCURRENT = 4,
    VDM_FAULT_CALIB_FAILED = 5,
    VDM_FAULT_NO_PULSES = 6,
    VDM_FAULT_COUNT_MISMATCH = 7,
} vdm_fault_t;
#define VDM_FAULT_LAST VDM_FAULT_COUNT_MISMATCH

typedef enum {
    VDM_SENSOR_TEMP_DS18B20 = 1,
    VDM_SENSOR_VAD_DS2438 = 2,
    VDM_SENSOR_VDD_DS2438 = 3,
    VDM_SENSOR_TEMP_DS2438 = 4,
} vdm_sensor_kind_t;

typedef enum {
    VDM_SSTAT_OK = 0,
    VDM_SSTAT_CRC_ERROR = 1,
    VDM_SSTAT_UNREACHABLE = 2,
    VDM_SSTAT_NOT_MEASURED = 3,
    VDM_SSTAT_OUT_OF_RANGE = 4,
} vdm_sensor_status_t;

typedef enum {
    VDM_LOG_ERROR = 0,
    VDM_LOG_WARN = 1,
    VDM_LOG_INFO = 2,
    VDM_LOG_DEBUG = 3,
} vdm_log_level_t;

/* ---------------------------------------------------------------------- */
/* Message bodies                                                          */
/* All bodies are serialized field by field, little-endian (see codec.h).  */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint8_t esp_proto_version;
    uint8_t flags; /* bit 0: reset_seq */
} vdm_hello_req_t;
#define VDM_HELLO_FLAG_RESET_SEQ (1u << 0)

/* HELLO response body, also the payload of EVT_BOOT */
typedef struct {
    uint8_t proto_version;
    uint8_t fw_major, fw_minor, fw_patch;
    uint32_t fw_build;
    uint8_t hw_rev;
    uint8_t mcu;
    uint8_t mcu_uid[12];
    uint8_t channel_count;
    uint32_t capabilities;
    uint8_t reset_cause;
    uint32_t boot_count;
    uint16_t config_crc;
} vdm_hello_t;

typedef struct {
    uint16_t link_timeout_s;
    uint16_t failsafe_pos;
    uint16_t startup_pos;
    uint16_t sensor_interval_s;
    uint16_t recal_after_moves;
    uint8_t recal_after_days;
} vdm_system_config_t;

typedef struct {
    uint8_t endstop_factor_open;
    uint8_t endstop_factor_close;
    uint16_t endstop_abs_ma;
    uint16_t inrush_blank_ms;
    uint16_t opencircuit_ma10;
    uint16_t opencircuit_ms;
    uint16_t move_timeout_s;
    uint16_t min_stroke_counts;
    uint8_t max_calib_retries;
} vdm_motor_params_t;

typedef struct {
    uint8_t channel;
    uint8_t mode;  /* vdm_valve_mode_t */
    uint8_t flags; /* VDM_VFLAG_* */
    uint8_t fault; /* vdm_fault_t */
    uint16_t actual;
    uint16_t target;
    uint16_t current_ma10;
} vdm_valve_state_t;

typedef struct {
    uint8_t count;
    vdm_valve_state_t valves[VDM_MAX_CHANNELS];
} vdm_valve_states_t;

typedef struct {
    uint8_t channel;
    uint16_t target;
} vdm_target_t;

typedef struct {
    uint8_t count;
    vdm_target_t targets[VDM_MAX_CHANNELS];
} vdm_set_targets_t;

typedef struct {
    vdm_valve_state_t state; /* state.channel is the detail's channel */
    uint32_t open_counts;
    uint32_t close_counts;
    int32_t deadzone_counts;
    uint32_t calib_age_h;
    uint32_t moves_since_calib;
    uint32_t moves_total;
    uint16_t mean_current_open_ma10;
    uint16_t mean_current_close_ma10;
    uint16_t peak_current_ma10;
    int32_t last_move_counts;
    uint32_t last_move_ms;
    uint8_t calib_retries;
    uint8_t last_fault;
} vdm_valve_detail_t;

typedef struct {
    uint8_t rom[8];
    uint8_t kind;   /* vdm_sensor_kind_t */
    uint8_t status; /* vdm_sensor_status_t */
    uint16_t age_s;
    int32_t value; /* m°C or mV */
} vdm_sensor_value_t;

/* GET_SENSORS response body, also the payload of EVT_SENSORS */
typedef struct {
    uint8_t total;
    uint8_t offset;
    uint8_t count;
    vdm_sensor_value_t sensors[VDM_SENSORS_PER_MSG];
} vdm_sensors_t;

typedef struct {
    uint32_t uptime_s;
    uint32_t rx_frames_ok;
    uint16_t rx_crc_err;
    uint16_t rx_malformed;
    uint16_t rx_overflow;
    uint16_t rx_bad_version;
    uint16_t rx_duplicate;
    uint16_t uart_hw_err;
    uint16_t ow_crc_err;
    uint16_t ow_bus_err;
    uint32_t eeprom_writes;
    uint16_t eeprom_err;
    uint32_t loop_max_us;
    int16_t mcu_temp_c10;
} vdm_diag_t;

typedef struct {
    uint8_t channel;
    uint8_t result; /* 0 = OK, otherwise vdm_fault_t */
    uint32_t open_counts;
    uint32_t close_counts;
    uint8_t retries;
} vdm_evt_calibration_t;

typedef struct {
    uint8_t total;
    uint8_t ds18_count;
    uint8_t ds2438_count;
    uint8_t bus_ok;
} vdm_evt_scan_done_t;

typedef struct {
    uint8_t level; /* vdm_log_level_t */
    uint8_t len;
    char text[VDM_LOG_TEXT_MAX + 1]; /* always null-terminated after decode */
} vdm_evt_log_t;

/* Default values (used when no valid stored configuration exists). */
extern const vdm_system_config_t vdm_system_config_default;
extern const vdm_motor_params_t vdm_motor_params_default;

#ifdef __cplusplus
}
#endif

#endif /* VDM_LINK_H */
