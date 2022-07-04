/**
  ESP8266 RFID reader firmware for the Open eXtensible Rack System
  
  GitHub repository:
    https://github.com/sumnerboy12/OXRS-BJ-RFIDReader-ESP-FW
    
  Copyright 2022 Ben Jones <ben.jones12@gmail.com>
*/

/*--------------------------- Macros ----------------------------------*/
#define STRINGIFY(s) STRINGIFY1(s)
#define STRINGIFY1(s) #s

/*--------------------------- Libraries -------------------------------*/
#include <Wire.h>
#include <PN532/PN532/PN532.h>
#include <PN532/PN532_I2C/PN532_I2C.h>
#include <NfcAdapter.h>
#include <SoftwareSerial.h>

#include <WiFiManager.h>
#include <OXRS_MQTT.h>
#include <OXRS_API.h>
#include <MqttLogger.h>

#include <ESP8266WiFi.h>

/*--------------------------- Constants -------------------------------*/
// Serial
#define     SERIAL_BAUD_RATE              115200

// REST API
#define     REST_API_PORT                 80

// Time between tag reads
#define     DEFAULT_TAG_READ_INTERVAL_MS  200

// Max NFC tag UID length
#define     MAX_UID_BYTES                 8

/*--------------------------- Global Variables ------------------------*/
// stack size counter
char * stackStart;

/*--------------------------- Instantiate Globals ---------------------*/
// WiFi client
WiFiClient _client;

// MQTT client
PubSubClient _mqttClient(_client);
OXRS_MQTT _mqtt(_mqttClient);

// REST API
WiFiServer _server(REST_API_PORT);
OXRS_API _api(_mqtt);

// Logging
MqttLogger _logger(_mqttClient, "log", MqttLoggerMode::MqttOnly);

// RFID reader
PN532_I2C pn532_i2c(Wire);
NfcAdapter nfc = NfcAdapter(pn532_i2c);

// Last tag read and when
uint32_t tagReadIntervalMs = DEFAULT_TAG_READ_INTERVAL_MS;
uint32_t lastTagReadMs = 0L;
byte lastUid[MAX_UID_BYTES];

/*--------------------------- Program ---------------------------------*/
uint32_t getStackSize()
{
  char stack;
  return (uint32_t)stackStart - (uint32_t)&stack;  
}

char * toHexString(char buffer[], byte data[], uint8_t len)
{
  for (uint8_t i = 0; i < len; i++)
  {
    byte nib1 = (data[i] >> 4) & 0x0F;
    byte nib2 = (data[i] >> 0) & 0x0F;

    buffer[i*2+0] = nib1  < 0xA ? '0' + nib1  : 'A' + nib1 - 0xA;
    buffer[i*2+1] = nib2  < 0xA ? '0' + nib2  : 'A' + nib2 - 0xA;
  }

  buffer[len*2] = '\0';
  return buffer;
}

char * toAsciiString(char buffer[], byte data[], uint8_t len)
{
  for (uint8_t i = 0; i < len; i++)
  {
    if (data[i] <= 0x1F) 
    {
      buffer[i] = '.';
    } 
    else 
    {
      buffer[i] = (char)data[i];
    }
  }

  buffer[len] = '\0';
  return buffer;
}

void publishTag(NfcTag * tag)
{
  // get the tag UID
  byte uid[MAX_UID_BYTES];
  tag->getUid(uid, tag->getUidLength());

  // build the JSON payload with the tag details
  DynamicJsonDocument json(4096);
  char buffer[1024];

  json["uid"] = toHexString(buffer, uid, tag->getUidLength());
  json["type"] = tag->getTagType();

  // does this tag have a message encoded?
  if (tag->hasNdefMessage())
  {
    NdefMessage ndefMessage = tag->getNdefMessage();

    JsonArray recordsJson = json.createNestedArray("records");
    for (uint8_t i = 0; i < ndefMessage.getRecordCount(); i++)
    {
      NdefRecord ndefRecord = ndefMessage.getRecord(i);

      int payloadLength = ndefRecord.getPayloadLength();
      byte payload[payloadLength];
      ndefRecord.getPayload(payload);

      JsonObject recordJson = recordsJson.createNestedObject();
      recordJson["tnf"] = ndefRecord.getTnf();
      recordJson["type"] = ndefRecord.getType();
      recordJson["id"] = ndefRecord.getId();
      recordJson["bytes"] = ndefRecord.getEncodedSize();

      JsonObject payloadJson = recordJson.createNestedObject("payload");
      payloadJson["hex"] = toHexString(buffer, payload, payloadLength);
      payloadJson["ascii"] = toAsciiString(buffer, payload, payloadLength);
    }
  }

  _mqtt.publishStatus(json.as<JsonVariant>());
}

