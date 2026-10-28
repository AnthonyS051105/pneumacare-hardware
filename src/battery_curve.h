#pragma once

#include <stdint.h>

// Estimasi persen baterai Li-Po 1-cell dari tegangan (mV) — FR-HW-030.
// Kurva tegangan-tanpa-beban tipikal Li-Po 3.7 V, diinterpolasi linear.
// ⚠️ Perkiraan kasar (tegangan turun saat beban WiFi tinggi); kalibrasi dengan
// baterai aktual saat uji daya tahan (Fase 6).
int battery_percent_from_mv(uint32_t battery_mv);
