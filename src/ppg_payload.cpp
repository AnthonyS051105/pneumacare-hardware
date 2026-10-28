#include "ppg_payload.h"

#include <ArduinoJson.h>

static size_t serialize_checked(const JsonDocument &doc, char *out, size_t out_cap) {
  if (doc.overflowed()) return 0;
  size_t needed = measureJson(doc);
  if (needed + 1 > out_cap) return 0;
  return serializeJson(doc, out, out_cap);
}

size_t ppg_payload_build(char *out, size_t out_cap, const char *device_id, uint64_t timestamp_ms,
                         uint32_t sample_rate_hz, const uint32_t *samples_ir,
                         const uint32_t *samples_red, size_t n_samples) {
  JsonDocument doc;
  doc["device_id"] = device_id;
  doc["timestamp_ms"] = timestamp_ms;
  doc["sample_rate_hz"] = sample_rate_hz;
  // "samples" tetap berarti infrared (backward-compat, §3.3 revisi 17 Agt 2026).
  JsonArray ir = doc["samples"].to<JsonArray>();
  JsonArray red = doc["samples_red"].to<JsonArray>();
  for (size_t i = 0; i < n_samples; i++) {
    ir.add(samples_ir[i]);
    red.add(samples_red[i]);
  }
  return serialize_checked(doc, out, out_cap);
}

size_t status_payload_build(char *out, size_t out_cap, const char *device_id, const char *status,
                            uint64_t timestamp_ms, bool include_battery, int battery_pct) {
  JsonDocument doc;
  doc["device_id"] = device_id;
  doc["status"] = status;
  doc["timestamp_ms"] = timestamp_ms;
  if (include_battery) {
    doc["battery_pct"] = battery_pct;
  }
  return serialize_checked(doc, out, out_cap);
}
