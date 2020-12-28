/**
 * pe32me162ir_pub // Talk to optical port of ISKRA ME-162, export to MQTT
 *
 * Components:
 * - ISKRA ME-162 electronic meter with optical port
 * - (ESP8266MOD+Wifi)
 * - a digital IR-transceiver from https://wiki.hal9k.dk/projects/kamstrup
 *   (NOTE: be sure to flip/check the order of the two BC547 transistors!)
 * - attach PIN10<->RX, PIN9<->TX, 3VC<->VCC, GND<->GND
 *   (either 5V or 3.3V seems to be okay)
 *
 * Dependencies:
 * - (for ESP8266) board: package_esp8266com_index.json
 * - (for ESP8266) library: ArduinoMqttClient
 * - (for Arduino) library (manually): CustomSoftwareSerial (by ledongthuc)
 *
 * Configuration:
 * - FIXME
 *
 * TODO:
 * - Fix RX/TX ports;
 * - make it work with ESP;
 * - actually PUSH something instead of saying yea yea yea;
 * - clean up duplicate code/states with 1.8.0/2.8.0;
 * - can we get the device to poke us? / more granular Watts
 * - fix another low mem warning (probably goes when we remove lastState debug)
 */

/* Serial connection for debugging. Don't increase this, it seems to
 * interfere with the CustomSoftwareSerial. Haven't looked at why, but
 * the software serial starts communicating crap. */
const int BAUD = 9600;

/* Pin definitions:
 * - on the ISKRA ME-162, the RX on the LEFT;
 * - so our IR TX must be on the LEFT. */
const int PIN_IR_RX = 10; // connect to digital pin 10
const int PIN_IR_TX = 9;  // connect to digital pin 9
//const int PIN_LED = 13; // standard Arduino Led

#include <Arduino.h> /* Serial, pinMode, INPUT, OUTPUT, ... */

/* MQTT/Wifi notes:
 * - does not work with the Arduino Uno */

#if defined(ARDUINO_ARCH_ESP8266)
# include <ArduinoMqttClient.h>
# include <ESP8266WiFi.h>
# include <SoftwareSerial.h>
#elif defined(ARDUINO_ARCH_AVR)
# include <CustomSoftwareSerial.h>
# define SoftwareSerial CustomSoftwareSerial
# define SWSERIAL_7E1 CSERIAL_7E1
#elif defined(TEST_BUILD)
  const int SWSERIAL_7E1 = 0;
  class SoftwareSerial {
  public:
    SoftwareSerial(int, int, bool) {};
    void begin(long, unsigned short) {}
    int available() { return true; }
    void print(char const *) {}
    int read() { return 0; }
  };
# undef F
# define F(x) x
#else
# error Unsupported platform
#endif

/* In config.h, you should have:
const char wifi_ssid[] = "<ssid>";
const char wifi_password[] = "<password>";
const char mqtt_broker[] = "192.168.1.2";
const int  mqtt_port = 1883;
const char mqtt_topic[] = "some/topic";
*/
#include "config.h"

#define VERSION "v0"
//#define DEBUG

/* ISKRA ME-162 notes:
 * > The optical port complies with the IEC 62056-21 (IEC
 * > 61107) standard, a mode C protocol is employed;
 * > data transmission rate is 9600 bit/sec.
 * ...
 * > The optical port wavelength is 660 nm and luminous
 * > intensity is min. 1 mW/sr for the ON state.
 *
 * IEC 62056-21 mode C is in fact an ASCII protocol:
 * > C. [Starts with fixed rate (300 baud), bidirectional ASCII protocol ...
 * > may switch baud rate ... may enter programming mode and]
 * > allows manufacturer-specific [extensions].
 * [ github.com/lvzon/dsmr-p1-parser/blob/master/doc/IEC-62056-21-notes.md ] */

