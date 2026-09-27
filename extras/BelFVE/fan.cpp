#include "fan.h"
#include "config.h"

#ifndef FAN_PULSES_PER_REV
#define FAN_PULSES_PER_REV 2
#endif
#ifndef FAN_PULSE_MIN_US
#define FAN_PULSE_MIN_US 800
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

uint32_t fanHeldPulses[FAN_COUNT];
uint32_t fanHeldMs[FAN_COUNT];
bool fanHeldRunning[FAN_COUNT];
bool fanPrevRunning[FAN_COUNT];
bool fanHeldValid = false;

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
  window->totalSlots = 0;
  window->mismatchSlots = 0;
  for(uint8_t i = 0; i < FAN_COUNT; i++)
  {
    window->avgPulses[i] = 0;
    window->avgMs[i] = 0;
    window->avgSlots[i] = 0;
    window->nonZeroSlots[i] = 0;
  }
}

void DropHeldSlot()
{
  fanHeldValid = false;
  for(uint8_t i = 0; i < FAN_COUNT; i++)
  {
    fanPrevRunning[i] = false;
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
    fanHeldPulses[i] = 0;
    fanHeldMs[i] = 0;
    fanHeldRunning[i] = false;
    fanPrevRunning[i] = false;
  }
  fanHeldValid = false;
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
  bool running[FAN_COUNT];
  for(uint8_t i = 0; i < FAN_COUNT; i++)
  {
    delta[i] = pulses[i] - fanPrevPulses[i];
    fanPrevPulses[i] = pulses[i];
    running[i] = delta[i] >= FAN_RUN_MIN_PULSES;
    fanRunFlag[i] = running[i];
  }

  if(elapsed > FAN_SLOT_MAX_MS)
  {
    Serial.printf("FAN: slot %lu ms zahozen\n", elapsed);
    DropHeldSlot();
    return;
  }

  if(fanHeldValid)
  {
    fanWindow.totalSlots++;
    for(uint8_t i = 0; i < FAN_COUNT; i++)
    {
      if(fanHeldRunning[i])
      {
        fanWindow.nonZeroSlots[i]++;
        if(fanPrevRunning[i] && running[i])
        {
          fanWindow.avgPulses[i] += fanHeldPulses[i];
          fanWindow.avgMs[i] += fanHeldMs[i];
          fanWindow.avgSlots[i]++;
        }
      }
      fanPrevRunning[i] = fanHeldRunning[i];
    }
    if(fanHeldRunning[FAN_A] != fanHeldRunning[FAN_B] && fanWindow.mismatchSlots < 65535)
    {
      fanWindow.mismatchSlots++;
    }
  }

  for(uint8_t i = 0; i < FAN_COUNT; i++)
  {
    fanHeldPulses[i] = delta[i];
    fanHeldMs[i] = (uint32_t)elapsed;
    fanHeldRunning[i] = running[i];
  }
  fanHeldValid = true;
}

void fanTake(FanWindow* window)
{
  *window = fanWindow;
  ResetFanWindow(&fanWindow);
}

uint16_t fanRpm(const FanWindow* window, uint8_t fan)
{
  if(window->avgMs[fan] == 0)
  {
    return 0;
  }
  uint64_t rpm = (uint64_t)window->avgPulses[fan] * 60000ULL
    / ((uint64_t)FAN_PULSES_PER_REV * window->avgMs[fan]);
  return rpm > 65535ULL ? (uint16_t)65535 : (uint16_t)rpm;
}

uint8_t fanRunPct(const FanWindow* window, uint8_t fan)
{
  if(window->totalSlots == 0)
  {
    return 0;
  }
  uint32_t pct = (uint32_t)window->nonZeroSlots[fan] * 100UL / window->totalSlots;
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
  Serial.printf("FAN %s: slotu=%u mismatch=%u",
    label,
    window->totalSlots,
    window->mismatchSlots);
  for(uint8_t i = 0; i < FAN_COUNT; i++)
  {
    Serial.printf("  %c: %u ot/min bezel=%u%% (%u nenulovych, prumer z %u, %lu imp)",
      i == FAN_A ? 'A' : 'B',
      fanRpm(window, i),
      fanRunPct(window, i),
      window->nonZeroSlots[i],
      window->avgSlots[i],
      (unsigned long)window->avgPulses[i]);
  }
  Serial.println();
}
