#pragma once

#include <stdint.h>

// Filtering ringan — FR-HW-010. High-pass Butterworth orde 2 (biquad) untuk
// membuang DC offset & rumble frekuensi rendah (gerakan tubuh/gesekan vest),
// plus low-pass opsional. Pengganti DC blocker 1-pole versi lama: roll-off
// -12 dB/oktaf (bukan -6) sehingga rumble di bawah cutoff teredam jauh lebih
// kuat tanpa menggeser cutoff ke dalam pita suara napas.
//
// State filter disimpan per channel_index (0-based, < NUM_AUDIO_CHANNELS).
// Filtering berat (adaptive, denoising) sengaja TIDAK di firmware — itu domain
// backend (ARCHITECTURE_HARDWARE.md §3).

// Menghitung koefisien & mereset state. cutoff <= 0 menonaktifkan filter tsb.
void noise_filter_init(float sample_rate_hz, float hpf_cutoff_hz, float lpf_cutoff_hz);

// Reset state semua channel (koefisien tetap).
void noise_filter_reset();

// Memfilter satu sample (domain 24-bit, sebagai float). Panggil berurutan per
// sample untuk tiap channel — jangan mencampur aliran antar channel.
float noise_filter_apply(uint8_t channel_index, float sample);
