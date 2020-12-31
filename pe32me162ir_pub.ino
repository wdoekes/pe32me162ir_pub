/**
 * pe32me162ir_pub // Talk to optical port of ISKRA ME-162, export to MQTT
 *
 * Components:
 * - ISKRA ME-162 electronic meter with optical port
 * - a digital IR-transceiver from https://wiki.hal9k.dk/projects/kamstrup --
 *   BEWARE: be sure to check the direction of the two BC547 transistors.
 *   The pictures on the wiki have them backwards. Check the
 *   https://wiki.hal9k.dk/_media/projects/kamstrup-schem-v2.pdf !
 * - ESP8266 (NodeMCU, with Wifi) _or_ an Arduino (Uno?). Wifi/MQTT push
 *   support is only(!) available for the ESP8266 at the moment.
 * - attach PIN_IR_RX<->RX, PIN_IR_TX<->TX, 3VC<->VCC (or 5VC), GND<->GND
 *
 * Dependencies:
 * - Arduino IDE
 * - (for ESP8266) board: package_esp8266com_index.json
 * - (for ESP8266) library: ArduinoMqttClient
 * - (for Arduino) library (manually): CustomSoftwareSerial (by ledongthuc)
 *
 * Configuration:
 * - Connect the pins as specified above and below (PIN_IR_RX, PIN_IR_TX);
 * - connect the IR-tranceiver to the ISKA ME-162, with _our_ TX on the left
 *   and _our_ RX on the right;
 * - create a config.h next to this file (see below, at #include "config.h").
 *
 * ISKRA ME-162 electricity meter notes:
 * > The optical port complies with the IEC 62056-21 (IEC
 * > 61107) standard, a mode C protocol is employed;
 * > data transmission rate is 9600 bit/sec.
 * ...
 * > The optical port wavelength is 660 nm and luminous
 * > intensity is min. 1 mW/sr for the ON state.
 *
 * IEC 62056-21 mode C:
 * - is a bidirectional ASCII protocol;
 * - that starts in 300 baud with 1 start bit, 7 data bits, 1 (even)
 *   parity bit and one stop bit;
 * - can be upgraded to higher baud rate (9600) after the handshake;
 * - and allows manufacturer-specific extensions.
 * - see: github.com/lvzon/dsmr-p1-parser/blob/master/doc/IEC-62056-21-notes.md
 *
 * TODO:
 * - clean up duplicate code/states with 1.8.0/2.8.0;
 * - can we get the device to poke us? / more granular Watts
 * - fix another low mem warning (probably goes when we remove lastState debug)
 */

#if defined(ARDUINO_ARCH_ESP8266)
/* On the ESP8266, the baud rate needs to be sufficiently high so it
 * doesn't affect the SoftwareSerial. (Probably because this Serial is
 * synchronous?) */
const int SERMON_BAUD = 115200; // serial monitor for debugging
const int PIN_IR_RX = 5;        // D1 / GPIO5
const int PIN_IR_TX = 4;        // D2 / GPIO4
#else /*defined(ARDUINO_ARCH_AVR)*/
/* On the Arduino Uno, the baud rate needs to be exactly 9600. If we
 * increase it, the CustomSoftwareSerial starts communicating crap. */
const int SERMON_BAUD = 9600;   // serial monitor for debugging
const int PIN_IR_RX = 9;        // digital pin 9
const int PIN_IR_TX = 10;       // digital pin 10
#endif

/* In config.h, you should have:
const char wifi_ssid[] = "<ssid>";
const char wifi_password[] = "<password>";
const char mqtt_broker[] = "192.168.1.2";
const int  mqtt_port = 1883;
const char mqtt_topic[] = "some/topic";
*/
#include "config.h"

