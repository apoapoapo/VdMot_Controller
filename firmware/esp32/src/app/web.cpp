/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "app/web.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Update.h>
#include <WebServer.h>
#include <esp_ota_ops.h>

#include "app/network.h"
#include "app/shared.h"
#include "app/web_page.h"
#include "core/stm32_flasher.h"
#include "vdm/codec.h"

#ifndef FW_BUILD
#define FW_BUILD 0
#endif

namespace app
{

namespace
{

WebServer server(80);
vdm::Model g_model; /* copy for the handlers, large: keep it off the stack */
bool g_upload_ok = false;
String g_upload_error;
size_t g_upload_size = 0;
File g_upload_file;
uint32_t g_reboot_at = 0;

const esp_partition_t *otherApp(esp_app_desc_t *desc);

bool authorized()
{
    const NetSettings &n = netSettings();
    if (n.web_user[0] == '\0') {
        return true;
    }
    if (server.authenticate(n.web_user, n.web_pwd)) {
        return true;
    }
    server.requestAuthentication(BASIC_AUTH, "VdMot");
    return false;
}

void reply(bool ok, const String &error = String())
{
    StaticJsonDocument<192> doc;
    doc["ok"] = ok;
    if (!ok) {
        doc["error"] = error;
    }
    String out;
    serializeJson(doc, out);
    server.send(ok ? 200 : 400, "application/json", out);
}

String romString(const uint8_t *rom)
{
    char b[24];
    snprintf(b, sizeof(b), "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X", rom[0], rom[1], rom[2],
             rom[3], rom[4], rom[5], rom[6], rom[7]);
    return b;
}

/* "all" -> every channel, otherwise a channel number 0..11 */
bool channelMask(uint16_t &mask)
{
    String ch = server.arg("ch");
    if (ch == "all") {
        mask = VDM_CHANNEL_MASK_ALL;
        return true;
    }
    long c = ch.toInt();
    if (ch.length() == 0 || c < 0 || c >= VDM_MAX_CHANNELS) {
        return false;
    }
    mask = static_cast<uint16_t>(1u << c);
    return true;
}

bool command(CommandType t, uint16_t value = 0, uint8_t ch = 0)
{
    Command c{};
    c.type = t;
    c.value = value;
    c.channel = ch;
    return sendCommand(c);
}

/* ---------------------------------------------------------------------- */
/* Handlers                                                                */
/* ---------------------------------------------------------------------- */

void handleIndex()
{
    if (!authorized()) {
        return;
    }
    server.send_P(200, "text/html", kIndexHtml);
}

void handleState()
{
    if (!authorized()) {
        return;
    }
    char err[64];
    readModel(g_model, err, sizeof(err));
    const vdm::Model &m = g_model;

    DynamicJsonDocument doc(24576);
    JsonObject esp = doc.createNestedObject("esp");
    char v[24];
    snprintf(v, sizeof(v), "%d.%d.%d+%lu", FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH,
             static_cast<unsigned long>(FW_BUILD));
    esp["version"] = v;
    esp["uptime_s"] = millis() / 1000;
    esp["heap"] = ESP.getFreeHeap();
    esp_app_desc_t prev;
    if (otherApp(&prev) != nullptr) {
        JsonObject o = esp.createNestedObject("previous");
        o["version"] = prev.version;
        o["date"] = prev.date;
        o["project"] = prev.project_name;
    }

    NetStatus ns = netStatus();
    JsonObject net = doc.createNestedObject("net");
    net["interface"] = ns.interface;
    net["ip"] = ns.ip;
    net["mac"] = ns.mac;
    net["rssi"] = ns.rssi;
    net["ap"] = ns.ap_active;
    net["ap_ssid"] = ns.ap_ssid;
    net["hostname"] = netSettings().hostname;

    JsonObject link = doc.createNestedObject("link");
    link["state"] = vdm::linkStateName(m.state);
    JsonObject st = link.createNestedObject("stats");
    st["requests"] = m.stats.requests;
    st["retries"] = m.stats.retries;
    st["timeouts"] = m.stats.timeouts;
    st["link_ups"] = m.stats.link_ups;
    st["stm_reboots"] = m.stats.stm_reboots;
    st["events"] = m.stats.events;
    st["events_lost"] = m.stats.evt_lost;
    st["hard_resets"] = m.stats.hard_resets;

    if (m.hello_valid) {
        JsonObject stm = doc.createNestedObject("stm");
        snprintf(v, sizeof(v), "%u.%u.%u+%lu", m.hello.fw_major, m.hello.fw_minor, m.hello.fw_patch,
                 static_cast<unsigned long>(m.hello.fw_build));
        stm["version"] = v;
        stm["proto"] = m.hello.proto_version;
        stm["mcu"] =
            m.hello.mcu == VDM_MCU_F411 ? "F411" : (m.hello.mcu == VDM_MCU_F401 ? "F401" : "?");
        stm["hw"] = m.hello.hw_rev == VDM_HW_C1 ? "C1" : "C2/C3";
        stm["reset_cause"] = m.hello.reset_cause;
        stm["boot_count"] = m.hello.boot_count;
    }

    JsonArray valves = doc.createNestedArray("valves");
    for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
        const vdm_valve_state_t &s = m.valves[ch];
        JsonObject o = valves.createNestedObject();
        o["ch"] = ch;
        o["mode"] = m.valves_valid ? s.mode : static_cast<uint8_t>(VDM_MODE_UNKNOWN);
        o["flags"] = s.flags;
        o["fault"] = s.fault;
        o["actual"] = m.valves_valid ? s.actual : VDM_POS_UNKNOWN;
        o["target"] = s.target;
        o["current"] = s.current_ma10;
        if (m.detail_valid[ch]) {
            const vdm_valve_detail_t &d = m.details[ch];
            o["open_counts"] = d.open_counts;
            o["close_counts"] = d.close_counts;
            o["moves"] = d.moves_total;
            o["calib_age_h"] = d.calib_age_h;
            o["last_fault"] = d.last_fault;
            o["mean_open"] = d.mean_current_open_ma10;
            o["mean_close"] = d.mean_current_close_ma10;
            o["peak"] = d.peak_current_ma10;
        }
    }

