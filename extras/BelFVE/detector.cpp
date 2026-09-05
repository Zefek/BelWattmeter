#include "detector.h"
#include "config.h"

#define DET_BATCH_INTERVAL_MS 50
#define DET_BATCH_SAMPLES 8
#define DET_SLOT_MS 1000
#define DET_SLOTS 256
#define DET_SCALE_MAX 1023
#define DET_ADC_FULL_SCALE_MV 3300
#define DET_ADC_RAW_MAX 4095
#define DET_TASK_CORE 1
#define DET_TASK_PRIORITY 2
#define DET_TASK_STACK 3072

#ifndef DET_STATE_OFF
#define DET_STATE_OFF 40
#endif
#ifndef DET_DROPOUT_MIN_RUN
#define DET_DROPOUT_MIN_RUN 20
#endif

struct DetectorSlot
{
  uint16_t samples;
  uint32_t sum[DET_CHANNELS];
  uint16_t hits[DET_CHANNELS];
  uint16_t ripple[DET_CHANNELS];
  uint16_t minValue[DET_CHANNELS];
  uint16_t maxValue[DET_CHANNELS];
  uint16_t rawAdc[DET_CHANNELS];
  uint8_t dropouts[DET_CHANNELS];
};

const uint8_t detPins[DET_CHANNELS] = { DET_PIN_A, DET_PIN_B };

DetectorSlot detRing[DET_SLOTS];
volatile uint32_t detWriteIndex = 0;
uint32_t detReadIndex = 0;

DetectorSlot detAccumulator;
bool detBatchLow[DET_CHANNELS];
uint16_t detBatchRipple[DET_CHANNELS];
uint8_t detConductRun[DET_CHANNELS];
uint8_t detRunBeforeIdle[DET_CHANNELS];
uint8_t detIdleBatches[DET_CHANNELS];

DetectorWindow detFastWindow;
DetectorWindow detSlowWindow;

void ResetWindow(DetectorWindow* window)
{
  window->samples = 0;
  window->lostSlots = 0;
  for(uint8_t i = 0; i < DET_CHANNELS; i++)
  {
    window->sum[i] = 0;
    window->hits[i] = 0;
    window->ripple[i] = 0;
    window->minValue[i] = DET_SCALE_MAX;
    window->maxValue[i] = 0;
    window->rawAdcMin[i] = DET_ADC_RAW_MAX;
    window->rawAdcMax[i] = 0;
    window->dropouts[i] = 0;
  }
}

void ResetAccumulator()
{
  detAccumulator.samples = 0;
  for(uint8_t i = 0; i < DET_CHANNELS; i++)
  {
    detAccumulator.sum[i] = 0;
    detAccumulator.hits[i] = 0;
    detAccumulator.ripple[i] = 0;
    detAccumulator.minValue[i] = DET_SCALE_MAX;
    detAccumulator.maxValue[i] = 0;
    detAccumulator.rawAdc[i] = 0;
    detAccumulator.dropouts[i] = 0;
  }
}

uint16_t ReadScaled(uint8_t pin)
{
  uint32_t mv = (uint32_t)analogReadMilliVolts(pin);
  uint32_t value = mv * DET_SCALE_MAX / DET_ADC_FULL_SCALE_MV;
  return value > DET_SCALE_MAX ? (uint16_t)DET_SCALE_MAX : (uint16_t)value;
}

void SampleChannel(uint8_t channel)
{
  uint16_t value = ReadScaled(detPins[channel]);

  detAccumulator.sum[channel] += value;
  if(value < detAccumulator.minValue[channel])
  {
    detAccumulator.minValue[channel] = value;
  }
  if(value > detAccumulator.maxValue[channel])
  {
    detAccumulator.maxValue[channel] = value;
  }
  if(value < DET_TH_STATE)
  {
    detAccumulator.hits[channel]++;
    detBatchLow[channel] = true;
  }
  if(value < DET_TH_CONDUCT && value > detBatchRipple[channel])
  {
    detBatchRipple[channel] = value;
  }
}

void CloseBatch()
{
  for(uint8_t i = 0; i < DET_CHANNELS; i++)
  {
    if(detBatchLow[i])
    {
      if(detBatchRipple[i] > detAccumulator.ripple[i])
      {
        detAccumulator.ripple[i] = detBatchRipple[i];
      }
      if(detConductRun[i] < 255)
      {
        detConductRun[i]++;
      }
      detIdleBatches[i] = 0;
      continue;
    }
    if(detConductRun[i] > 0)
    {
      detRunBeforeIdle[i] = detConductRun[i];
      detConductRun[i] = 0;
    }
    if(detIdleBatches[i] < DET_DROPOUT_BATCHES)
    {
      detIdleBatches[i]++;
      if(detIdleBatches[i] == DET_DROPOUT_BATCHES
        && detRunBeforeIdle[i] >= DET_DROPOUT_MIN_RUN
        && detAccumulator.dropouts[i] < 255)
      {
        detAccumulator.dropouts[i]++;
      }
    }
  }
}

void PublishSlot()
{
  for(uint8_t i = 0; i < DET_CHANNELS; i++)
  {
    detAccumulator.rawAdc[i] = (uint16_t)analogRead(detPins[i]);
  }
  uint32_t index = detWriteIndex;
  detRing[index % DET_SLOTS] = detAccumulator;
  __sync_synchronize();
  detWriteIndex = index + 1;
  ResetAccumulator();
}