/* Include files specific to the platform (ESP8266, Arduino or TEST) */
#if defined(ARDUINO_ARCH_ESP8266)
# include <Arduino.h> /* Serial, pinMode, INPUT, OUTPUT, ... */
# include <SoftwareSerial.h>
# define HAVE_MQTT
#elif defined(ARDUINO_ARCH_AVR)
# include <Arduino.h> /* Serial, pinMode, INPUT, OUTPUT, ... */
# include <CustomSoftwareSerial.h>
# define SoftwareSerial CustomSoftwareSerial
# define SWSERIAL_7E1 CSERIAL_7E1
#elif defined(TEST_BUILD)
# include "BogoArduino.h"
# include "BogoSoftwareSerial.h"
# include "BogoSerial.h"
#else
# error Unsupported platform
#endif

/* Include files specific to Wifi/MQTT */
#ifdef HAVE_MQTT
# include <ArduinoMqttClient.h>
# include <ESP8266WiFi.h>
static void ensure_wifi();
static void ensure_mqtt();
#else
static inline void ensure_wifi() {} /* noop */
static inline void ensure_mqtt() {} /* noop */
#endif

#define VERSION "v0"


enum State {
  STATE_START = 0,
  STATE_EXPECT_HELLO,
  STATE_ENTER_DATA_MODE,
  STATE_EXPECT_DATA_READOUT,
  STATE_EXPECT_DATA_READOUT_SLOW,
  STATE_UPGRADE,
  STATE_START2,
  STATE_EXPECT_HELLO2,
  STATE_ENTER_PROGRAMMING_MODE,
  STATE_EXPECT_PROGRAMMING_MODE,
  STATE_REQUEST_1_8_0,
  STATE_EXPECT_1_8_0,
  STATE_REQUEST_2_8_0,
  STATE_EXPECT_2_8_0,
  STATE_PUSH,
  STATE_SLEEP
};

/* Calculate and (optionally) check block check character (BCC) */
static int din_66219_bcc(const char *s);

/* Events */
static void on_hello(const char *data, size_t end, State st);
static void on_data_readout(const char *data, size_t end);
static void on_response(const char *data, size_t end, State st);
static void on_push();

/* ASCII control codes */
const char C_SOH = '\x01';
#define S_SOH "\x01"
const char C_STX = '\x02';
#define S_STX "\x02"
const char C_ETX = '\x03';
#define S_ETX "\x03"
const char C_ACK = '\x06';
#define S_ACK "\x06"
const char C_NAK = '\x15';
#define S_NAK "\x15"

/* We use the guid to store something unique to identify the device by.
 * For now, we'll populate it with the ESP8266 Wifi MAC address,
 * if available. */
static char guid[24] = "<no_wifi_found>"; // "EUI48:11:22:33:44:55:66"

#ifdef HAVE_MQTT
WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);
#endif

/* We need a (Custom)SoftwareSerial because the Arduino Uno does not do
 * 300 baud. Once we get up to speed, we _could_ use the HardwareSerial
 * instead. (But we don't, right now.)
 * - the Arduino Uno Hardware Serial does not support baud below 1200;
 * - IR-communication starts with 300 baud;
 * - using SoftwareSerial allows us to use the hardware serial monitor
 *   for debugging;
 * - Arduino SoftwareSerial does only SERIAL_8N1 (8bit, no parity, 1 stop bit)
 *   (so we use CustomSoftwareSerial; for ESP8266 the included
 *   SoftwareSerial already includes different modes);
 * - we require SERIAL_7E1 (7bit, even parity, 1 stop bit).
 * Supply RX pin, TX pin, inverted=false. Our IR-device uses:
 * HIGH == no TX light == (serial) idle */
SoftwareSerial iskra(PIN_IR_RX, PIN_IR_TX, false);

State state, nextState;
unsigned long lastStateChange;

size_t recvBufPos;
const int recvBufSize = 1023;
char recvBuf[recvBufSize + 1];

char deviceHello[16];

long deltaValue[2] = {-1, -1};
long deltaTime[2] = {-1, -1};
long lastValue[2] = {-1, -1};
long lastTime[2] = {-1, -1};

