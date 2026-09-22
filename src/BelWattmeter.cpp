#include "BelWattmeter.h"

const int16_t voltageLimit = 430;
const int16_t currentLimit = 2000;
const int16_t powerLimit = 8600;

bool InRange(int16_t value, int16_t limit)
{
  return value > -limit && value < limit;
}

int AverageRounded(int32_t sum, int count)
{
  int32_t half = (int32_t)count / 2;
  return (int)((sum >= 0 ? sum + half : sum - half) / (int32_t)count);
}

BelWattmeter::BelWattmeter(Stream& serial, BelDataCallback callback, unsigned long interval)
  : serial(serial), callback(callback), interval(interval)
{
}

void BelWattmeter::ProcessByte(int dataIndex, uint8_t data)
{
  if(dataIndex == 1)  this->dataLength = data;
  if(dataIndex == 4)  this->voltageTmp = data;
  if(dataIndex == 5)  this->voltageTmp |= data << 8;
  if(dataIndex == 6)  this->currentTmp = data;
  if(dataIndex == 7)  this->currentTmp |= data << 8;
  if(dataIndex == 8)  this->consumptionTmp = data;
  if(dataIndex == 9)  this->consumptionTmp |= data << 8;
  if(dataIndex == 21) this->powerTmp = data;
  if(dataIndex == 22) this->powerTmp |= data << 8;
  if(dataIndex == 28)
  {
    crcOk = crc == data;
    return;
  }
  crc += data;
}

void BelWattmeter::Loop()
{
  while(serial.available())
  {
    int raw = serial.read();
    if(raw < 0)
    {
      continue;
    }
    byte read = (byte)raw;
    switch(state)
    {
      case WAIT_FOR_START:
        if(read == 0xFC)
        {
          state = IN_FRAME;
          crc = 0;
          feCount = 0;
          dataIndex = 0;
          dataLength = 0;
          this->voltageTmp = 0;
          this->currentTmp = 0;
          this->consumptionTmp = 0;
          this->powerTmp = 0;
          crcOk = false;
        }
      break;
      case IN_FRAME:
        if(read == 0xFD)
        {
          state = ESCAPE_NEXT;
        }
        else if (read == 0xFE)
        {
          feCount++;
          if(feCount == 2)
          {
            int16_t voltage = (int16_t)voltageTmp;
            int16_t current = (int16_t)currentTmp;
            int16_t power = (int16_t)powerTmp;
            if(crcOk && InRange(voltage, voltageLimit) && InRange(current, currentLimit) && InRange(power, powerLimit))
            {
              voltageSum += voltage;
              currentSum += current;
              powerSum += power;
              data.consumption = consumptionTmp;
              counter++;
            }
            else if(!crcOk && frameErrors < 65535)
            {
              frameErrors++;
            }
            state = WAIT_FOR_START;
          }
        }
        else
        {
          if(dataIndex > 1 && (dataIndex > this->dataLength + 1 || this->dataLength != 28))
          {
            state = WAIT_FOR_START;
            break;
          }
          ProcessByte(dataIndex, read);
          feCount = 0;
          dataIndex++;
        }
      break;
      case ESCAPE_NEXT:
        ProcessByte(dataIndex, read);
        state = IN_FRAME;
        feCount = 0;
        dataIndex++;
      break;
    }
  }

  if(millis() - lastEmit >= interval)
  {
    lastEmit = millis();
    if(callback != nullptr && counter > 0)
    {
      data.voltage = AverageRounded(voltageSum, counter);
      data.current = AverageRounded(currentSum, counter);
      data.power = AverageRounded(powerSum, counter);
      callback(data);
    }
    Reset();
  }
}

uint16_t BelWattmeter::GetFrameErrors() const
{
  return frameErrors;
}

void BelWattmeter::Reset()
{
  data.voltage = 0;
  data.current = 0;
  data.power = 0;
  data.consumption = 0;
  voltageSum = 0;
  currentSum = 0;
  powerSum = 0;
  counter = 0;
}
