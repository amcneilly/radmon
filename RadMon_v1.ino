/*

ESP32 Geiger Counter with caching to flash memory
-----------------------------
Designed to be lower power avoiding continuous WiFI operation. WiFI only enabled when transmitting results online. 
Geiger counter readings stored to flash memory and then pulled at a pretertermined interval to be trasmitted to ThingsLabs for analysis. 
Alerts can be configured if radiation level exceeds a configured threshold. Sent via SMS using the inbuilt IFTTT API call.

Hardware details
----------------------------
Standard ESP32 module
Tempeture senstor DS18S20
Geiger tube M4011

Geiger Counter Kit
https://www.banggood.com/Assembled-DIY-Geiger-Counter-Kit-Module-Miller-Tube-GM-Tube-Nuclear-Radiation-Detector-p-1136883.html
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <OneWire.h>
#include <IFTTTMaker.h>
#include "time.h"

#ifdef __cplusplus
extern "C" {
#endif

uint8_t temprature_sens_read();
//uint8_t g_phyFuns;

#ifdef __cplusplus
}
#endif

//based on number of Giger counter readings 
//default Gieger reading every 1 minute LOG_PERIOD. so if this is 120 go online every 2 hours to send data
#define ReadingsTransmitThreshold 180

//Used for CPM (counts per minute) calculation from the miller tube
//Logging period in milliseconds
#define LOG_PERIOD 60000 
//1 minute value in milliseconds
#define MINUTE_PERIOD 60000


/* DS18S20 Temperature chip i/o */
OneWire  ds(27);  // on pin 27

#ifndef CREDENTIALS
// WLAN
#define ssid  "SSID"
#define password "PASSWORD"

// IFTTT KEY used for SMS ALERT
#define IFTTT_KEY "IFTTTKEY"
#endif

// ThingSpeak Settings used to store and analyse data
const char* server = "api.thingspeak.com";
const char* Thingspeak_API_KEY = "APIKEY";
const char* Thingspeak_Channel_ID = "ChannelID";

// ThingSpeak Settings
const char* server = "api.thingspeak.com";
const char* Thingspeak_API_KEY = "APIKEY";
const char* Thingspeak_Channel_ID = "ChannelID";

//Constant configured for Geiger tube M4011
const float cpmConversation = 0.008120;

bool alerted = false;

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 36000;
const int   daylightOffset_sec = 0;

//crude data storage
char dataStore [60000];
int usedData = 0;

IPAddress ip;
WiFiClientSecure secure_client;

IFTTTMaker ifttt(IFTTT_KEY, secure_client);

volatile unsigned long counts = 0;                       // Tube events
unsigned long cpm = 0;                                   // CPM
float uSvH = 0;
float caseTemp = 0;
float cpuTemp = 0;
unsigned long previousMillis;                            // Time measurement
unsigned long lastAlertMillis;                            // Time measurement
const int inputPin = 26;

void ISR_impulse() { // Captures count of events from Geiger counter board
  counts++;
}


void printLocalTime()
{
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return;
  }
  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
}

void setup()
{
  Serial.begin(115200);

  lastAlertMillis = 0;
  
  //connect to WiFi
  Serial.printf("Connecting to %s for NTP setup", ssid);

  WiFi.disconnect(false);
  WiFi.mode(WIFI_STA);
  
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
  }
  Serial.println(" CONNECTED");

  delay(5000);
  
  //init and get the time
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  printLocalTime();

  //disconnect WiFi as it's no longer needed
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  pinMode(inputPin, INPUT);                                                // Set pin for capturing Tube events
  interrupts();                                                            // Enable interrupts
  attachInterrupt(digitalPinToInterrupt(inputPin), ISR_impulse, FALLING); // Define interrupt on falling edge

}

int logEntires = 0;

void loop()
{
  unsigned long currentMillis = millis();

  //based on number of readings. default reading every 1 minute. so if this is 5 then every 5. minutes 
  if(logEntires >= ReadingsTransmitThreshold) {

    logEntires = 0;

    //log data to internet
    PushLogs();
  } 
  else if (currentMillis - previousMillis > LOG_PERIOD) {

    previousMillis = currentMillis;

    PrintValues();

    RadCalc();

    LogData();
  }
  else
  {
    //NB we dont do RadCalc as we need to capture CPM over a time period

    CaseTempCalc();

    CPUTempCalc();

    ThresholdCheck();

    delay(250);
  }


  if(usedData >= 60000)
  {
    Serial.println("ERROR USED TOO MUCH MEMEORY");
  }
}

void PrintValues()
{
  Serial.println("Measurements");
  Serial.println("cpm : " + String(cpm));
  Serial.println("uSvH : " + String(uSvH));
  Serial.println("caseTemp : " + String(caseTemp));
  Serial.println("cpuTemp : " + String(cpuTemp));
  Serial.println("");
  Serial.println("debug usedData : " + String(usedData));
  Serial.println("debug logEntires : " + String(logEntires));
  Serial.println("");
  
}

//Convert counts to CPM / uSvH
void RadCalc()
{
  cpm = counts * MINUTE_PERIOD / LOG_PERIOD;
  uSvH = cpm * cpmConversation;

  counts = 0;
}