static inline void trace_rx_buffer() {
#if defined(ARDUINO_ARCH_ESP8266)
  /* On the ESP8266, the SoftwareSerial.available() never returns true
   * consecutive times: that means that we'd end up doing the trace()
   * for _every_ received character.
   * And when we have (slow) 9600 baud on the debug-Serial, this messes
   * up communication with the IR-Serial: some echoes appeared in a
   * longer receive buffer).
   * Solution: no tracing. */
#else
  if (recvBufPos) {
    Serial.print(F("<< "));
    Serial.print(recvBuf);
    Serial.println(F(" (cont)"));
  }
#endif
}

void setup()
{
  Serial.begin(SERMON_BAUD);
  while (!Serial)
    delay(0);

#ifdef HAVE_MQTT
  strncpy(guid, "EUI48:", 6);
  strncpy(guid + 6, WiFi.macAddress().c_str(), sizeof(guid) - (6 + 1));
#endif

  pinMode(PIN_IR_RX, INPUT);
  pinMode(PIN_IR_TX, OUTPUT);

  // Welcome message
  Serial.print(F("Booted pe32me162ir_pub " VERSION " guid "));
  Serial.println(guid);

  // Initial connect (if available)
  ensure_wifi();
  ensure_mqtt();

  state = nextState = STATE_START;
  lastStateChange = millis();
}

