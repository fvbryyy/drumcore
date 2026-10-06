#include "encoder.h"
#include "config.h"

// Quadrature decoding delegated to the well-tested mathertel RotaryEncoder
// library, using the exact configuration that was verified on this hardware:
//   pins (D6, D5) Dibalik — arah kanan/kiri normal lagi;
//   LatchMode::TWO03      — encoders whose detent sits at states 0 AND 3.
//
// The reference sketch polls tick() every loop. Here it is even safer: tick()
// is called from an interrupt on every pin CHANGE, so a fast spin can never
// fall into a 20 ms UI poll gap or get eaten by LCD I2C writes.
#include <RotaryEncoder.h>

#include <esp8266_peri.h>

// NOTE: pin order is intentionally (ENCODER_PIN_B, ENCODER_PIN_A) so the
// library sees (D6, D5) exactly like the verified reference sketch.
static RotaryEncoder rotary(
    ENCODER_PIN_B,   // D6 -> sig1
    ENCODER_PIN_A,   // D5 -> sig2
    RotaryEncoder::LatchMode::TWO03);

static volatile bool pressed = false;
static volatile bool longPressed = false;

// Last position handed out by encoderGetDelta().
static int32_t lastReportPos = 0;

// Cap steps emitted in one poll window so an int8_t overflow or an electrical
// burst cannot cause erratic menu jumps.
static constexpr int8_t DELTA_CLAMP = 8;

// ------------------------------------------------------------
// Button state machine (unchanged from the previous driver).
//
// IDLE --> PRESSED --> LONG_PRESSED --> (release) --> IDLE
//                  \--> (release < threshold) emits short press --> IDLE
//
// Once a long press is emitted, the state becomes LONG_PRESSED and the
// pending short press is cancelled forever. encoderFlush() may move the
// machine to CONSUMED while the button is still held, so the remaining part
// of that hold can never fire anything in the newly entered state.
// ------------------------------------------------------------

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

    // ---- Debounce the raw level into stableButton. ----
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

    // ---- State transitions on the debounced level. ----
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
                // Released before the long-press threshold: a short press.
                pressed = true;
                btnState = BtnState::IDLE;
            }
            else if (now - downStartMs >= ENCODER_LONG_PRESS_MS)
            {
                // Held long enough: emit the long press exactly once and
                // cancel the pending short press forever for this hold.
                longPressed = true;
                btnState = BtnState::LONG_PRESSED;
            }
            break;

        case BtnState::LONG_PRESSED:
            // Still holding: nothing more. Release simply ends the hold.
            if (stableButton == HIGH)
            {
                btnState = BtnState::IDLE;
            }
            break;

        case BtnState::CONSUMED:
            // The transition already used this press. The rest of the hold is
            // inert, and releasing it must not produce any press event.
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

    // Align the machine with the physical button. If it is still pressed
    // (typical right after a long press caused a transition), mark the rest
    // of this hold as CONSUMED so no release-click or second long-press can
    // fire inside the new state. If it is released, go back to IDLE.
    uint8_t raw = digitalRead(ENCODER_BUTTON);
    stableButton = raw;
    btnState = (raw == LOW) ? BtnState::CONSUMED : BtnState::IDLE;
}