/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "app/shared.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace app
{

namespace
{

QueueHandle_t g_commands = nullptr;
SemaphoreHandle_t g_lock = nullptr;
vdm::Model g_model;
bool g_model_valid = false;
char g_error[64] = "";
UpdateStatus g_update;
vdm::LogEntry g_log[kEspLogSize];
uint8_t g_log_head = 0;
uint8_t g_log_count = 0;

struct Lock {
    Lock()
    {
        xSemaphoreTake(g_lock, portMAX_DELAY);
    }
    ~Lock()
    {
        xSemaphoreGive(g_lock);
    }
};

} // namespace

void sharedInit()
{
    g_commands = xQueueCreate(16, sizeof(Command));
    g_lock = xSemaphoreCreateMutex();
}

bool sendCommand(const Command &c)
{
    return xQueueSend(g_commands, &c, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool receiveCommand(Command &c)
{
    return xQueueReceive(g_commands, &c, 0) == pdTRUE;
}

void publishModel(const vdm::Model &m, const char *last_error)
{
    Lock l;
    g_model = m;
    g_model_valid = true;
    strlcpy(g_error, last_error, sizeof(g_error));
}

bool readModel(vdm::Model &out, char *last_error, size_t err_len)
{
    Lock l;
    out = g_model;
    strlcpy(last_error, g_error, err_len);
    return g_model_valid;
}

void setUpdateStatus(const UpdateStatus &s)
{
    Lock l;
    g_update = s;
}

UpdateStatus updateStatus()
{
    Lock l;
    return g_update;
}

void espLog(uint8_t level, const char *fmt, ...)
{
    vdm::LogEntry e;
    e.ms = millis();
    e.level = level;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.text, sizeof(e.text), fmt, ap);
    va_end(ap);
    Serial.printf("[%lu] %s\n", static_cast<unsigned long>(e.ms), e.text);
    Lock l;
    uint8_t idx = static_cast<uint8_t>((g_log_head + g_log_count) % kEspLogSize);
    if (g_log_count == kEspLogSize) {
        g_log_head = static_cast<uint8_t>((g_log_head + 1u) % kEspLogSize);
    } else {
        g_log_count++;
    }
    g_log[idx] = e;
}

uint8_t espLogCopy(vdm::LogEntry *out, uint8_t max)
{
    Lock l;
    uint8_t n = g_log_count < max ? g_log_count : max;
    uint8_t first = static_cast<uint8_t>(g_log_count - n);
    for (uint8_t i = 0; i < n; i++) {
        out[i] = g_log[(g_log_head + first + i) % kEspLogSize];
    }
    return n;
}

} // namespace app