void loop()
{
  switch (state) {

  /* #1: At 300 baud, we send "/?!<CR><LF>" or "/?1!<CR><LF>" */
  case STATE_START:
  case STATE_START2:
    Serial.println(F(">> /?!<CR><LF>"));
    /* Communication starts at 300 baud, at 1+7+1+1=10 bits/septet. So, for
     * 30 septets/second, we could wait 33.3ms when there is nothing. */
    iskra.begin(300, SWSERIAL_7E1);
    /* According to spec, the time between the reception of a message
     * and the transmission of an answer is: between 200ms (or 20ms) and
     * 1500ms. So adding an appropriate delay(200) before send should be
     * sufficient. */
    delay(200);
    iskra.print(F("/?!\r\n"));
    nextState = (state == STATE_START
      ? STATE_EXPECT_HELLO : STATE_EXPECT_HELLO2);
    break;

  /* #2: We receive "/ISK5ME162-0033<CR><LF>" */
  case STATE_EXPECT_HELLO:
  case STATE_EXPECT_HELLO2:
    if (iskra.available()) {
      bool done = false;
      while (iskra.available() && recvBufPos < recvBufSize) {
        char c = iskra.read();
        /* We've received (at least three) 0x7f's at the start. Ignore
         * all of them, as we won't expect them in the hello anyway. */
        if (recvBufPos == 0 && c == 0x7f) {
#if defined(ARDUINO_ARCH_AVR)
          Serial.println(F("<< skipping 0x7f")); // only observed on Arduino
#endif
        } else {
          recvBuf[recvBufPos++] = c;
          recvBuf[recvBufPos] = '\0';
        }
        if (c == '\n' && recvBufPos >= 2 && recvBuf[recvBufPos - 2] == '\r') {
          recvBuf[recvBufPos - 2] = '\0'; /* drop <CR><LF> */
          on_hello(recvBuf + 1, recvBufPos - 3, state);
          done = true;
          break;
        }
      }
      if (!done)
        trace_rx_buffer();
    }
    /* When there is no data, we could wait 30ms for another 10 bits.
     * But we've seen odd thing happen. Busy-loop instead. */
    break;

  /* #3: We send an ACK with "speed 5" to switch to 9600 baud */
  case STATE_ENTER_DATA_MODE:
  case STATE_ENTER_PROGRAMMING_MODE:
    // ACK V Z Y:
    //   V = protocol control (0=normal, 1=2ndary, ...)
    //   Z = 0=NAK or char[3] in HELLO for ACK speed change (9600 for ME162)
    //   Y = mode control (0=readout, 1=programming, 2=binary)
    if (state == STATE_ENTER_DATA_MODE) {
      Serial.println(F(">> <ACK>050<CR><LF>"));
      delay(200);
      iskra.print(F(S_ACK "050\r\n"));
    } else {
      Serial.println(F(">> <ACK>051<CR><LF>"));
      delay(200);
      iskra.print(F(S_ACK "051\r\n")); // 051=9600baud, 001=300baud (NAK speed)
    }
    /* Assuming that the begin(NEW_SPEED) does not affect the
     * previous print() jobs: we can read/write immediately. */
    iskra.begin(9600, SWSERIAL_7E1);
    nextState = (state == STATE_ENTER_DATA_MODE
      ? STATE_EXPECT_DATA_READOUT : STATE_EXPECT_PROGRAMMING_MODE);
    break;

  /* #4: We expect "<STX>$EDIS_DATA<ETX>$BCC" with power info. */
  case STATE_EXPECT_DATA_READOUT:
  case STATE_EXPECT_DATA_READOUT_SLOW:
  case STATE_EXPECT_PROGRAMMING_MODE: /* <SOH>P0<STX>()<ETX>$BCC */
  case STATE_EXPECT_1_8_0:            /* <STX>(0032835.698*kWh)<ETX>$BCC */
  case STATE_EXPECT_2_8_0:            /* <STX>(0000000.001*kWh)<ETX>$BCC */
    if (iskra.available()) {
      bool done = false;
      while (iskra.available() && recvBufPos < recvBufSize) {
        char c = iskra.read();
        /* We'll receive some 0x7f's at the start. Ignore them. */
        if (recvBufPos == 0 && c == 0x7f) {
#if defined(ARDUINO_ARCH_AVR)
          Serial.println(F("<< skipping 0x7f")); // only observed on Arduino
#endif
        } else {
          recvBuf[recvBufPos++] = c;
          recvBuf[recvBufPos] = '\0';
        }
        if (c == C_NAK) {
          switch (state) {
          case STATE_EXPECT_DATA_READOUT:
            nextState = STATE_ENTER_DATA_MODE; break;
          case STATE_EXPECT_DATA_READOUT_SLOW:
            nextState = STATE_SLEEP; break;
          case STATE_EXPECT_PROGRAMMING_MODE:
            nextState = STATE_ENTER_PROGRAMMING_MODE; break;
          case STATE_EXPECT_1_8_0:
            nextState = STATE_REQUEST_1_8_0; break;
          case STATE_EXPECT_2_8_0:
            nextState = STATE_REQUEST_2_8_0; break;
          }
          break;
        }
        if (recvBufPos >= 2 && recvBuf[recvBufPos - 2] == C_ETX) {
          /* We're looking at a BCC now. Validate. */
          Serial.print(F("<< "));
          Serial.println(recvBuf);
          int res = din_66219_bcc(recvBuf);
          if (res < 0) {
            delay(200);
            iskra.print(F(S_NAK));
            Serial.print(F("bcc fail: "));
            Serial.println(res);
          } else {
            delay(200);
            iskra.print(F(S_ACK));
            recvBuf[recvBufPos - 2] = '\0'; /* drop ETX */

            switch (state) {
            case STATE_EXPECT_DATA_READOUT:
              on_data_readout(recvBuf + 1, recvBufPos - 3);
              nextState = STATE_UPGRADE;
              break;
            case STATE_EXPECT_PROGRAMMING_MODE:
              if (recvBufPos >= 6 &&
                  memcmp(recvBuf, (S_SOH "P0" S_STX "()"), 6) == 0) {
                nextState = STATE_REQUEST_1_8_0;
              } else {
                nextState = STATE_ENTER_PROGRAMMING_MODE;
              }
              break;
            case STATE_EXPECT_1_8_0:
              on_response(recvBuf + 1, recvBufPos - 3, state);
              nextState = STATE_REQUEST_2_8_0;
              break;
            case STATE_EXPECT_2_8_0:
              on_response(recvBuf + 1, recvBufPos - 3, state);
              nextState = STATE_PUSH;
              break;
            }
          }
          done = true;
          break;
        }
      }
      if (!done)
        trace_rx_buffer();
    }
    break;

  /* #5: Kill the connection with "<SOH>B0<ETX>" */
  case STATE_UPGRADE:
    delay(200);
    iskra.print(F(S_SOH "B0" S_ETX "q"));
    nextState = STATE_START2;
    break;

  /* Continuous: send "<SOH>R1<STX>1.8.0()<ETX>" for 1.8.0 register */
  case STATE_REQUEST_1_8_0:
  case STATE_REQUEST_2_8_0:
    Serial.println(F(">> <SOH>R1<STX>[12].8.0()<ETX>"));
    if (state == STATE_REQUEST_1_8_0) {
      delay(200);
      // For some reason, the first call to this always fails on the
      // ESP8266. After a NAK the retry works though.
      iskra.print(F(S_SOH "R1" S_STX "1.8.0()" S_ETX "Z"));
      nextState = STATE_EXPECT_1_8_0;
    } else {
      delay(200);
      // For some reason, the first call to this always fails on the
      // ESP8266. After a NAK the retry works though.
      iskra.print(F(S_SOH "R1" S_STX "2.8.0()" S_ETX "Y"));
      nextState = STATE_EXPECT_2_8_0;
    }
    break;

  /* Continuous: push data to remote */
  case STATE_PUSH:
    on_push();
    nextState = STATE_SLEEP;
    break;

  /* Continuous: sleep a while before data fetch */
  case STATE_SLEEP:
    /* We need to sleep a lot, or else the Watt guestimate makes no sense
     * for low Wh deltas. For 550W, we'll still only get 9.17 Wh per minute,
     * so we'd oscillate between 9 (540W) and 10 (600W). */
    // FIXME: can we get the device to poke us?
    // FIXME: how about using the Wh-pulse LED as a trigger...?
    delay(60000);
    nextState = STATE_REQUEST_1_8_0;
    break;

  default:
    break;
  }

  if (state != nextState || (
      state != STATE_SLEEP &&
      (millis() - lastStateChange) > 15000)) {

    if (state == nextState) {
      if (recvBufPos) {
        Serial.print(F("<< "));
        Serial.print(recvBuf);
        Serial.print(F(" (stale incoming buffer, size "));
        Serial.print(recvBufPos);
        Serial.println(F(")"));
      }
      Serial.println(F("state change timeout, resetting..."));
      nextState = STATE_START;
    }
    Serial.print(F("state: "));
    Serial.print(state);
    Serial.print(F(" -> "));
    Serial.println(nextState);
    state = nextState;
    recvBufPos = 0;
    lastStateChange = millis();
  }
}

