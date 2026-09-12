#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <BelWattmeter.h>
#include "time.h"
#include "esp_sntp.h"
#include "esp_task_wdt.h"
#include "config.h"
#include "secret.h"
#include "ota.h"
#include "detector.h"

#ifndef FW_VERSION
#define FW_VERSION 0
#endif
#define HA_INTERVAL_MS 60000UL
#define DIAG_INTERVAL_MS 300000UL
#define RECONNECT_INTERVAL_MS 5000UL
#define WDT_TIMEOUT_S 90
#define WIFI_CONNECT_TIMEOUT_MS 15000UL
#define TIME_SYNC_TIMEOUT_MS 15000UL
#define TLS_HANDSHAKE_TIMEOUT_S 30
#define MQTT_TLS_PORT 8883
#define MQTT_BUFFER_SIZE 256
#define MQTT_KEEP_ALIVE 60
#define TIME_VALID_THRESHOLD 1700000000UL
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"

void OnBelData(BelData data);

WiFiClientSecure net;
PubSubClient mqtt(net);
BelWattmeter bel(Serial2, OnBelData, HA_INTERVAL_MS);

#pragma pack(push, 1)
struct FveData
{
  uint8_t voltage[4];
  uint8_t current[4];
  uint8_t power[4];
  uint8_t consumption[4];
  uint8_t heaterState;
  uint8_t dutyA[2];
  uint8_t dutyB[2];
  uint8_t energyA[8];
  uint8_t energyB[8];
};

struct DiagData
{
  uint32_t uptimeMinutes;
  uint16_t freeHeapKb;
  uint16_t minFreeHeapKb;
  uint16_t wifiReconnects;
  uint16_t mqttFailCount;
  uint16_t otaFailCount;
  uint16_t loopMaxMs;
  uint16_t belFrameErrors;
  uint16_t loadDropouts;
  uint16_t rawA;
  uint16_t rawB;
  uint16_t rippleA;
  uint16_t rippleB;
  uint8_t resetReason;
  uint16_t fwVersion;
  int8_t rssi;
};
#pragma pack(pop)

static_assert(sizeof(FveData) == 37, "FveData wire layout must stay 37 bytes");
static_assert(sizeof(DiagData) == 32, "DiagData wire layout must stay 32 bytes");

FveData currentFveData;
DiagData currentDiagData;
BelData pendingBelData;
bool belDataPending = false;
char heaterState = '0';

uint64_t energyMilliWhA = 0;
uint64_t energyMilliWhB = 0;
uint16_t lastConsumption = 0;
bool consumptionValid = false;

enum BelAvailability { BEL_UNKNOWN, BEL_ONLINE, BEL_OFFLINE };
enum FaultState { FAULT_UNKNOWN, FAULT_OK, FAULT_SUSPECT };

uint16_t otaFailures = 0;
unsigned long lastDiagSend = 0;
unsigned long lastConnectTry = 0;
unsigned long lastBelData = 0;
unsigned long lastHaSend = 0;
uint16_t belFrameErrorBase = 0;
BelAvailability belAvailability = BEL_UNKNOWN;
BelAvailability sentAvailability = BEL_UNKNOWN;
FaultState faultState = FAULT_UNKNOWN;
FaultState sentFault = FAULT_UNKNOWN;
bool timeSynced = false;
char ntpFromDhcp[16] = "";

void convertToHalfByte(uint32_t value, uint8_t* result, uint8_t length)
{
  for(uint8_t i = 0; i < length; i++)
  {
    uint8_t v = value & 0x0F;
    result[i] = v < 10 ? (uint8_t)('0' + v) : (uint8_t)('7' + v);
    value = value >> 4;
  }
}

uint16_t heapKb(uint32_t bytes)
{
  return (uint16_t)(bytes / 1024);
}

void OnBelData(BelData data)
{
#if BEL_ENABLED
  pendingBelData = data;
  belDataPending = true;
#else
  (void)data;
#endif
}

