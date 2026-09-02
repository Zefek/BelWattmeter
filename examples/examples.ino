#include <BelWattmeter.h>

#if defined(HAVE_HWSERIAL1)
  #define BEL_SERIAL Serial1
#else
  #include <SoftwareSerial.h>
  SoftwareSerial belSerial(10, 11);
  #define BEL_SERIAL belSerial
#endif

void OnBelData(BelData data);

BelWattmeter wattmeter(BEL_SERIAL, OnBelData);

void OnBelData(BelData data)
{
  Serial.print("U=");
  Serial.print(data.voltage);
  Serial.print("V  I=");
  Serial.print(data.current);
  Serial.print("  P=");
  Serial.print(data.power);
  Serial.print("W  E=");
  Serial.println(data.consumption);
}

void setup()
{
  Serial.begin(115200);
  BEL_SERIAL.begin(9600);
}

void loop()
{
  wattmeter.Loop();
}