void on_hello(const char *data, size_t end, State st)
{
  /* recvBuf = "ISK5ME162-0033" (ISKRA ME-162)
   * - uppercase 'K' means slow-ish (200ms (not 20ms) response times)
   * - suggest baud '5' (9600baud) */
  Serial.print(F("on_hello: "));
  Serial.println(data); // "ISK5ME162-0033" (without '/' and <CR><LF>)

  /* Store hello string in deviceHello */
  deviceHello[sizeof(deviceHello - 1)] = '\0';
  strncpy(deviceHello, data, sizeof(deviceHello) - 1);

  /* Check if we can upgrade the speed */
  if (end >= 3 && data[3] == '5') {
    // Send ACK, and change speed.
    if (st == STATE_EXPECT_HELLO) {
      nextState = STATE_ENTER_DATA_MODE;
    } else if (st == STATE_EXPECT_HELLO2) {
      nextState = STATE_ENTER_PROGRAMMING_MODE;
    }
  } else {
    /* If this is not a '5', we cannot upgrade to 9600 baud, and we cannot
     * enter programming mode to get values ourselves. */
    nextState = STATE_EXPECT_DATA_READOUT_SLOW;
  }
}

void on_data_readout(const char *data, size_t end)
{
  /* Data between <STX> and <ETX>. It should look like:
   * > C.1.0(28342193)        // Meter serial number
   * > 0.0.0(28342193)        // Device address
   * > 1.8.0(0032826.545*kWh) // Total positive active energy (A+)
   * > 1.8.1(0000000.000*kWh) // Positive active energy in first tariff (T1)
   * > 1.8.2(0032826.545*kWh) // Positive active energy in second tariff (T2)
   * > 2.8.0(0000000.001*kWh) // Total negative active energy (A-)
   * > 2.8.1(0000000.000*kWh) // Negative active energy in first tariff (T1)
   * > 2.8.2(0000000.001*kWh) // Negative active energy in second tariff (T2)
   * > F.F(0000000)           // Meter fatal error
   * > !                      // end-of-data
   * (With <CR><LF> everywhere.) */
  Serial.print(F("on_data_readout: ["));
  Serial.print(deviceHello);
  Serial.print(F("]: "));
  Serial.print(data);

  ensure_wifi();
  ensure_mqtt();
#ifdef HAVE_MQTT
  // Use simple application/x-www-form-urlencoded format, except for
  // the DATA bit (FIXME).
  mqttClient.beginMessage(mqtt_topic);
  mqttClient.print("device_id=");
  mqttClient.print(guid);
  mqttClient.print("&power_hello=");
  mqttClient.print(deviceHello);
  mqttClient.print("&DATA=");
  mqttClient.print(data); // FIXME: unformatted data..
  mqttClient.endMessage();
#endif
}