bool SyncTime()
{
  const ip_addr_t* dhcpServer = esp_sntp_getserver(0);
  if(dhcpServer != NULL && !ip_addr_isany_val(*dhcpServer))
  {
    snprintf(ntpFromDhcp, sizeof(ntpFromDhcp), "%s", ipaddr_ntoa(dhcpServer));
  }
  if(ntpFromDhcp[0] != '\0')
  {
    Serial.printf("NTP z DHCP: %s\n", ntpFromDhcp);
    configTime(0, 0, ntpFromDhcp, NTP_SERVER_1, NTP_SERVER_2);
  }
  else
  {
    configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);
  }
  unsigned long start = millis();
  time_t now = time(nullptr);
  while(now < TIME_VALID_THRESHOLD && millis() - start < TIME_SYNC_TIMEOUT_MS)
  {
    esp_task_wdt_reset();
    delay(200);
    now = time(nullptr);
  }
  return now >= TIME_VALID_THRESHOLD;
}

bool EnsureConnected()
{
  if(WiFi.status() == WL_CONNECTED && mqtt.connected())
  {
    return true;
  }
  if(millis() - lastConnectTry < RECONNECT_INTERVAL_MS)
  {
    return false;
  }
  lastConnectTry = millis();

  if(WiFi.status() != WL_CONNECTED)
  {
    sentAvailability = BEL_UNKNOWN;
    sentFault = FAULT_UNKNOWN;
    if(currentDiagData.wifiReconnects < 65535)
    {
      currentDiagData.wifiReconnects++;
    }
    WiFi.begin(WifiSSID, WifiPassword);
    unsigned long start = millis();
    while(WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS)
    {
      esp_task_wdt_reset();
      delay(100);
    }
    if(WiFi.status() != WL_CONNECTED)
    {
      Serial.println("WiFi: nepripojeno, zkusim znovu");
      return false;
    }
    Serial.printf("WiFi: pripojeno, IP %s\n", WiFi.localIP().toString().c_str());
  }

  if(!timeSynced)
  {
    timeSynced = SyncTime();
    if(!timeSynced)
    {
      Serial.println("NTP: cas se nepodarilo synchronizovat");
      return false;
    }
  }

  bool connected = mqtt.connect(MQTTClientId, MQTTUsername, MQTTPassword, TOPIC_FVE_STATE, 0, true, "Offline");
  if(!connected)
  {
    sentAvailability = BEL_UNKNOWN;
    sentFault = FAULT_UNKNOWN;
    if(currentDiagData.mqttFailCount < 65535)
    {
      currentDiagData.mqttFailCount++;
    }
    Serial.printf("MQTT: connect selhal, stav %d\n", mqtt.state());
    return false;
  }
  sentAvailability = BEL_UNKNOWN;
  sentFault = FAULT_UNKNOWN;
  Serial.println("MQTT: pripojeno");
  return true;
}

void PublishAvailability()
{
  if(belAvailability == BEL_UNKNOWN || belAvailability == sentAvailability)
  {
    return;
  }
  const char* state = belAvailability == BEL_ONLINE ? "Online" : "Offline";
  if(mqtt.publish(TOPIC_FVE_STATE, state, true))
  {
    sentAvailability = belAvailability;
    Serial.printf("FVE availability: %s\n", state);
  }
}

void PublishFault()
{
  if(faultState == FAULT_UNKNOWN || faultState == sentFault)
  {
    return;
  }
  const char* state = faultState == FAULT_SUSPECT ? "Fault" : "Ok";
  if(mqtt.publish(TOPIC_FVE_FAULT, state, true))
  {
    sentFault = faultState;
    Serial.printf("FVE fault: %s\n", state);
  }
}