void processPN532() 
{
  // if no tag present then ensure we are ready to read a new one
  if (!nfc.tagPresent(5))
  {
    memset(lastUid, 0, MAX_UID_BYTES);
    return;
  }

  // read the tag details
  NfcTag tag = nfc.read();

  // get the tag UID
  byte uid[MAX_UID_BYTES];
  tag.getUid(uid, tag.getUidLength());

  // if the tag hasn't changed then nothing to do
  if (memcmp(uid, lastUid, tag.getUidLength()) == 0) 
    return;

  // save the tag UID so we can ignore re-reads
  memcpy(lastUid, uid, tag.getUidLength());

  // publish the tag details
  publishTag(&tag);
}

void getFirmwareJson(JsonVariant json)
{
  JsonObject firmware = json.createNestedObject("firmware");

  firmware["name"] = FW_NAME;
  firmware["shortName"] = FW_SHORT_NAME;
  firmware["maker"] = FW_MAKER;
  firmware["version"] = STRINGIFY(FW_VERSION);

#if defined(FW_GITHUB_URL)
  firmware["githubUrl"] = FW_GITHUB_URL;
#endif
}

void getSystemJson(JsonVariant json)
{
  JsonObject system = json.createNestedObject("system");

  system["heapUsedBytes"] = getStackSize();
  system["heapFreeBytes"] = ESP.getFreeHeap();
  system["flashChipSizeBytes"] = ESP.getFlashChipSize();

  system["sketchSpaceUsedBytes"] = ESP.getSketchSize();
  system["sketchSpaceTotalBytes"] = ESP.getFreeSketchSpace();

  FSInfo fsInfo;
  SPIFFS.info(fsInfo);
  
  system["fileSystemUsedBytes"] = fsInfo.usedBytes;
  system["fileSystemTotalBytes"] = fsInfo.totalBytes;
}