//check if we need to do immediate notfication reporting
void ThresholdCheck()
{
  Serial.println("ThresholdCheck");

  unsigned long currentMillis = millis();

  //30 minutes between alerts if triggered
  if ((currentMillis - lastAlertMillis > 1800000) || lastAlertMillis == 0)
  {
    //Serial.println("checking values ...");
  
    if (uSvH >= 0.75f )
      IFTTT( "AbnormalRadiationLevels", String(uSvH));
  
    if (caseTemp >= 48.0f)
      IFTTT( "CaseTempHigh", String(caseTemp));
  }
  else
  {
    Serial.println("Too soon before last alert to fire again. need to wait 30 minutes");
  }
}

//log data to internal memory strcuture
void LogData() {

  //get Time
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return;
  }

  char buffer[26];
  strftime(buffer, 26, "%Y-%m-%d %H:%M:%S +1000", &timeinfo);

  if(logEntires == 0) {
    String startHead = "{\"write_api_key\":\"" + Thingspeak_API_KEY + "\",\"updates\":[";
    strncpy(&dataStore[usedData], startHead.c_str(), startHead.length());
    usedData += startHead.length();
  }

  String head1 = "{\"created_at\":\"";
  strncpy(&dataStore[usedData], head1.c_str(), head1.length());
  usedData += head1.length();
  strncpy(&dataStore[usedData], buffer, 25);
  usedData += 25;

  LogFieldToData("field1", String(cpm), false);
  LogFieldToData("field2", String(uSvH), false);
  LogFieldToData("field3", String(caseTemp), false);
  LogFieldToData("field4", String(cpuTemp), true);

  strncpy(&dataStore[usedData], "},", 2);
  usedData += 2;

  logEntires++;
}

void LogFieldToData(String fname, String value, bool lastValue)
{
  String head2 = "\",\"" + fname + "\": \"";
  strncpy(&dataStore[usedData], head2.c_str(), head2.length());
  usedData += head2.length();
  strncpy(&dataStore[usedData], value.c_str(), value.length());
  usedData += value.length();

  strncpy(&dataStore[usedData], "\"", 1);

  if(lastValue)
  {
    strncpy(&dataStore[usedData], "\"", 1);
    usedData += 1;
  }

}

//Push logs to Internet. clear local memory strcuture
void PushLogs() {

  //terminate header
  String endHead = "]}";
  strncpy(&dataStore[usedData-1], endHead.c_str(), endHead.length());
  usedData += endHead.length();
   
  HTTPClient client;

  WiFi.disconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long startMills = millis();
  
  while (WiFi.status() != WL_CONNECTED) {

      unsigned long cMills = millis();
       
      delay(500);

      if (millis() - startMills > 60000) {
        Serial.println("Could not connect to WIFI. Aborting");
        return;
      }
  }
  
  Serial.println("Connected to WIFI");

  client.begin("http://api.thingspeak.com/channels/" + Thingspeak_Channel_ID + "/bulk_update.json");

  Serial.println("posting");

    client.addHeader("Content-Type", "application/json");
    client.addHeader("Cache-Control", "no-cache");

    Serial.println("Posting Data");
    Serial.println(dataStore);

    int httpCode = client.POST(dataStore);
    String payload = client.getString();
    
    Serial.println(httpCode);   //Print HTTP return code
    Serial.println(payload);    //Print request response payload
    
    client.end();  //Close connection

  delay(5000);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  
  Serial.println("Disconnected from WIFI");
  Serial.println("");
 
  //local memory store ready to be cleared
  usedData = 0;
}

void CaseTempCalc()
{
  byte i;
  byte present = 0;
  byte type_s;
  byte data[12];
  byte addr[8];
   
  if ( !ds.search(addr))
  {
    ds.reset_search();
    delay(250);
    return;
  }
   
  if (OneWire::crc8(addr, 7) != addr[7])
  {
    Serial.println("CRC is not valid!");
    return;
  }

  // the first ROM byte indicates which chip
  switch (addr[0])
  {
    case 0x10:
      type_s = 1;
      break;
    case 0x28:
      type_s = 0;
      break;
    case 0x22:
      type_s = 0;
      break;
    default:
      Serial.println("Device is not a DS18x20 family device.");
      return;
  }
   
  ds.reset();
  ds.select(addr);
  ds.write(0x44, 1); // start conversion, with parasite power on at the end
  delay(250);
  present = ds.reset();
  ds.select(addr);
  ds.write(0xBE); // Read Scratchpad
   
  for ( i = 0; i < 9; i++)
  {
    data[i] = ds.read();
  }
   
  // Convert the data to actual temperature
  int16_t raw = (data[1] << 8) | data[0];
  
  if (type_s) {
    raw = raw << 3; // 9 bit resolution default
    if (data[7] == 0x10)
    {
      raw = (raw & 0xFFF0) + 12 - data[6];
    }
  }
  else
  {
    byte cfg = (data[4] & 0x60);
    if (cfg == 0x00) raw = raw & ~7; // 9 bit resolution, 93.75 ms
    else if (cfg == 0x20) raw = raw & ~3; // 10 bit res, 187.5 ms
    else if (cfg == 0x40) raw = raw & ~1; // 11 bit res, 375 ms
  }
  
  caseTemp = (float)raw / 16.0;
}

void CPUTempCalc() {
  
  cpuTemp = (temprature_sens_read() - 32 ) / 1.8;
  
  delay(250);
}

void IFTTT(String event, String postValue) {

  Serial.println("IFTTT() " + event + " " + postValue);

  WiFi.disconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  delay(5000);
  
  if (ifttt.triggerEvent(event, String(postValue))) {
    Serial.println("Successfully sent to IFTTT");
  }
  else
  {
    Serial.println("IFTTT failed!");
  }

  delay(5000);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  lastAlertMillis = millis();
}