void AccumulateEnergy(const BelData* data, const DetectorWindow* window)
{
  uint16_t consumption = (uint16_t)data->consumption;
  uint32_t deltaUnits = consumptionValid ? (uint32_t)(uint16_t)(consumption - lastConsumption) : 0;
  if(deltaUnits > BEL_DELTA_MAX_UNITS)
  {
    Serial.printf("BEL: prirustek %lu jednotek zahozen\n", (unsigned long)deltaUnits);
    deltaUnits = 0;
  }
  lastConsumption = consumption;
  consumptionValid = true;

  uint32_t hitsA = window->hits[DET_CHANNEL_A];
  uint32_t hitsB = window->hits[DET_CHANNEL_B];
  if(hitsA + hitsB == 0)
  {
    return;
  }
  uint64_t deltaMilliWh = (uint64_t)deltaUnits * BEL_WH_PER_UNIT * 1000ULL;
  energyMilliWhA += deltaMilliWh * hitsA / (hitsA + hitsB);
  energyMilliWhB += deltaMilliWh * hitsB / (hitsA + hitsB);
}

void PublishHomeAssistant(const BelData* data, const DetectorWindow* window)
{
  convertToHalfByte((uint32_t)data->voltage, currentFveData.voltage, 4);
  convertToHalfByte((uint32_t)data->current, currentFveData.current, 4);
  convertToHalfByte((uint32_t)data->power, currentFveData.power, 4);
  convertToHalfByte((uint32_t)(uint16_t)data->consumption, currentFveData.consumption, 4);
  currentFveData.heaterState = (uint8_t)heaterState;
  convertToHalfByte(detectorDuty(window, DET_CHANNEL_A), currentFveData.dutyA, 2);
  convertToHalfByte(detectorDuty(window, DET_CHANNEL_B), currentFveData.dutyB, 2);
  convertToHalfByte((uint32_t)(energyMilliWhA / 1000ULL), currentFveData.energyA, 8);
  convertToHalfByte((uint32_t)(energyMilliWhB / 1000ULL), currentFveData.energyB, 8);

  mqtt.publish(TOPIC_FVE, (const uint8_t*)&currentFveData, sizeof(FveData), true);
  Serial.printf("FVE publish: U=%d I=%d P=%d E=%d stav=%c EA=%lu EB=%lu\n",
    data->voltage, data->current, data->power, data->consumption,
    heaterState,
    (unsigned long)(energyMilliWhA / 1000ULL),
    (unsigned long)(energyMilliWhB / 1000ULL));
}

void PublishDiagnostics(const DetectorWindow* window)
{
  uint16_t dropouts = window->dropouts[DET_CHANNEL_A] > window->dropouts[DET_CHANNEL_B]
    ? window->dropouts[DET_CHANNEL_A]
    : window->dropouts[DET_CHANNEL_B];

  currentDiagData.uptimeMinutes = millis() / 60000UL;
  currentDiagData.freeHeapKb = heapKb(ESP.getFreeHeap());
  currentDiagData.minFreeHeapKb = heapKb(ESP.getMinFreeHeap());
  currentDiagData.otaFailCount = otaFailures;
  currentDiagData.belFrameErrors = (uint16_t)(bel.GetFrameErrors() - belFrameErrorBase);
  currentDiagData.loadDropouts = dropouts;
  currentDiagData.rawA = detectorAverage(window, DET_CHANNEL_A);
  currentDiagData.rawB = detectorAverage(window, DET_CHANNEL_B);
  currentDiagData.rippleA = window->ripple[DET_CHANNEL_A];
  currentDiagData.rippleB = window->ripple[DET_CHANNEL_B];
  currentDiagData.fwVersion = (uint16_t)FW_VERSION;
  currentDiagData.rssi = (int8_t)WiFi.RSSI();

  mqtt.publish(TOPIC_FVE_DIAG, (const uint8_t*)&currentDiagData, sizeof(DiagData), false);
  currentDiagData.loopMaxMs = 0;
  detectorLog(window, "diag", heaterState);
}