enum State {
  STATE_START = 0,
  STATE_EXPECT_HELLO,
  STATE_ENTER_DATA_MODE,
  STATE_EXPECT_DATA_READOUT,
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

/* We use the guid to store something unique to identify the device by.
 * For now, we'll populate it with the ESP8266 Wifi MAC address. */
static char guid[24]; // "EUI48:11:22:33:44:55:66"

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

/* We need a (Custom)SoftwareSerial because the Arduino Uno does not do
 * 300 baud. Once we get up to speed, we could use the HardwareSerial
 * instead. (But we don't, right now.)
 * - the Arduino Uno Hardware Serial does not support baud below 1200;
 * - communication starts with 300 baud;
 * - using SoftwareSerial allows for easy debug using HardwareSerial (USB);
 * - Arduino SoftwareSerial does only SERIAL_8N1 (8bit, no parity, 1 stop bit)
 *   (so we use CustomSoftwareSerial; for ESP8266 it's called SoftwareSerial)
 * - we require SERIAL_7E1 (7bit, even parity, 1 stop bit)
 * Supply RX pin, TX pin, inverted=false. Our device has HIGH == no TX
 * light == idle. */
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


void setup()
{
  Serial.begin(BAUD);
  while (!Serial)
    delay(0);

  //pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_IR_RX, INPUT);
  pinMode(PIN_IR_TX, OUTPUT);

  state = nextState = STATE_START;
  lastStateChange = millis();
  delay(1000);
  Serial.println("pe32iskra_pub: ISKRA ME-162 publisher");
}


void loop()
{
  switch (state) {
  /* FIXME/TODO: if no state switch for 60 seconds, go back to START */

  /* #1: At 300 baud, we send "/?!<CR><LF>" or "/?1!<CR><LF>" */
  case STATE_START:
  case STATE_START2:
    Serial.println(F(">> /?!<CR><LF>"));
    /* Communication starts at 300 baud, at 1+7+1+1=10 bits/septet. So, for
     * 30 septets/second, we can wait 33.3ms when there is nothing. */
    iskra.begin(300, SWSERIAL_7E1);
    iskra.print(F("/?!\r\n"));
    recvBufPos = 0;
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
          //Serial.println(F("<< skipping 0x7f"));
        } else {
          recvBuf[recvBufPos++] = c;
          recvBuf[recvBufPos] = '\0';
        }
        if (c == '\n' && recvBufPos >= 2 && recvBuf[recvBufPos - 2] == '\r') {
          recvBuf[recvBufPos - 2] = '\0'; /* drop <CR><LF> */
          on_hello(recvBuf + 1, recvBufPos - 3, state);
          recvBufPos = 0;
          done = true;
          break;
        }
      }
      if (done)
        break;
      if (recvBufPos) {
        Serial.print(F("<< "));
        Serial.print(recvBuf);
        Serial.println(F(" (cont)"));
      }
    }
    /* When there is no data, wait 30ms for another 10 bits. */
    delay(30);
    break;

  /* #3: We send an ACK with "speed 5" to switch to 9600 baud */
  case STATE_ENTER_DATA_MODE:
  case STATE_ENTER_PROGRAMMING_MODE:
    if (state == STATE_ENTER_DATA_MODE) {
      Serial.println(F(">> <ACK>050<CR><LF>"));
      iskra.print(F(S_ACK "050\r\n"));
    } else {
      Serial.println(F(">> <ACK>051<CR><LF>"));
      iskra.print(F(S_ACK "051\r\n"));
    }
    /* Assuming that the begin(NEW_SPEED) does not affect the
     * previous print() jobs: we can read/write immediately. */
    iskra.begin(9600, SWSERIAL_7E1);
    recvBufPos = 0;
    nextState = (state == STATE_ENTER_DATA_MODE
      ? STATE_EXPECT_DATA_READOUT : STATE_EXPECT_PROGRAMMING_MODE);
    break;

  /* #4: We expect "<STX>$EDIS_DATA<ETX>$BCC" with power info. */
  case STATE_EXPECT_DATA_READOUT:
  case STATE_EXPECT_PROGRAMMING_MODE: /* <SOH>P0<STX>()<ETX>$BCC */
  case STATE_EXPECT_1_8_0:            /* <STX>(0032835.698*kWh)<ETX>$BCC */
  case STATE_EXPECT_2_8_0:            /* <STX>(0000000.001*kWh)<ETX>$BCC */
    if (iskra.available()) {
      bool done = false;
      while (iskra.available() && recvBufPos < recvBufSize) {
        char c = iskra.read();
        /* We'll received some 0x7f's at the start. Ignore them. */
        if (recvBufPos == 0 && c == 0x7f) {
          //Serial.println(F("<< skipping 0x7f"));
        } else {
          recvBuf[recvBufPos++] = c;
          recvBuf[recvBufPos] = '\0';
        }
        if (recvBufPos >= 2 && recvBuf[recvBufPos - 2] == C_ETX) {
          /* We're looking at a BCC now. Validate. */
          Serial.print(F("<< "));
          Serial.println(recvBuf);
          int res = din_66219_bcc(recvBuf);
          if (res < 0) {
            iskra.print(F(S_NAK));
            Serial.print(F("bcc fail: "));
            Serial.println(res, DEC);
          } else {
            iskra.print(F(S_ACK));
            recvBuf[recvBufPos - 2] = '\0'; /* drop ETX */

            switch (state) {
            case STATE_EXPECT_DATA_READOUT:
              on_data_readout(recvBuf + 1, recvBufPos - 3);
              nextState = STATE_UPGRADE;
              break;
            case STATE_EXPECT_PROGRAMMING_MODE:
              nextState = STATE_REQUEST_1_8_0;
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
          recvBufPos = 0;
          done = true;
          break;
        }
      }
      if (!done && recvBufPos) {
        Serial.print(F("<< "));
        Serial.print(recvBuf);
        Serial.println(F(" (cont)"));
      }
    }
    break;

  /* #5: Kill the connection with "<SOH>B0<ETX>" */
  case STATE_UPGRADE:
    iskra.print(F(S_SOH "B0" S_ETX "q"));
    nextState = STATE_START2;
    break;

  /* Continuous: send "<SOH>R1<STX>1.8.0()<ETX>" for 1.8.0 register */
  case STATE_REQUEST_1_8_0:
  case STATE_REQUEST_2_8_0:
    Serial.println(F(">> <SOH>R1<STX>[12].8.0()<ETX>"));
    if (state == STATE_REQUEST_1_8_0) {
      iskra.print(F(S_SOH "R1" S_STX "1.8.0()" S_ETX "Z"));
      nextState = STATE_EXPECT_1_8_0;
    } else {
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
    delay(60000);
    nextState = STATE_REQUEST_1_8_0;
    break;

  default:
    break;
  }

  if (state != nextState) {
    Serial.print(F("state: "));
    Serial.print(state, DEC);
    Serial.print(F(" -> "));
    Serial.println(nextState, DEC);
    state = nextState;
    lastStateChange = millis();
  } else if (state != STATE_SLEEP &&
      (millis() - lastStateChange) > 15000) {
    Serial.print("millis:");
    Serial.print(millis(), DEC);
    Serial.print(", lastStateChange:");
    Serial.print(lastStateChange, DEC);
    Serial.print(", diff:");
    Serial.println((millis() - lastStateChange), DEC);
    nextState = STATE_START;
  }
}

void on_hello(const char *data, size_t end, State st)
{
  /* recvBuf = "ISK5ME162-0033" (ISKRA ME-162)
   * - uppercase 'K' means slow-ish
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
    // Skip speed change.
    // FIXME: this blows up, we'd never get to programming mode;
    // and only get the data readout over 300 baud.
    nextState = STATE_EXPECT_DATA_READOUT;
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
  // FIXME: send to somewhere, including the deviceHello!
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
  Serial.print("pushing device: ");
  Serial.println(deviceHello);
  for (int idx = 0; idx < 2; ++idx) {
    if (lastValue[idx] >= 0) {
      Serial.print("pushing value: ");
      Serial.println(lastValue[idx]);
    }
    if (deltaTime[idx] > 0) {
      // (Wh * 3600) == Ws; (Tms / 1000) == Ts; W == Ws / T
      float watt = (deltaValue[idx] * 3600.0) / (deltaTime[idx] / 1000.0);
      Serial.print("pushing watt: ");
      Serial.println(watt);
    }
  }
  Serial.print("pushing uptime: ");
  Serial.println(millis());
}

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

void delay(unsigned long) {}
unsigned long millis() { static int m = 50000; return (m += 60000); }
void pinMode(uint8_t pin, uint8_t mode) {}
size_t Print::print(char const *p) { printf("%s", p); return 0; }
size_t Print::print(int p, int) { printf("%d", p); return 0; }
size_t Print::print(long p, int) { printf("%ld", p); return 0; }
size_t Print::print(unsigned long p, int) { printf("%lu", p); return 0; }
size_t Print::println(char const *p) { printf("%s\n", p); return 0; }
size_t Print::println(double p, int) { printf("%f\n", p); return 0; }
size_t Print::println(int p, int) { printf("%d\n", p); return 0; }
size_t Print::println(long p, int) { printf("%ld\n", p); return 0; }
size_t Print::println(unsigned long p, int) { printf("%lu\n", p); return 0; }

HardwareSerial::HardwareSerial(
  volatile uint8_t *ubrrh, volatile uint8_t *ubrrl,
  volatile uint8_t *ucsra, volatile uint8_t *ucsrb,
  volatile uint8_t *ucsrc, volatile uint8_t *udr) :
    _ubrrh(ubrrh), _ubrrl(ubrrl),
    _ucsra(ucsra), _ucsrb(ucsrb), _ucsrc(ucsrc),
    _udr(udr),
    _rx_buffer_head(0), _rx_buffer_tail(0),
    _tx_buffer_head(0), _tx_buffer_tail(0) {}
void HardwareSerial::begin(unsigned long, uint8_t) {}
#endif

/* vim: set ts=8 sw=2 sts=2 et ai: */
