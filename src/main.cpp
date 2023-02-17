/**
  RFID reader firmware for the Open eXtensible Rack System
  
  GitHub repository:
    https://github.com/sumnerboy12/OXRS-BJ-RFIDReader-ESP-FW
    
  Copyright 2022 Ben Jones <ben.jones12@gmail.com>
*/

/*--------------------------- Libraries -------------------------------*/
#include <SoftwareSerial.h>
#include <NfcAdapter.h>
#include <PN532/PN532/PN532.h>

#ifdef USE_I2C_NFC
#include <Wire.h>
#include <PN532/PN532_I2C/PN532_I2C.h>
#else
#include <SPI.h>
#include <PN532/PN532_SPI/PN532_SPI.h>
#endif

#if defined(OXRS_ESP32)
#include <OXRS_32.h>                  // ESP32 support
OXRS_32 oxrs;

#elif defined(OXRS_ESP8266)
#include <OXRS_8266.h>                // ESP8266 support
OXRS_8266 oxrs;

#endif

/*--------------------------- Constants -------------------------------*/
// Serial
#define     SERIAL_BAUD_RATE              115200

// Time between tag reads
#define     DEFAULT_TAG_READ_INTERVAL_MS  200

// Max NFC tag UID length
#define     MAX_UID_BYTES                 8

/*--------------------------- Instantiate Globals ---------------------*/
// RFID reader
#ifdef USE_I2C_NFC
PN532_I2C pn532_i2c(Wire);
NfcAdapter nfc = NfcAdapter(pn532_i2c);
#else
PN532_SPI pn532_spi(SPI, SPI_SS_PIN);
NfcAdapter nfc = NfcAdapter(pn532_spi);
#endif

// Last tag read and when
uint32_t tagReadIntervalMs = DEFAULT_TAG_READ_INTERVAL_MS;
uint32_t lastTagReadMs = 0L;
byte lastUid[MAX_UID_BYTES];

/*--------------------------- Program ---------------------------------*/
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

  // does this tag have a message?
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

  // publish the tag details
  oxrs.publishStatus(json.as<JsonVariant>());
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

void setConfigSchema()
{
  // Define our config schema
  StaticJsonDocument<1024> json;
  
  JsonObject tagReadIntervalMs = json.createNestedObject("tagReadIntervalMs");
  tagReadIntervalMs["title"] = "Tag Read Interval (milliseconds)";
  tagReadIntervalMs["description"] = "How often to check if a tag is near the reader (defaults to 200 milliseconds). Must be a number between 0 and 60000 (i.e. 1 min).";
  tagReadIntervalMs["type"] = "integer";
  tagReadIntervalMs["minimum"] = 0;
  tagReadIntervalMs["maximum"] = 60000;

  // Pass our config schema down to the hardware library
  oxrs.setConfigSchema(json.as<JsonVariant>());
}

void jsonConfig(JsonVariant json)
{
  if (json.containsKey("tagReadIntervalMs"))
  {
    tagReadIntervalMs = json["tagReadIntervalMs"].as<uint32_t>();
  }
}

/**
  Initialisation
*/
void initialisePN532(void)
{
  oxrs.print("[rfid] scanning for NFC reader on ");

#ifdef USE_I2C_NFC
  oxrs.println(F("I2C"));
  Wire.begin();
#else
  oxrs.println(F("SPI"));
  SPI.begin();
#endif

  // Initialise the PN532 reader
  nfc.begin();
}

/**
  Setup
*/
void setup() 
{
  Serial.begin(SERIAL_BAUD_RATE);
  delay(1000);
  Serial.println(F("[rfid] starting up..."));
  
  // Start hardware
  oxrs.begin(jsonConfig, NULL);

  // Set up the RFID reader
  initialisePN532();

  // Set up the config schema (for self-discovery and adoption)
  setConfigSchema();
}

/**
  Main processing loop
*/
void loop() 
{
  // Let hardware handle any events etc
  oxrs.loop();

  // Check if we are ready to read another tag
  if ((millis() - lastTagReadMs) > tagReadIntervalMs)
  {
    // Process RFID reader
    processPN532();

    // Reset our timer
    lastTagReadMs = millis();
  }
}
