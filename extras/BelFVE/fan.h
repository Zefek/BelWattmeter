#pragma once

#include <Arduino.h>

#define FAN_COUNT 2
#define FAN_A 0
#define FAN_B 1

struct FanWindow
{
  uint32_t avgPulses[FAN_COUNT];
  uint32_t avgMs[FAN_COUNT];
  uint16_t avgSlots[FAN_COUNT];
  uint16_t nonZeroSlots[FAN_COUNT];
  uint16_t totalSlots;
  uint16_t mismatchSlots;
  uint32_t windowPulses[FAN_COUNT];
  uint32_t windowMs;
};

void fanInit();
void fanLoop();
void fanTake(FanWindow* window);
uint16_t fanRpm(const FanWindow* window, uint8_t fan);
uint8_t fanRunPct(const FanWindow* window, uint8_t fan);
bool fanRunning(uint8_t fan);
char fanStateChar();
void fanLog(const FanWindow* window, const char* label);
void fanScopeText(const FanWindow* window, uint8_t fan, char* out, size_t len);