void setup()
{
  currentDiagData.resetReason = (uint8_t)esp_reset_reason();
  Serial.begin(115200);
  Serial2.setRxBufferSize(BEL_RX_BUFFER_SIZE);
  Serial2.begin(9600, SERIAL_8N1, BEL_RX_PIN, BEL_TX_PIN);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  detectorInit();

  WiFi.mode(WIFI_STA);
  esp_sntp_servermode_dhcp(true);
  WiFi.begin(WifiSSID, WifiPassword);

  net.setCACert(MQTTCACert);
  net.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
  mqtt.setServer(MQTTHost, MQTT_TLS_PORT);
  mqtt.setBufferSize(MQTT_BUFFER_SIZE);
  mqtt.setKeepAlive(MQTT_KEEP_ALIVE);

  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_task_wdt_reconfigure(&wdtConfig);
  esp_task_wdt_add(NULL);
#if !BEL_ENABLED
  belAvailability = BEL_ONLINE;
  Serial.println("BEL: vypnuty, U/I/P/E se posilaji jako nuly");
#endif
  Serial.printf("BelFVE setup OK, verze %d\n", (int)FW_VERSION);
}

void loop()
{
  unsigned long currentMillis = millis();
  esp_task_wdt_reset();

  bool connected = EnsureConnected();
  mqtt.loop();
  bel.Loop();
  detectorDrain();

#if BEL_ENABLED
  if(currentMillis - lastBelData >= BEL_SILENCE_TIMEOUT_MS)
  {
    if(belAvailability != BEL_OFFLINE)
    {
      belFrameErrorBase = bel.GetFrameErrors();
    }
    if(belAvailability == BEL_ONLINE)
    {
      bool producing = pendingBelData.voltage >= BEL_FAULT_MIN_VOLTAGE && pendingBelData.power >= BEL_FAULT_MIN_POWER;
      faultState = producing ? FAULT_SUSPECT : FAULT_OK;
      Serial.printf("BEL ticho, posledni U=%d P=%d -> %s\n", pendingBelData.voltage, pendingBelData.power, producing ? "Fault" : "Ok");
    }
    belAvailability = BEL_OFFLINE;
  }
#else
  if(currentMillis - lastHaSend >= HA_INTERVAL_MS)
  {
    pendingBelData = BelData();
    belDataPending = true;
  }
#endif
  if(connected)
  {
#if BEL_ENABLED
    PublishFault();
#endif
    PublishAvailability();
  }

  if(belDataPending)
  {
    belDataPending = false;
    lastBelData = currentMillis;
    lastHaSend = currentMillis;
    belAvailability = BEL_ONLINE;
#if BEL_ENABLED
    if(faultState != FAULT_SUSPECT || pendingBelData.power >= BEL_FAULT_CLEAR_MIN_POWER)
    {
      faultState = FAULT_OK;
    }
#endif
    DetectorWindow window;
    detectorTakeFast(&window);
    heaterState = detectorState(&window, heaterState);
    AccumulateEnergy(&pendingBelData, &window);
    detectorLog(&window, connected ? "online" : "offline", heaterState);   // DOCASNA DIAGNOSTIKA
    if(connected)
    {
      PublishHomeAssistant(&pendingBelData, &window);
    }
  }

  if(currentMillis - lastDiagSend >= DIAG_INTERVAL_MS)
  {
    lastDiagSend = currentMillis;
    DetectorWindow window;
    detectorTakeSlow(&window);
    if(connected)
    {
      PublishDiagnostics(&window);
    }
    else
    {
      detectorLog(&window, "diag-offline", heaterState);
    }
  }

  otaLoop();

  unsigned long iterDur = millis() - currentMillis;
  if(iterDur > currentDiagData.loopMaxMs)
  {
    currentDiagData.loopMaxMs = (iterDur > 65535UL) ? 65535 : (uint16_t)iterDur;
  }
}
