#include "fan.h"
#include "config.h"
#include "driver/pulse_cnt.h"

#ifndef FAN_PULSES_PER_REV
#define FAN_PULSES_PER_REV 4
#endif
#ifndef FAN_PCNT_FILTER_NS
#define FAN_PCNT_FILTER_NS 10000
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

pcnt_unit_handle_t fanPcntUnit[FAN_COUNT] = { NULL, NULL };

uint32_t fanPcntTotal[FAN_COUNT];
uint32_t fanPrevPulses[FAN_COUNT];
uint32_t fanWindowPulses[FAN_COUNT];
bool fanRunFlag[FAN_COUNT];

uint32_t fanHeldPulses[FAN_COUNT];
uint32_t fanHeldMs[FAN_COUNT];
bool fanHeldRunning[FAN_COUNT];
bool fanPrevRunning[FAN_COUNT];
bool fanHeldValid = false;

unsigned long fanLastSlot = 0;
unsigned long fanWindowStartMs = 0;
FanWindow fanWindow;

void SetupPcnt(uint8_t fan, int pin)
{
  pcnt_unit_config_t unitConfig = {};
  unitConfig.low_limit = -32768;
  unitConfig.high_limit = 32767;
  if(pcnt_new_unit(&unitConfig, &fanPcntUnit[fan]) != ESP_OK)
  {
    fanPcntUnit[fan] = NULL;
    Serial.print(F("FAN: PCNT jednotka selhala na kanalu "));
    Serial.println(fan);
    return;
  }

  pcnt_chan_config_t chanConfig = {};
  chanConfig.edge_gpio_num = pin;
  chanConfig.level_gpio_num = -1;
  pcnt_channel_handle_t channel = NULL;
  if(pcnt_new_channel(fanPcntUnit[fan], &chanConfig, &channel) != ESP_OK)
  {
    fanPcntUnit[fan] = NULL;
    Serial.print(F("FAN: PCNT kanal selhal na kanalu "));
    Serial.println(fan);
    return;
  }

  pcnt_channel_set_edge_action(channel,
    PCNT_CHANNEL_EDGE_ACTION_INCREASE,
    PCNT_CHANNEL_EDGE_ACTION_INCREASE);
  pcnt_channel_set_level_action(channel,
    PCNT_CHANNEL_LEVEL_ACTION_KEEP,
    PCNT_CHANNEL_LEVEL_ACTION_KEEP);

  if(FAN_PCNT_FILTER_NS > 0)
  {
    pcnt_glitch_filter_config_t filterConfig = {};
    filterConfig.max_glitch_ns = FAN_PCNT_FILTER_NS;
    if(pcnt_unit_set_glitch_filter(fanPcntUnit[fan], &filterConfig) != ESP_OK)
    {
      Serial.print(F("FAN: PCNT filtr selhal na kanalu "));
      Serial.println(fan);
    }
  }

  pcnt_unit_enable(fanPcntUnit[fan]);
  pcnt_unit_clear_count(fanPcntUnit[fan]);
  pcnt_unit_start(fanPcntUnit[fan]);
}

void DrainPcnt()
{
  for(uint8_t i = 0; i < FAN_COUNT; i++)
  {
    if(fanPcntUnit[i] == NULL)
    {
      continue;
    }
    int value = 0;
    if(pcnt_unit_get_count(fanPcntUnit[i], &value) == ESP_OK && value > 0)
    {
      pcnt_unit_clear_count(fanPcntUnit[i]);
      fanPcntTotal[i] += (uint32_t)value;
    }
  }
}

void ResetFanWindow(FanWindow* window)
{
  window->totalSlots = 0;
  window->mismatchSlots = 0;
  window->windowMs = 0;
  for(uint8_t i = 0; i < FAN_COUNT; i++)
  {
    window->avgPulses[i] = 0;
    window->avgMs[i] = 0;
    window->avgSlots[i] = 0;
    window->nonZeroSlots[i] = 0;
    window->windowPulses[i] = 0;
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
    fanPcntTotal[i] = 0;
    fanPrevPulses[i] = 0;
    fanWindowPulses[i] = 0;
    fanRunFlag[i] = false;
    fanHeldPulses[i] = 0;
    fanHeldMs[i] = 0;
    fanHeldRunning[i] = false;
    fanPrevRunning[i] = false;
  }
  fanHeldValid = false;
  SetupPcnt(FAN_A, FAN_TACH_PIN_A);
  SetupPcnt(FAN_B, FAN_TACH_PIN_B);
  pinMode(FAN_TACH_PIN_A, INPUT_PULLUP);
  pinMode(FAN_TACH_PIN_B, INPUT_PULLUP);
  fanLastSlot = millis();
  fanWindowStartMs = fanLastSlot;
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
  DrainPcnt();

  uint32_t delta[FAN_COUNT];
  bool running[FAN_COUNT];
  for(uint8_t i = 0; i < FAN_COUNT; i++)
  {
    delta[i] = fanPcntTotal[i] - fanPrevPulses[i];
    fanPrevPulses[i] = fanPcntTotal[i];
    fanWindowPulses[i] += delta[i];
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
  unsigned long now = millis();
  for(uint8_t i = 0; i < FAN_COUNT; i++)
  {
    fanWindow.windowPulses[i] = fanWindowPulses[i];
    fanWindowPulses[i] = 0;
  }
  fanWindow.windowMs = (uint32_t)(now - fanWindowStartMs);
  fanWindowStartMs = now;

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

void fanScopeText(const FanWindow* window, uint8_t fan, char* out, size_t len)
{
  uint32_t rate = window->windowMs == 0
    ? 0
    : (uint32_t)((uint64_t)window->windowPulses[fan] * 1000ULL / window->windowMs);
  snprintf(out, len,
    "%c pcnt=%lu/%lums rate=%lu/s slotu=%u prumer=%u ot/min=%u bezel=%u%%",
    fan == FAN_A ? 'A' : 'B',
    (unsigned long)window->windowPulses[fan],
    (unsigned long)window->windowMs,
    (unsigned long)rate,
    window->totalSlots,
    window->avgSlots[fan],
    fanRpm(window, fan),
    fanRunPct(window, fan));
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
  char line[180];
  for(uint8_t i = 0; i < FAN_COUNT; i++)
  {
    fanScopeText(window, i, line, sizeof(line));
    Serial.print(F("FAN scope: "));
    Serial.println(line);
  }
}
