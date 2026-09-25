#include "fan.h"
#include "config.h"

#ifndef FAN_PULSES_PER_REV
#define FAN_PULSES_PER_REV 2
#endif
#ifndef FAN_PULSE_MIN_US
#define FAN_PULSE_MIN_US 300
#endif
#ifndef FAN_SLOT_MS
#define FAN_SLOT_MS 1000
#endif
#ifndef FAN_SLOT_MAX_MS
#define FAN_SLOT_MAX_MS 5000
#endif
#ifndef FAN_RUN_MIN_PULSES
#define FAN_RUN_MIN_PULSES 10
#endif

portMUX_TYPE fanMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t fanPulses[FAN_COUNT];
volatile uint32_t fanLastPulseUs[FAN_COUNT];

uint32_t fanPrevPulses[FAN_COUNT];
bool fanRunFlag[FAN_COUNT];
unsigned long fanLastSlot = 0;
FanWindow fanWindow;

void IRAM_ATTR OnFanPulseA()
{
  uint32_t now = micros();
  portENTER_CRITICAL_ISR(&fanMux);
  if(now - fanLastPulseUs[FAN_A] >= FAN_PULSE_MIN_US)
  {
    fanLastPulseUs[FAN_A] = now;
    fanPulses[FAN_A] = fanPulses[FAN_A] + 1;
  }
  portEXIT_CRITICAL_ISR(&fanMux);
}

void IRAM_ATTR OnFanPulseB()
{
  uint32_t now = micros();
  portENTER_CRITICAL_ISR(&fanMux);
  if(now - fanLastPulseUs[FAN_B] >= FAN_PULSE_MIN_US)
  {
    fanLastPulseUs[FAN_B] = now;
    fanPulses[FAN_B] = fanPulses[FAN_B] + 1;
  }
  portEXIT_CRITICAL_ISR(&fanMux);
}

void ResetFanWindow(FanWindow* window)
{
  window->windowMs = 0;
  window->mismatchSlots = 0;
  for(uint8_t i = 0; i < FAN_COUNT; i++)
  {
    window->runMs[i] = 0;
    window->runPulses[i] = 0;
  }
}

void fanInit()
{
  ResetFanWindow(&fanWindow);
  for(uint8_t i = 0; i < FAN_COUNT; i++)
  {
    fanPulses[i] = 0;
    fanLastPulseUs[i] = 0;
    fanPrevPulses[i] = 0;
    fanRunFlag[i] = false;
  }
  pinMode(FAN_TACH_PIN_A, INPUT_PULLUP);
  pinMode(FAN_TACH_PIN_B, INPUT_PULLUP);
  fanLastSlot = millis();
  attachInterrupt(digitalPinToInterrupt(FAN_TACH_PIN_A), OnFanPulseA, FALLING);
  attachInterrupt(digitalPinToInterrupt(FAN_TACH_PIN_B), OnFanPulseB, FALLING);
}

void fanLoop()
{
  unsigned long now = millis();
  unsigned long elapsed = now - fanLastSlot;
  if(elapsed < FAN_SLOT_MS)
  {
    return;
  }
  fanLastSlot = now;

  uint32_t pulses[FAN_COUNT];
  portENTER_CRITICAL(&fanMux);
  for(uint8_t i = 0; i < FAN_COUNT; i++)
  {
    pulses[i] = fanPulses[i];
  }
  portEXIT_CRITICAL(&fanMux);

  uint32_t delta[FAN_COUNT];
  for(uint8_t i = 0; i < FAN_COUNT; i++)
  {
    delta[i] = pulses[i] - fanPrevPulses[i];
    fanPrevPulses[i] = pulses[i];
    fanRunFlag[i] = delta[i] >= FAN_RUN_MIN_PULSES;
  }

  if(elapsed > FAN_SLOT_MAX_MS)
  {
    Serial.printf("FAN: slot %lu ms zahozen\n", elapsed);
    return;
  }

  fanWindow.windowMs += (uint32_t)elapsed;
  for(uint8_t i = 0; i < FAN_COUNT; i++)
  {
    if(fanRunFlag[i])
    {
      fanWindow.runMs[i] += (uint32_t)elapsed;
      fanWindow.runPulses[i] += delta[i];
    }
  }
  if(fanRunFlag[FAN_A] != fanRunFlag[FAN_B] && fanWindow.mismatchSlots < 65535)
  {
    fanWindow.mismatchSlots++;
  }
}

void fanTake(FanWindow* window)
{
  *window = fanWindow;
  ResetFanWindow(&fanWindow);
}

uint16_t fanRpm(const FanWindow* window, uint8_t fan)
{
  if(window->runMs[fan] == 0)
  {
    return 0;
  }
  uint64_t rpm = (uint64_t)window->runPulses[fan] * 60000ULL
    / ((uint64_t)FAN_PULSES_PER_REV * window->runMs[fan]);
  return rpm > 65535ULL ? (uint16_t)65535 : (uint16_t)rpm;
}

uint8_t fanRunPct(const FanWindow* window, uint8_t fan)
{
  if(window->windowMs == 0)
  {
    return 0;
  }
  uint32_t pct = window->runMs[fan] * 100UL / window->windowMs;
  return pct > 100 ? (uint8_t)100 : (uint8_t)pct;
}

bool fanRunning(uint8_t fan)
{
  return fanRunFlag[fan];
}

char fanStateChar()
{
  return (char)('0' + (fanRunFlag[FAN_A] ? 1 : 0) + (fanRunFlag[FAN_B] ? 2 : 0));
}

void fanLog(const FanWindow* window, const char* label)
{
  Serial.printf("FAN %s: okno=%lus mismatch=%u",
    label,
    (unsigned long)(window->windowMs / 1000UL),
    window->mismatchSlots);
  for(uint8_t i = 0; i < FAN_COUNT; i++)
  {
    Serial.printf("  %c: %u ot/min bezel=%u%% (%lus, %lu imp)",
      i == FAN_A ? 'A' : 'B',
      fanRpm(window, i),
      fanRunPct(window, i),
      (unsigned long)(window->runMs[i] / 1000UL),
      (unsigned long)window->runPulses[i]);
  }
  Serial.println();
}
