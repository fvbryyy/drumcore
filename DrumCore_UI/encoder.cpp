#include "encoder.h"
#include "config.h"

// Decoding quadrature didelegasikan ke library mathertel RotaryEncoder yang
// teruji, dengan konfigurasi persis yang terverifikasi di hardware ini:
//   pin (D6, D5) dibalik — arah kanan/kiri normal lagi;
//   LatchMode::TWO03     — encoder yang detent-nya di state 0 DAN 3.
// Sketsa referensi mem-poll tick() tiap loop; di sini lebih aman: tick()
// dipanggil dari interupsi setiap CHANGE pin, jadi putaran cepat tak pernah
// jatuh ke celah poll UI 20 ms atau termakan tulis I2C LCD.
#include <RotaryEncoder.h>

#include <esp8266_peri.h>

// NOTE: urutan pin sengaja (ENCODER_PIN_B, ENCODER_PIN_A) agar library
// melihat (D6, D5) persis seperti sketsa referensi yang terverifikasi.
static RotaryEncoder rotary(
    ENCODER_PIN_B,   // D6 -> sig1
    ENCODER_PIN_A,   // D5 -> sig2
    RotaryEncoder::LatchMode::TWO03);

static volatile bool pressed = false;
static volatile bool longPressed = false;

static int32_t lastReportPos = 0;

// Batasi step per jendela poll agar overflow int8_t/burst elektrik tak
// menyebabkan lompatan menu liar.
static constexpr int8_t DELTA_CLAMP = 8;

// State machine tombol (tak berubah dari driver sebelumnya).
//   IDLE --> PRESSED --> LONG_PRESSED --> (release) --> IDLE
//                    \--> (release < threshold) emit short press --> IDLE
// Setelah long press diemit, state menjadi LONG_PRESSED dan short press
// pending dibatalkan selamanya. encoderFlush() bisa memindahkan mesin ke
// CONSUMED saat tombol masih ditahan, agar sisa hold tak bisa menyalakan
// apa pun di state yang baru dimasuki.

enum class BtnState : uint8_t
{
    IDLE,          // button physically released
    PRESSED,       // button held, waiting for release or the long threshold
    LONG_PRESSED,  // long press already emitted, still holding
    CONSUMED       // transition flushed the press; ignore until release
};

static BtnState btnState = BtnState::IDLE;

// Debounced button level (stableButton) + debounce window tracking.
static uint8_t stableButton = HIGH;
static bool settlePending = false;
static uint32_t settleStartMs = 0;

static uint32_t downStartMs = 0;

static constexpr uint16_t DEBOUNCE_MS = 50;

// Sample BOTH channels in one atomic GPIO register snapshot on every CHANGE.
// Two separate digitalRead() calls can sample A and B microseconds apart, and
// a pin that flips between the reads would fabricate a bogus intermediate
// state that could eat the corner of a real click.
static void IRAM_ATTR onEncoderInterrupt()
{
    uint32_t reg = GPIO_REG_READ(GPIO_IN_ADDRESS);

    int sig1 = (reg & digitalPinToBitMask(ENCODER_PIN_B)) ? 1 : 0;
    int sig2 = (reg & digitalPinToBitMask(ENCODER_PIN_A)) ? 1 : 0;

    rotary.tick(sig1, sig2);
}

void encoderInit()
{
    pinMode(ENCODER_PIN_A, INPUT_PULLUP);
    pinMode(ENCODER_PIN_B, INPUT_PULLUP);
    pinMode(ENCODER_BUTTON, INPUT_PULLUP);

    rotary.setPosition(0);
    lastReportPos = 0;

    stableButton = digitalRead(ENCODER_BUTTON);
    settlePending = false;
    btnState = BtnState::IDLE;
    pressed = false;
    longPressed = false;

    attachInterrupt(
        digitalPinToInterrupt(ENCODER_PIN_A),
        onEncoderInterrupt,
        CHANGE
    );

    attachInterrupt(
        digitalPinToInterrupt(ENCODER_PIN_B),
        onEncoderInterrupt,
        CHANGE
    );
}

void encoderUpdate()
{
    uint32_t now = millis();

    uint8_t raw = digitalRead(ENCODER_BUTTON);

    if (raw != stableButton)
    {
        if (!settlePending)
        {
            settlePending = true;
            settleStartMs = now;
        }
        else if (now - settleStartMs >= DEBOUNCE_MS)
        {
            stableButton = raw;
            settlePending = false;
        }
    }
    else
    {
        settlePending = false;
    }

    switch (btnState)
    {
        case BtnState::IDLE:
            if (stableButton == LOW)
            {
                btnState = BtnState::PRESSED;
                downStartMs = now;
            }
            break;

        case BtnState::PRESSED:
            if (stableButton == HIGH)
            {
                pressed = true;
                btnState = BtnState::IDLE;
            }
            else if (now - downStartMs >= ENCODER_LONG_PRESS_MS)
            {
                // Tahan cukup lama: emit long press tepat sekali; short press
                // pending dibatalkan selamanya untuk hold ini.
                longPressed = true;
                btnState = BtnState::LONG_PRESSED;
            }
            break;

        case BtnState::LONG_PRESSED:
            if (stableButton == HIGH)
            {
                btnState = BtnState::IDLE;
            }
            break;

        case BtnState::CONSUMED:
            // Transisi sudah memakai press ini. Sisa hold inert; melepasnya
            // tidak boleh menghasilkan event press apa pun.
            if (stableButton == HIGH)
            {
                btnState = BtnState::IDLE;
            }
            break;
    }
}

int8_t encoderGetDelta()
{
    noInterrupts();
    long pos = rotary.getPosition();
    interrupts();

    int32_t diff = pos - lastReportPos;
    lastReportPos = pos;

    if (diff == 0)
    {
        return 0;
    }

    if (diff > DELTA_CLAMP)
    {
        diff = DELTA_CLAMP;
    }
    else if (diff < -DELTA_CLAMP)
    {
        diff = -DELTA_CLAMP;
    }

    return (int8_t)diff;
}

bool encoderPressed()
{
    noInterrupts();
    bool p = pressed;
    pressed = false;
    interrupts();
    return p;
}

bool encoderLongPressed()
{
    noInterrupts();
    bool lp = longPressed;
    longPressed = false;
    interrupts();
    return lp;
}

void encoderFlush()
{
    noInterrupts();
    rotary.setPosition(0);
    lastReportPos = 0;
    pressed = false;
    longPressed = false;
    settlePending = false;
    interrupts();

    // Selaraskan mesin dengan tombol fisik. Bila masih ditekan (khas tepat
    // setelah long press memicu transisi), tandai sisa hold ini sebagai
    // CONSUMED agar tak ada release-click/long-press kedua yang menyala di
    // state baru. Bila sudah dilepas, kembali ke IDLE.
    uint8_t raw = digitalRead(ENCODER_BUTTON);
    stableButton = raw;
    btnState = (raw == LOW) ? BtnState::CONSUMED : BtnState::IDLE;
}