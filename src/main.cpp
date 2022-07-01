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
#include <NfcAdapter.h>
#include <PN532/PN532/PN532.h>
#include <PN532/PN532_HSU/PN532_HSU.h>

#include <SoftwareSerial.h>
#include <WiFiManager.h>
#include <OXRS_MQTT.h>
#include <OXRS_API.h>
#include <MqttLogger.h>

#include <ESP8266WiFi.h>

/*--------------------------- Constants -------------------------------*/
// Serial
#define     SERIAL_BAUD_RATE          115200

// REST API
#define     REST_API_PORT             80

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
PN532_HSU pn532hsu(Serial);
NfcAdapter nfc(pn532hsu);

/*--------------------------- Program ---------------------------------*/
uint32_t getStackSize()
{
  char stack;
  return (uint32_t)stackStart - (uint32_t)&stack;  
}

void processPN532() 
{
    _logger.println("\nScan a NFC tag\n");
    if (nfc.tagPresent()) {
        NfcTag tag = nfc.read();
        tag.print();
    }
    delay(5000);
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

  // Process RFID reader
  processPN532();
}