static void on_response(const char *data, size_t end, State st)
{
  /* (0032835.698*kWh) */
  Serial.print(F("on_response: ["));
  Serial.print(deviceHello);
  Serial.print(F(", "));
  Serial.print(st == STATE_EXPECT_1_8_0 ? "1.8.0" : "2.8.0");
  Serial.print(F("]: "));
  Serial.println(data);

  if (end == 17 && data[0] == '(' && data[8] == '.' &&
      memcmp(data + 12, "*kWh)", 5) == 0) {
    long curValue = atol(data + 1) * 1000 + atol(data + 9);
    int idx = (st == STATE_EXPECT_1_8_0 ? 0 : 1);

    /* > This number will overflow (go back to zero), after approximately
     * > 50 days.
     * So, we'll do it sooner, but make sure we know about it. */
    long curTime = (millis() & 0x3fffffffL);

    if (lastValue[idx] >= 0) {
      deltaValue[idx] = (curValue - lastValue[idx]);
      deltaTime[idx] = (curTime - lastTime[idx]);
      if (deltaValue[idx] < 0 || deltaTime[idx] < 0) {
        deltaValue[idx] = deltaTime[idx] = -1;
      }
    } else {
      deltaValue[idx] = deltaTime[idx] = -1;
    }
    lastValue[idx] = curValue;
    lastTime[idx] = curTime;
  }
}

void on_push()
{
  ensure_wifi();
  ensure_mqtt();

#ifdef HAVE_MQTT
  // Use simple application/x-www-form-urlencoded format.
  mqttClient.beginMessage(mqtt_topic);
  mqttClient.print("device_id=");
  mqttClient.print(guid);
#endif

  Serial.print("pushing device: ");
  Serial.println(deviceHello);
  for (int idx = 0; idx < 2; ++idx) {
    if (lastValue[idx] >= 0) {
      Serial.print("pushing value: ");
      Serial.println(lastValue[idx]);
#ifdef HAVE_MQTT
      // FIXME: we definitely need the "1.8.0" in here too
      mqttClient.print("&watthour[");
      mqttClient.print(idx);
      mqttClient.print("]=");
      mqttClient.print(lastValue[idx]);
#endif
    }
    if (deltaTime[idx] > 0) {
      // (Wh * 3600) == Ws; (Tms / 1000) == Ts; W == Ws / T
      float watt = (deltaValue[idx] * 3600.0) / (deltaTime[idx] / 1000.0);
      Serial.print("pushing watt: ");
      Serial.println(watt);
#ifdef HAVE_MQTT
      // FIXME: we definitely need the "1.8.0" in here too
      mqttClient.print("&watt[");
      mqttClient.print(idx);
      mqttClient.print("]=");
      mqttClient.print(watt);
#endif
    }
  }

  Serial.print(F("pushing uptime: "));
  Serial.println(millis());
#ifdef HAVE_MQTT
  mqttClient.print(F("&uptime="));
  mqttClient.print(millis());
  mqttClient.endMessage();
#endif
}

