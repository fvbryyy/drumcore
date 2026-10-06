#ifndef ENCODER_H
#define ENCODER_H

#include <Arduino.h>

void encoderInit();

void encoderUpdate();

int8_t encoderGetDelta();

// Short press: a click released before the long-press threshold.
// Returns (and clears) a single press event.
bool encoderPressed();

// Long press: tombol ditahan melewati ENCODER_LONG_PRESS_MS. Diemit tepat
// sekali per hold; long press tidak pernah menghasilkan short press juga.
bool encoderLongPressed();

// Bersihkan atomik delta rotasi, short-press, dan long-press yang pending.
// Dipakai saat transisi state yang deterministik agar event sisa dari state
// lama tak pernah diproses state baru. Tidak menonaktifkan interupsi rotary.
//
// Bila dipanggil saat tombol masih ditahan fisik (mis. tepat setelah long
// press memicu pindah state), encoder masuk "CONSUMED": sisa hold itu
// diabaikan dan TIDAK ada short press saat tombol akhirnya dilepas. Ini
// mencegah release-click atau long-press kedua menyala di state yang baru.
void encoderFlush();

#endif