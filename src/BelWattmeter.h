#ifndef BELWATTMETER_H
#define BELWATTMETER_H

#include <Arduino.h>

struct BelData
{
  int voltage = 0;
  int current = 0;
  int power = 0;
  int consumption = 0;
};

enum BelParseState { WAIT_FOR_START, IN_FRAME, ESCAPE_NEXT };

typedef void (*BelDataCallback)(BelData data);

class BelWattmeter
{
  private:
    Stream& serial;
    BelDataCallback callback;
    unsigned long interval;
    unsigned long lastEmit = 0;
    BelParseState state = WAIT_FOR_START;
    int dataLength = 0;
    int dataIndex = 0;
    unsigned int voltageTmp = 0;
    unsigned int currentTmp = 0;
    unsigned int consumptionTmp = 0;
    unsigned int powerTmp = 0;
    byte crc = 0;
    bool crcOk = false;
    int counter = 0;
    BelData data;
    uint8_t feCount = 0;
    void Reset();

    void ProcessByte(int dataIndex, uint8_t data);

  public:
    BelWattmeter(Stream& serial, BelDataCallback callback, unsigned long interval = 60000);
    void Loop();
};

#endif