#ifdef HAVE_MQTT
/**
 * Check that Wifi is up, or connect when not connected.
 */
static void ensure_wifi() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(wifi_ssid, wifi_password);
    for (int i = 30; i >= 0; --i) {
      if (WiFi.status() == WL_CONNECTED) {
        break;
      }
      delay(1000);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("Wifi UP on \"");
      Serial.print(wifi_ssid);
      Serial.print("\", Local IP: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.print("Wifi NOT UP on \"");
      Serial.print(wifi_ssid);
      Serial.println("\".");
    }
  }
}
#endif // HAVE_MQTT

#ifdef HAVE_MQTT
/**
 * Check that the MQTT connection is up or connect if it isn't.
 */
static void ensure_mqtt()
{
  mqttClient.poll();
  if (!mqttClient.connected()) {
    if (mqttClient.connect(mqtt_broker, mqtt_port)) {
      Serial.print("MQTT connected: ");
      Serial.println(mqtt_broker);
    } else {
      Serial.print("MQTT connection to ");
      Serial.print(mqtt_broker);
      Serial.print(" failed! Error code = ");
      Serial.println(mqttClient.connectError());
    }
  }
}
#endif // HAVE_MQTT

/**
 * Calculate and (optionally) check block check character (BCC)
 *
 * IEC 62056-21 block check character (BCC):
 * - ISO/IEC 1155:1978 or DIN 66219: XOR of all values;
 * - ISO/IEC 1745:1975: start XOR after first SOH or STX.
 *
 * Return values:
 *  >=0  Calculated value
 *   -1  No checkable data
 *   -2  No ETX found
 *   -3  Bad BCC value
 */
static int din_66219_bcc(const char *s)
{
  const char *p = s;
  char bcc = 0;
  while (*p != '\0' && *p != C_SOH && *p != C_STX)
    ++p;
  if (*p == '\0')
    return -1; /* no checkable data */
  while (*++p != '\0' && *p != C_ETX)
    bcc ^= *p;
  if (*p == '\0')
    return -2; /* no end of transmission?? */
  bcc ^= *p;
  if (*++p != '\0' && bcc != *p)
    return -3; /* block check char was wrong */
  return bcc;
}

#ifdef TEST_BUILD
static void test_din_66219_bcc()
{
  char test1[] = (
    S_STX
    "C.1.0(28342193)\r\n"
    "0.0.0(28342193)\r\n"
    "1.8.0(0032826.545*kWh)\r\n"
    "1.8.1(0000000.000*kWh)\r\n"
    "1.8.2(0032826.545*kWh)\r\n"
    "2.8.0(0000000.001*kWh)\r\n"
    "2.8.1(0000000.000*kWh)\r\n"
    "2.8.2(0000000.001*kWh)\r\n"
    "F.F(0000000)\r\n"
    "!\r\n"
    S_ETX
    "L");
  char test2[] = S_SOH "B0" S_ETX "q";
  printf("[%d] %s\n", din_66219_bcc(test1), test1);
  printf("[%d] %s\n", din_66219_bcc(test2), test2);
}

int main()
{
  test_din_66219_bcc();
  on_hello("ISK5ME162-0033", 14, STATE_EXPECT_HELLO);
  on_response("(0032826.545*kWh)", 17, STATE_EXPECT_1_8_0);
  printf("%ld - %ld - %ld\n",
    lastValue[0], deltaValue[0], deltaTime[0]);
  on_response("(0032826.554*kWh)", 17, STATE_EXPECT_1_8_0);
  printf("%ld - %ld - %ld\n",
    lastValue[0], deltaValue[0], deltaTime[0]);
  on_push();
  return 0;
}

#endif

/* vim: set ts=8 sw=2 sts=2 et ai: */