    JsonArray sensors = doc.createNestedArray("sensors");
    for (uint8_t i = 0; i < m.sensor_total && i < vdm::Model::kMaxSensors; i++) {
        const vdm_sensor_value_t &s = m.sensors[i];
        JsonObject o = sensors.createNestedObject();
        o["rom"] = romString(s.rom);
        o["kind"] = s.kind;
        o["status"] = s.status;
        o["age"] = s.age_s;
        o["value"] = s.value;
    }
    if (m.scan_valid) {
        JsonObject sc = doc.createNestedObject("scan");
        sc["total"] = m.scan.total;
        sc["bus_ok"] = m.scan.bus_ok;
    }

    if (m.sys_valid) {
        JsonObject o = doc.createNestedObject("sys");
        o["link_timeout_s"] = m.sys.link_timeout_s;
        o["failsafe_pos"] = m.sys.failsafe_pos;
        o["startup_pos"] = m.sys.startup_pos;
        o["sensor_interval_s"] = m.sys.sensor_interval_s;
        o["recal_after_moves"] = m.sys.recal_after_moves;
        o["recal_after_days"] = m.sys.recal_after_days;
    }
    if (m.motor_valid) {
        JsonObject o = doc.createNestedObject("motor");
        o["endstop_factor_open"] = m.motor.endstop_factor_open;
        o["endstop_factor_close"] = m.motor.endstop_factor_close;
        o["endstop_abs_ma"] = m.motor.endstop_abs_ma;
        o["inrush_blank_ms"] = m.motor.inrush_blank_ms;
        o["opencircuit_ma10"] = m.motor.opencircuit_ma10;
        o["opencircuit_ms"] = m.motor.opencircuit_ms;
        o["move_timeout_s"] = m.motor.move_timeout_s;
        o["min_stroke_counts"] = m.motor.min_stroke_counts;
        o["max_calib_retries"] = m.motor.max_calib_retries;
    }
    if (m.diag_valid) {
        JsonObject o = doc.createNestedObject("diag");
        o["stm_uptime_s"] = m.diag.uptime_s;
        o["stm_rx_ok"] = m.diag.rx_frames_ok;
        o["stm_rx_crc_err"] = m.diag.rx_crc_err;
        o["stm_rx_malformed"] = m.diag.rx_malformed;
        o["stm_rx_overflow"] = m.diag.rx_overflow;
        o["stm_rx_duplicate"] = m.diag.rx_duplicate;
        o["ow_crc_err"] = m.diag.ow_crc_err;
        o["ow_bus_err"] = m.diag.ow_bus_err;
        o["eeprom_writes"] = m.diag.eeprom_writes;
        o["eeprom_err"] = m.diag.eeprom_err;
        o["stm_loop_max_us"] = m.diag.loop_max_us;
    }