void getNetworkJson(JsonVariant json)
{
  byte mac[6];
  WiFi.macAddress(mac);
  
  char mac_display[18];
  sprintf_P(mac_display, PSTR("%02X:%02X:%02X:%02X:%02X:%02X"), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  JsonObject network = json.createNestedObject("network");

  network["mode"] = "wifi";
  network["ip"] = WiFi.localIP();
  network["mac"] = mac_display;
}

void getConfigSchemaJson(JsonVariant json)
{
  JsonObject configSchema = json.createNestedObject("configSchema");
  
  // Config schema metadata
  configSchema["$schema"] = JSON_SCHEMA_VERSION;
  configSchema["title"] = FW_SHORT_NAME;
  configSchema["type"] = "object";

  JsonObject properties = configSchema.createNestedObject("properties");

  JsonObject tagReadIntervalMs = properties.createNestedObject("tagReadIntervalMs");
  tagReadIntervalMs["title"] = "Tag Read Interval (milliseconds)";
  tagReadIntervalMs["description"] = "How often to check for a new tag near the reader (defaults to 200 milliseconds). Must be a number between 0 and 60000 (i.e. 1 min).";
  tagReadIntervalMs["type"] = "integer";
  tagReadIntervalMs["minimum"] = 0;
  tagReadIntervalMs["maximum"] = 60000;
}

void getCommandSchemaJson(JsonVariant json)
{
  JsonObject commandSchema = json.createNestedObject("commandSchema");
  
  // Command schema metadata
  commandSchema["$schema"] = JSON_SCHEMA_VERSION;
  commandSchema["title"] = FW_SHORT_NAME;
  commandSchema["type"] = "object";

  JsonObject properties = commandSchema.createNestedObject("properties");
}

/**
  API callbacks
*/
void _apiAdopt(JsonVariant json)
{
  // Build device adoption info
  getFirmwareJson(json);
  getSystemJson(json);
  getNetworkJson(json);
  getConfigSchemaJson(json);
  getCommandSchemaJson(json);
}

/**
  MQTT callbacks
*/
void _mqttConnected()
{
  // MqttLogger doesn't copy the logging topic to an internal
  // buffer so we have to use a static array here
  static char logTopic[64];
  _logger.setTopic(_mqtt.getLogTopic(logTopic));

  // Publish device adoption info
  DynamicJsonDocument json(JSON_ADOPT_MAX_SIZE);
  _mqtt.publishAdopt(_api.getAdopt(json.as<JsonVariant>()));

  // Log the fact we are now connected
  _logger.println("[rfid] mqtt connected");
}

void _mqttDisconnected(int state) 
{
  // Log the disconnect reason
  // See https://github.com/knolleary/pubsubclient/blob/2d228f2f862a95846c65a8518c79f48dfc8f188c/src/PubSubClient.h#L44
  switch (state)
  {
    case MQTT_CONNECTION_TIMEOUT:
      _logger.println(F("[rfid] mqtt connection timeout"));
      break;
    case MQTT_CONNECTION_LOST:
      _logger.println(F("[rfid] mqtt connection lost"));
      break;
    case MQTT_CONNECT_FAILED:
      _logger.println(F("[rfid] mqtt connect failed"));
      break;
    case MQTT_DISCONNECTED:
      _logger.println(F("[rfid] mqtt disconnected"));
      break;
    case MQTT_CONNECT_BAD_PROTOCOL:
      _logger.println(F("[rfid] mqtt bad protocol"));
      break;
    case MQTT_CONNECT_BAD_CLIENT_ID:
      _logger.println(F("[rfid] mqtt bad client id"));
      break;
    case MQTT_CONNECT_UNAVAILABLE:
      _logger.println(F("[rfid] mqtt unavailable"));
      break;
    case MQTT_CONNECT_BAD_CREDENTIALS:
      _logger.println(F("[rfid] mqtt bad credentials"));
      break;      
    case MQTT_CONNECT_UNAUTHORIZED:
      _logger.println(F("[rfid] mqtt unauthorised"));
      break;      
  }
}

void _mqttCallback(char * topic, byte * payload, int length)
{
  // Pass down to our MQTT handler
  _mqtt.receive(topic, payload, length);
}

void _mqttConfig(JsonVariant json)
{
  if (json.containsKey("tagReadIntervalMs"))
  {
    tagReadIntervalMs = json["tagReadIntervalMs"].as<uint32_t>();
  }
}

void _mqttCommand(JsonVariant json)
{
}

/**
  Initialisation
*/
void initialiseSerial()
{
  Serial.begin(SERIAL_BAUD_RATE);
  delay(1000);
  
  _logger.println(F("[rfid] starting up..."));

  DynamicJsonDocument json(128);
  getFirmwareJson(json.as<JsonVariant>());

  _logger.print(F("[rfid] "));
  serializeJson(json, _logger);
  _logger.println();
}

void initialseWifi(byte * mac)
{
  // Ensure we are in the correct WiFi mode
  WiFi.mode(WIFI_STA);

  // Connect using saved creds, or start captive portal if none found
  // Blocks until connected or the portal is closed
  WiFiManager wm;  
  if (!wm.autoConnect("OXRS_WiFi", "superhouse"))
  {
    // If we are unable to connect then restart
    ESP.restart();
  }
  
  // Get ESP8266 base MAC address
  WiFi.macAddress(mac);

  // Format the MAC address for display
  char mac_display[18];
  sprintf_P(mac_display, PSTR("%02X:%02X:%02X:%02X:%02X:%02X"), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  // Display MAC/IP addresses on serial
  _logger.print(F("[rfid] mac address: "));
  _logger.println(mac_display);  
  _logger.print(F("[rfid] ip address: "));
  _logger.println(WiFi.localIP());
}

void initialiseMqtt(byte * mac)
{
  // Set the default client id to the last 3 bytes of the MAC address
  char clientId[32];
  sprintf_P(clientId, PSTR("%02x%02x%02x"), mac[3], mac[4], mac[5]);  
  _mqtt.setClientId(clientId);
  
  // Register our callbacks
  _mqtt.onConnected(_mqttConnected);
  _mqtt.onDisconnected(_mqttDisconnected);
  _mqtt.onConfig(_mqttConfig);
  _mqtt.onCommand(_mqttCommand);
  
  // Start listening for MQTT messages
  _mqttClient.setCallback(_mqttCallback);  
}

void initialiseRestApi(void)
{
  // NOTE: this must be called *after* initialising MQTT since that sets
  //       the default client id, which has lower precendence than MQTT
  //       settings stored in file and loaded by the API

  // Set up the REST API
  _api.begin();

  // Register our callbacks
  _api.onAdopt(_apiAdopt);

  _server.begin();
}

void initialisePN532(void)
{
  // Initialise the PN532 reader
  nfc.begin();
}

/**
  Setup
*/
void setup() 
{
  // Store the address of the stack at startup so we can determine
  // the stack size at runtime (see getStackSize())
  char stack;
  stackStart = &stack;
  
  // Set up serial
  initialiseSerial();  

  // Set up network and obtain an IP address
  byte mac[6];
  initialseWifi(mac);

  // initialise MQTT
  initialiseMqtt(mac);

  // Set up the REST API
  initialiseRestApi();

  // Set up the RFID reader
  initialisePN532();
}

/**
  Main processing loop
*/
void loop() 
{
  // Check our MQTT broker connection is still ok
  _mqtt.loop();

  // Handle any REST API requests
  WiFiClient client = _server.available();
  _api.loop(&client);

  // Check if we are ready to read another tag
  if ((millis() - lastTagReadMs) > tagReadIntervalMs)
  {
    // Process RFID reader
    processPN532();

    // Reset our timer
    lastTagReadMs = millis();
  }
}