void DetectorTask(void* arg)
{
  TickType_t last = xTaskGetTickCount();
  uint32_t tick = 0;
  for(;;)
  {
    uint32_t phase = tick % DET_BATCH_INTERVAL_MS;
    if(phase < DET_BATCH_SAMPLES)
    {
      if(phase == 0)
      {
        for(uint8_t i = 0; i < DET_CHANNELS; i++)
        {
          detBatchLow[i] = false;
          detBatchRipple[i] = 0;
        }
      }
      SampleChannel(DET_CHANNEL_A);
      SampleChannel(DET_CHANNEL_B);
      detAccumulator.samples++;
      if(phase == DET_BATCH_SAMPLES - 1)
      {
        CloseBatch();
      }
    }
    tick++;
    if(tick % DET_SLOT_MS == 0)
    {
      PublishSlot();
    }
    vTaskDelayUntil(&last, pdMS_TO_TICKS(1));
  }
}

void AddSlot(DetectorWindow* window, const DetectorSlot* slot)
{
  window->samples += slot->samples;
  for(uint8_t i = 0; i < DET_CHANNELS; i++)
  {
    window->sum[i] += slot->sum[i];
    window->hits[i] += slot->hits[i];
    if(slot->ripple[i] > window->ripple[i])
    {
      window->ripple[i] = slot->ripple[i];
    }
    if(slot->samples > 0 && slot->minValue[i] < window->minValue[i])
    {
      window->minValue[i] = slot->minValue[i];
    }
    if(slot->maxValue[i] > window->maxValue[i])
    {
      window->maxValue[i] = slot->maxValue[i];
    }
    if(slot->rawAdc[i] < window->rawAdcMin[i])
    {
      window->rawAdcMin[i] = slot->rawAdc[i];
    }
    if(slot->rawAdc[i] > window->rawAdcMax[i])
    {
      window->rawAdcMax[i] = slot->rawAdc[i];
    }
    window->dropouts[i] += slot->dropouts[i];
  }
}

void detectorInit()
{
  ResetAccumulator();
  ResetWindow(&detFastWindow);
  ResetWindow(&detSlowWindow);
  for(uint8_t i = 0; i < DET_CHANNELS; i++)
  {
    detBatchLow[i] = false;
    detBatchRipple[i] = 0;
    detConductRun[i] = 0;
    detRunBeforeIdle[i] = 0;
    detIdleBatches[i] = DET_DROPOUT_BATCHES;
  }
  pinMode(DET_PIN_A, INPUT);
  pinMode(DET_PIN_B, INPUT);
  xTaskCreatePinnedToCore(DetectorTask, "detector", DET_TASK_STACK, NULL, DET_TASK_PRIORITY, NULL, DET_TASK_CORE);
}

void detectorDrain()
{
  uint32_t write = detWriteIndex;
  __sync_synchronize();
  uint32_t pending = write - detReadIndex;
  if(pending > DET_SLOTS)
  {
    uint16_t lost = (pending - DET_SLOTS) > 65535UL ? 65535 : (uint16_t)(pending - DET_SLOTS);
    detFastWindow.lostSlots += lost;
    detSlowWindow.lostSlots += lost;
    detReadIndex = write - DET_SLOTS;
    Serial.printf("DETECTOR: ztraceno %u slotu\n", lost);
  }
  while(detReadIndex != write)
  {
    const DetectorSlot* slot = &detRing[detReadIndex % DET_SLOTS];
    AddSlot(&detFastWindow, slot);
    AddSlot(&detSlowWindow, slot);
    detReadIndex++;
  }
}

void detectorTakeFast(DetectorWindow* window)
{
  *window = detFastWindow;
  ResetWindow(&detFastWindow);
}

void detectorTakeSlow(DetectorWindow* window)
{
  *window = detSlowWindow;
  ResetWindow(&detSlowWindow);
}

uint16_t detectorAverage(const DetectorWindow* window, uint8_t channel)
{
  if(window->samples == 0)
  {
    return 0;
  }
  return (uint16_t)(window->sum[channel] / window->samples);
}

uint8_t detectorDuty(const DetectorWindow* window, uint8_t channel)
{
  if(window->samples == 0)
  {
    return 0;
  }
  return (uint8_t)(window->hits[channel] * 100UL / window->samples);
}

bool ChannelActive(const DetectorWindow* window, uint8_t channel, bool previous)
{
  uint8_t duty = detectorDuty(window, channel);
  return previous ? duty >= DET_STATE_OFF : duty >= DET_STATE_MAJORITY;
}

char detectorState(const DetectorWindow* window, char previous)
{
  uint8_t bits = (previous >= '0' && previous <= '3') ? (uint8_t)(previous - '0') : 0;
  bool a = ChannelActive(window, DET_CHANNEL_A, (bits & 1) != 0);
  bool b = ChannelActive(window, DET_CHANNEL_B, (bits & 2) != 0);
  return (char)('0' + (a ? 1 : 0) + (b ? 2 : 0));
}

void detectorLog(const DetectorWindow* window, const char* label, char state)
{
  Serial.printf("DETECTOR %s: n=%lu stav=%c", label, (unsigned long)window->samples, state);
  for(uint8_t i = 0; i < DET_CHANNELS; i++)
  {
    uint16_t avg = detectorAverage(window, i);
    Serial.printf("  %c: avg=%u (%u mV) min=%u max=%u duty=%u%% ripple=%u drop=%u adc=%u..%u%s",
      i == DET_CHANNEL_A ? 'A' : 'B',
      avg,
      (unsigned)((uint32_t)avg * DET_ADC_FULL_SCALE_MV / DET_SCALE_MAX),
      window->samples > 0 ? window->minValue[i] : 0,
      window->maxValue[i],
      detectorDuty(window, i),
      window->ripple[i],
      window->dropouts[i],
      window->rawAdcMin[i],
      window->rawAdcMax[i],
      window->rawAdcMax[i] >= DET_ADC_RAW_MAX ? " SATURACE" : "");
  }
  Serial.println();
}