    UpdateStatus u = updateStatus();
    JsonObject up = doc.createNestedObject("update");
    up["active"] = u.active;
    up["percent"] = u.percent;
    up["phase"] = u.phase;
    up["result"] = u.result;

    JsonArray log = doc.createNestedArray("log");
    for (uint8_t i = 0; i < m.log_count; i++) {
        const vdm::LogEntry &e = m.log[(m.log_head + i) % vdm::Model::kLogSize];
        JsonObject o = log.createNestedObject();
        o["ms"] = e.ms;
        o["level"] = e.level;
        o["text"] = e.text;
    }
    doc["error"] = err;

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

void handleTarget()
{
    if (!authorized()) {
        return;
    }
    long ch = server.arg("ch").toInt();
    float pos = server.arg("pos").toFloat();
    if (!server.hasArg("ch") || !server.hasArg("pos") || ch < 0 || ch >= VDM_MAX_CHANNELS ||
        pos < 0.0f || pos > 100.0f) {
        reply(false, "ch 0..11 and pos 0..100 required");
        return;
    }
    reply(command(CommandType::SetTarget, static_cast<uint16_t>(pos * 10.0f + 0.5f),
                  static_cast<uint8_t>(ch)),
          "queue full");
}

void handleMaskCommand(CommandType t)
{
    if (!authorized()) {
        return;
    }
    uint16_t mask;
    if (!channelMask(mask)) {
        reply(false, "ch must be 0..11 or all");
        return;
    }
    reply(command(t, mask), "queue full");
}

void handleSimple(CommandType t)
{
    if (!authorized()) {
        return;
    }
    reply(command(t), "queue full");
}

bool argU16(const char *name, uint16_t &v)
{
    if (!server.hasArg(name)) {
        return false;
    }
    long x = server.arg(name).toInt();
    if (x < 0 || x > 65535) {
        return false;
    }
    v = static_cast<uint16_t>(x);
    return true;
}

bool argU8(const char *name, uint8_t &v)
{
    uint16_t x;
    if (!argU16(name, x) || x > 255) {
        return false;
    }
    v = static_cast<uint8_t>(x);
    return true;
}

void handleSystemConfig()
{
    if (!authorized()) {
        return;
    }
    Command c{};
    c.type = CommandType::SetSystemConfig;
    vdm_system_config_t &s = c.sys;
    uint8_t buf[VDM_SYSTEM_CONFIG_LEN];
    size_t n;
    if (!argU16("link_timeout_s", s.link_timeout_s) || !argU16("failsafe_pos", s.failsafe_pos) ||
        !argU16("startup_pos", s.startup_pos) ||
        !argU16("sensor_interval_s", s.sensor_interval_s) ||
        !argU16("recal_after_moves", s.recal_after_moves) ||
        !argU8("recal_after_days", s.recal_after_days) ||
        !vdm_enc_system_config(&s, buf, sizeof(buf), &n)) {
        reply(false, "missing or out-of-range value (see docs/protocol)");
        return;
    }
    reply(sendCommand(c), "queue full");
}

void handleMotorParams()
{
    if (!authorized()) {
        return;
    }
    Command c{};
    c.type = CommandType::SetMotorParams;
    vdm_motor_params_t &m = c.motor;
    uint8_t buf[VDM_MOTOR_PARAMS_LEN];
    size_t n;
    if (!argU8("endstop_factor_open", m.endstop_factor_open) ||
        !argU8("endstop_factor_close", m.endstop_factor_close) ||
        !argU16("endstop_abs_ma", m.endstop_abs_ma) ||
        !argU16("inrush_blank_ms", m.inrush_blank_ms) ||
        !argU16("opencircuit_ma10", m.opencircuit_ma10) ||
        !argU16("opencircuit_ms", m.opencircuit_ms) ||
        !argU16("move_timeout_s", m.move_timeout_s) ||
        !argU16("min_stroke_counts", m.min_stroke_counts) ||
        !argU8("max_calib_retries", m.max_calib_retries) ||
        !vdm_enc_motor_params(&m, buf, sizeof(buf), &n)) {
        reply(false, "missing or out-of-range value (see docs/protocol)");
        return;
    }
    reply(sendCommand(c), "queue full");
}

void handleChannels()
{
    if (!authorized()) {
        return;
    }
    uint16_t mask = 0;
    for (int i = 0; i < server.args(); i++) {
        if (server.argName(i) == "ch") {
            long ch = server.arg(i).toInt();
            if (ch >= 0 && ch < VDM_MAX_CHANNELS) {
                mask |= static_cast<uint16_t>(1u << ch);
            }
        }
    }
    reply(command(CommandType::SetChannels, mask), "queue full");
}

/* --- STM32 image upload ------------------------------------------------ */

void handleStmUploadData()
{
    HTTPUpload &up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        g_upload_ok = false;
        g_upload_size = 0;
        g_upload_error = "";
        if (!authorized()) {
            g_upload_error = "not authorized";
            return;
        }
        if (updateStatus().active) {
            g_upload_error = "an update is already running";
            return;
        }
        g_upload_file = LittleFS.open(kStm32ImagePath, "w");
        if (!g_upload_file) {
            g_upload_error = "cannot store the file";
        }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (g_upload_file && g_upload_file.write(up.buf, up.currentSize) != up.currentSize) {
            g_upload_error = "file system full";
            g_upload_file.close();
        }
        g_upload_size += up.currentSize;
    } else if (up.status == UPLOAD_FILE_END) {
        if (g_upload_file) {
            g_upload_file.close();
        }
        if (g_upload_error.length() == 0) {
            File f = LittleFS.open(kStm32ImagePath, "r");
            uint8_t head[8] = {};
            size_t n = f ? f.read(head, sizeof(head)) : 0;
            size_t size = f ? f.size() : 0;
            if (f) {
                f.close();
            }
            g_upload_ok = vdm::Stm32Flasher::plausibleImage(head, n, size);
            if (!g_upload_ok) {
                g_upload_error = "not an STM32 application image";
            }
        }
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        if (g_upload_file) {
            g_upload_file.close();
        }
        g_upload_error = "upload aborted";
    }
}

void handleStmUploadDone()
{
    if (!g_upload_ok) {
        reply(false, g_upload_error.length() ? g_upload_error : String("upload failed"));
        return;
    }
    UpdateStatus s;
    s.active = true;
    strlcpy(s.phase, "queued", sizeof(s.phase));
    setUpdateStatus(s);
    reply(command(CommandType::FlashStm32), "queue full");
}

/* --- ESP32 OTA ------------------------------------------------------ */

void handleEspUploadData()
{
    HTTPUpload &up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        g_upload_ok = false;
        g_upload_error = "";
        if (!authorized()) {
            g_upload_error = "not authorized";
            return;
        }
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            g_upload_error = Update.errorString();
        }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (g_upload_error.length() == 0 &&
            Update.write(up.buf, up.currentSize) != up.currentSize) {
            g_upload_error = Update.errorString();
        }
    } else if (up.status == UPLOAD_FILE_END) {
        if (g_upload_error.length() == 0) {
            g_upload_ok = Update.end(true);
            if (!g_upload_ok) {
                g_upload_error = Update.errorString();
            }
        } else {
            Update.abort();
        }
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        g_upload_error = "upload aborted";
    }
}

