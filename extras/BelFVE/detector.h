#pragma once

#include <Arduino.h>

#define DET_CHANNELS 2
#define DET_CHANNEL_A 0
#define DET_CHANNEL_B 1

struct DetectorWindow
{
  uint32_t samples;
  uint32_t sum[DET_CHANNELS];
  uint32_t hits[DET_CHANNELS];
  uint16_t ripple[DET_CHANNELS];
  uint16_t minValue[DET_CHANNELS];
  uint16_t maxValue[DET_CHANNELS];
  uint16_t rawAdcMin[DET_CHANNELS];
  uint16_t rawAdcMax[DET_CHANNELS];
  uint16_t dropouts[DET_CHANNELS];
  uint16_t lostSlots;
};

void detectorInit();
void detectorDrain();
void detectorTakeFast(DetectorWindow* window);
void detectorTakeSlow(DetectorWindow* window);
uint16_t detectorAverage(const DetectorWindow* window, uint8_t channel);
uint8_t detectorDuty(const DetectorWindow* window, uint8_t channel);
char detectorState(const DetectorWindow* window, char previous);
void detectorLog(const DetectorWindow* window, const char* label, char state);