void handleEspUploadDone()
{
    reply(g_upload_ok, g_upload_error.length() ? g_upload_error : String("update failed"));
    if (g_upload_ok) {
        g_reboot_at = millis() + 1000;
    }
}

/* the other OTA slot: after an update it still holds the previous firmware */
const esp_partition_t *otherApp(esp_app_desc_t *desc)
{
    const esp_partition_t *p = esp_ota_get_next_update_partition(nullptr);
    if (p == nullptr || esp_ota_get_partition_description(p, desc) != ESP_OK) {
        return nullptr;
    }
    return p;
}

void handleRollback()
{
    if (!authorized()) {
        return;
    }
    esp_app_desc_t desc;
    const esp_partition_t *p = otherApp(&desc);
    if (p == nullptr) {
        reply(false, "no previous firmware found");
        return;
    }
    if (esp_ota_set_boot_partition(p) != ESP_OK) {
        reply(false, "cannot select the previous firmware");
        return;
    }
    reply(true);
    g_reboot_at = millis() + 500;
}

void handleReboot()
{
    if (!authorized()) {
        return;
    }
    reply(true);
    g_reboot_at = millis() + 500;
}

} // namespace

void webBegin()
{
    server.on("/", HTTP_GET, handleIndex);
    server.on("/api/state", HTTP_GET, handleState);
    server.on("/api/target", HTTP_POST, handleTarget);
    server.on("/api/calibrate", HTTP_POST, [] { handleMaskCommand(CommandType::Calibrate); });
    server.on("/api/detect", HTTP_POST, [] { handleMaskCommand(CommandType::Detect); });
    server.on("/api/stop", HTTP_POST, [] { handleMaskCommand(CommandType::Stop); });
    server.on("/api/scan", HTTP_POST, [] { handleSimple(CommandType::SensorScan); });
    server.on("/api/stm32/reset", HTTP_POST, [] { handleSimple(CommandType::ResetStm32); });
    server.on("/api/stm32/hardreset", HTTP_POST, [] { handleSimple(CommandType::HardResetStm32); });
    server.on("/api/config/system", HTTP_POST, handleSystemConfig);
    server.on("/api/config/motor", HTTP_POST, handleMotorParams);
    server.on("/api/config/channels", HTTP_POST, handleChannels);
    server.on("/api/stm32/firmware", HTTP_POST, handleStmUploadDone, handleStmUploadData);
    server.on("/api/esp/firmware", HTTP_POST, handleEspUploadDone, handleEspUploadData);
    server.on("/api/reboot", HTTP_POST, handleReboot);
    server.on("/api/esp/rollback", HTTP_POST, handleRollback);
    server.onNotFound([] { server.send(404, "text/plain", "not found"); });
    server.begin();
}

void webLoop()
{
    server.handleClient();
    if (g_reboot_at != 0 && static_cast<int32_t>(millis() - g_reboot_at) >= 0) {
        ESP.restart();
    }
}

} // namespace app
