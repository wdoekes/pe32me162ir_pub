/**
 * pe32me162ir_pub // Talk to optical port of ISKRA ME-162, export to MQTT
 *
 * Components:
 * - ISKRA ME-162 electronic meter with optical port
 * - a digital IR-transceiver from https://wiki.hal9k.dk/projects/kamstrup --
 *   BEWARE: be sure to check the direction of the two BC547 transistors.
 *   The pictures on the wiki have them backwards. Check the
 *   https://wiki.hal9k.dk/_media/projects/kamstrup-schem-v2.pdf !
 * - ESP8266 (NodeMCU, with Wifi) _or_ an Arduino (Uno?). Wifi/MQTT publish
 *   support is only(!) available for the ESP8266 at the moment.
 * - attach PIN_IR_RX<->RX, PIN_IR_TX<->TX, 3VC<->VCC (or 5VC), GND<->GND
 * - optionally, an analog light sensor to attach to A0<->SIG (and 3VC and GND)
 *
 * Building/dependencies:
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
 * - See: github.com/lvzon/dsmr-p1-parser/blob/master/doc/IEC-62056-21-notes.md
 *
 * TODO:
 * - clean up duplicate code/states with 1.8.0/2.8.0;
 * - clean up publish and on_data_readout debug
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

/* Optionally, you may attach a light sensor diode (or photo transistor
 * or whatever) to analog pin A0 and have it monitor the red watt hour
 * pulse LED. This improves the current Watt calculation when the power
 * consumption is low. A pulse causes the sleep 60s to be cut short,
 * increasing the possibility that two consecutive readings are "right
 * after a new watt hour value."
 * > In the meter mode it [...] blinks with a pulse rate of 1000 imp/kWh,
 * > the pulse's width is 40 ms. */
const int PULSE_THRESHOLD = 100;  // analog value between 0 and 1023
/* By leaving the interval between 30s and 60s, we can have 30s in
 * which to watch for a pulse LED: 30s/Wh-pulse == 120Wh-pulse/hour == 120Watt
 * I.e. for usage as low as 120Watt, we'll get a pulse in time.
 * And, if no pulse LED is connected, you'll simply get values every 60s.
 * Do note that the ISKRA ME-162 appears to time out slightly after 60s.
 * After that time it will not respond to our programming mode request, and
 * we'd (state change) timeout and go back to start.
 * (One advantage of the 30s, is that we get more granular Watt usage
 * during peak times.) */
const int PUBLISH_INTERVAL_MIN = 30; // wait at least 30s before publish
const int PUBLISH_INTERVAL_MAX = 60; // wait at most 60s before publish
const int STATE_CHANGE_TIMEOUT = 15; // reset state after 15s of no change

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
#endif

#define VERSION "v0"


enum State {
  STATE_WR_LOGIN = 0,
  STATE_RD_IDENTIFICATION,
  STATE_WR_REQ_DATA_MODE, /* data (readout) mode */
  STATE_RD_DATA_READOUT,
  STATE_RD_DATA_READOUT_SLOW,
  STATE_WR_RESTART,

  STATE_WR_LOGIN2,
  STATE_RD_IDENTIFICATION2,
  STATE_WR_PROG_MODE, /* programming mode */
  STATE_RD_PROG_MODE_ACK,
  STATE_WR_REQ_1_8_0,
  STATE_RD_VAL_1_8_0,
  STATE_WR_REQ_2_8_0,
  STATE_RD_VAL_2_8_0,

  STATE_PUBLISH,
  STATE_SLEEP,
  STATE_WAIT_FOR_PULSE
};

/* Calculate and (optionally) check block check character (BCC) */
static int din_66219_bcc(const char *s);
/* C-escape, for improved serial monitor readability */
static const char *cescape(char *buffer, const char *p, size_t maxlen);

#ifdef HAVE_MQTT
static void ensure_wifi();
static void ensure_mqtt();
#else
static inline void ensure_wifi() {} /* noop */
static inline void ensure_mqtt() {} /* noop */
#endif

/* Helpers */
static inline void iskra_tx(const char *p);
static inline void serial_print_cescape(const char *p);
static inline void trace_rx_buffer();

/* Events */
static State on_data_block_or_data_set(char *data, size_t pos, State st);
static State on_hello(const char *data, size_t end, State st);
static void on_data_readout(const char *data, size_t end);
static void on_response(const char *data, size_t end, State st);

static void publish();

/* ASCII control codes */
const char C_SOH = '\x01';
#define    S_SOH   "\x01"
const char C_STX = '\x02';
#define    S_STX   "\x02"
const char C_ETX = '\x03';
#define    S_ETX   "\x03"
const char C_ACK = '\x06';
#define    S_ACK   "\x06"
const char C_NAK = '\x15';
#define    S_NAK   "\x15"

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

State state, nextState, writeState;
unsigned long lastStateChange;

/* Storage for incoming data. If the data readout is larger than this size
 * bytes, then the rest of the code won't cope. (The observed data is at most
 * 200 octets long, so this should be sufficient.) */
size_t buffer_pos;
const int buffer_size = 800;
char buffer_data[buffer_size + 1];

/* IEC 62056-21 6.3.2 + 6.3.14:
 * 3chars + 1char-baud + (optional) + 16char-ident */
char identification[32];

/* Record low and high pulse values so we can debug/monitor the light
 * sensor values from the MQTT data. */
short pulse_low = 1023;
short pulse_high = 0;

long deltaValue[2] = {-1, -1};
long deltaTime[2] = {-1, -1};
long lastValue[2] = {-1, -1};
long lastTime[2] = {-1, -1};


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
  delay(200); /* tiny sleep to avoid dupe log after double restart */
  Serial.print(F("Booted pe32me162ir_pub " VERSION " guid "));
  Serial.println(guid);

  // Initial connect (if available)
  ensure_wifi();
  ensure_mqtt();

  // Send termination command, in case we were already connected and
  // in 9600 baud previously.
  iskra.begin(9600, SWSERIAL_7E1);
  iskra_tx(S_SOH "B0" S_ETX "q");

  state = nextState = STATE_WR_LOGIN;
  lastStateChange = millis();
}

void loop()
{
  switch (state) {

  /* #1: At 300 baud, we send "/?!\r\n" or "/?1!\r\n" */
  case STATE_WR_LOGIN:
  case STATE_WR_LOGIN2:
    writeState = state;
    /* Communication starts at 300 baud, at 1+7+1+1=10 bits/septet. So, for
     * 30 septets/second, we could wait 33.3ms when there is nothing. */
    iskra.begin(300, SWSERIAL_7E1);
    iskra_tx("/?!\r\n");
    nextState = (state == STATE_WR_LOGIN
      ? STATE_RD_IDENTIFICATION : STATE_RD_IDENTIFICATION2);
    break;

  /* #2: We receive "/ISK5ME162-0033\r\n" */
  case STATE_RD_IDENTIFICATION:
  case STATE_RD_IDENTIFICATION2:
    if (iskra.available()) {
      while (iskra.available() && buffer_pos < buffer_size) {
        char ch = iskra.read();
        /* We've received (at least three) 0x7f's at the start. Ignore
         * all of them, as we won't expect them in the hello anyway. */
        if (buffer_pos == 0 && ch == 0x7f) {
#if defined(ARDUINO_ARCH_AVR)
          Serial.println(F("<< (skipping 0x7f)")); // only observed on Arduino
#endif
        } else if (ch == '\0') {
          Serial.println(F("<< (unexpected NUL, ignoring"));
        } else {
          buffer_data[buffer_pos++] = ch;
          buffer_data[buffer_pos] = '\0';
        }
        if (ch == '\n' && buffer_pos >= 2 &&
            buffer_data[buffer_pos - 2] == '\r') {
          buffer_data[buffer_pos - 2] = '\0'; /* drop "\r\n" */
          nextState = on_hello(buffer_data + 1, buffer_pos - 3, state);
          buffer_pos = 0;
          break;
        }
      }
      trace_rx_buffer();
    }
    /* When there is no data, we could wait 30ms for another 10 bits.
     * But we've seen odd thing happen. Busy-loop instead. */
    break;

  /* #3: We send an ACK with "speed 5" to switch to 9600 baud */
  case STATE_WR_REQ_DATA_MODE:
  case STATE_WR_PROG_MODE:
    writeState = state;
    /* ACK V Z Y:
     *   V = protocol control (0=normal, 1=2ndary, ...)
     *   Z = 0=NAK or 'ISK5ME162'[3] for ACK speed change (9600 for ME-162)
     *   Y = mode control (0=readout, 1=programming, 2=binary)
     * "\ACK 001\r\n" should NAK speed, but go into programming mode,
     * but that doesn't work on the ISKRA. */
    if (state == STATE_WR_REQ_DATA_MODE) {
      iskra_tx(S_ACK "050\r\n"); // 050 = 9600baud + data readout mode
      nextState = STATE_RD_DATA_READOUT;
    } else {
      iskra_tx(S_ACK "051\r\n"); // 051 = 9600baud + programming mode
      nextState = STATE_RD_PROG_MODE_ACK;
    }
    /* We're assuming here that the speed change does not affect the
     * previously written characters. It shouldn't if they're written
     * synchronously. */
    iskra.begin(9600, SWSERIAL_7E1);
    break;

  /* #4: We expect "\STX $EDIS_DATA\ETX $BCC" with power info. */
  case STATE_RD_DATA_READOUT:
  case STATE_RD_DATA_READOUT_SLOW:
  case STATE_RD_PROG_MODE_ACK:      /* \SOH P0\STX ()\ETX $BCC */
  case STATE_RD_VAL_1_8_0:          /* \STX (0032835.698*kWh)\ETX $BCC */
  case STATE_RD_VAL_2_8_0:          /* \STX (0000000.001*kWh)\ETX $BCC */
    if (iskra.available()) {
      while (iskra.available() && buffer_pos < buffer_size) {
        char ch = iskra.read();
        /* We'll receive some 0x7f's at the start. Ignore them. */
        if (buffer_pos == 0 && ch == 0x7f) {
#if defined(ARDUINO_ARCH_AVR)
          Serial.println(F("<< (skipping 0x7f)")); // only observed on Arduino
#endif
        } else if (ch == '\0') {
          Serial.println(F("<< (unexpected NUL, ignoring)"));
        } else {
          buffer_data[buffer_pos++] = ch;
          buffer_data[buffer_pos] = '\0';
        }
        if (ch == C_NAK) {
          Serial.print(F("<< "));
          serial_print_cescape(buffer_data);
          nextState = writeState;
          buffer_pos = 0;
          break;
        }
        if (buffer_pos >= 2 && buffer_data[buffer_pos - 2] == C_ETX) {
          /* If the last non-BCC token is EOT, we should send an ACK
           * to get the rest. But seeing that message ends with ETX, we
           * should not ACK. */
          Serial.print(F("<< "));
          serial_print_cescape(buffer_data);

          /* We're looking at a BCC now. Validate. */
          int res = din_66219_bcc(buffer_data);
          if (res < 0) {
            Serial.print(F("bcc fail: "));
            Serial.println(res);
            /* Hope for a restransmit. Reset buffer. */
            buffer_pos = 0;
            break;
          }

          /* Valid BCC. Call appropriate handlers and switch state. */
          nextState = on_data_block_or_data_set(
            buffer_data, buffer_pos, state);
          buffer_pos = 0;
          break;
        }
      }
      trace_rx_buffer();
    }
    break;

  /* #5: Terminate the connection with "\SOH B0\ETX " */
  case STATE_WR_RESTART:
    writeState = state;
    iskra_tx(S_SOH "B0" S_ETX "q");
    nextState = STATE_WR_LOGIN2;
    break;

  /* Continuous: send "\SOH R1\STX 1.8.0()\ETX " for 1.8.0 register */
  case STATE_WR_REQ_1_8_0:
  case STATE_WR_REQ_2_8_0:
    writeState = state;

    if (state == STATE_WR_REQ_1_8_0) {
      iskra_tx(S_SOH "R1" S_STX "1.8.0()" S_ETX "Z");
      nextState = STATE_RD_VAL_1_8_0;
    } else {
      iskra_tx(S_SOH "R1" S_STX "2.8.0()" S_ETX "Y");
      nextState = STATE_RD_VAL_2_8_0;
    }
    break;

  /* Continuous: publish data to remote */
  case STATE_PUBLISH:
    publish();
    nextState = STATE_SLEEP;
    break;

  /* Continuous: sleep PUBLISH_INTERVAL_MIN before data fetch */
  case STATE_SLEEP:
    /* We need to sleep a lot, or else the Watt guestimate makes no sense
     * for low Wh deltas. For 550W, we'll still only get 9.17 Wh per minute,
     * so we'd oscillate between 9 (540W) and 10 (600W).
     * However: if you monitor the Watt hour pulse LED, we can reduce
     * the oscillations. */
    delay(PUBLISH_INTERVAL_MIN * 1000);
    pulse_low = 1023;
    pulse_high = 0;
    nextState = STATE_WAIT_FOR_PULSE;
    break;

  /* Continuous: listen for pulse up to PUBLISH_INTERVAL_MAX seconds */
  case STATE_WAIT_FOR_PULSE:
    {
      const int max_wait = (
        PUBLISH_INTERVAL_MAX - PUBLISH_INTERVAL_MIN) * 1000;
      bool have_waited_max_time = (millis() - lastStateChange) >= max_wait;
      short val = analogRead(A0);
      if (val < pulse_low) {
        pulse_low = val;
      } else if (val > pulse_high) {
        pulse_high = val;
      }
      if (val >= PULSE_THRESHOLD) {
        /* Sleep cut short, for better average calculations. */
        Serial.print("pulse: Got value ");
        Serial.println(val);
        /* Add delay. It appears that after a Wh pulse, the meter takes at
         * most 1000ms to update the Wh counter. Without this delay, we'd
         * usually get the Wh count of the previous second, except
         * sometimes. That effect caused seemingly random high and then
         * low spikes in the Watt averages. */
        delay(1000);
        nextState = STATE_WR_REQ_1_8_0;
      } else if (have_waited_max_time) {
        /* Timeout waiting for pulse; no problem. */
        nextState = STATE_WR_REQ_1_8_0;
      }
    }
    break;
  }

  /* Always check for state change timeout */
  if (state == nextState &&
      state != STATE_SLEEP &&
      state != STATE_WAIT_FOR_PULSE &&
      (millis() - lastStateChange) > (STATE_CHANGE_TIMEOUT * 1000)) {
    if (buffer_pos) {
      Serial.print(F("<< (stale buffer sized "));
      Serial.print(buffer_pos);
      Serial.print(") ");
      serial_print_cescape(buffer_data);
    }
    /* Note that after having been connected, it may take up to a minute
     * before a new connection can be established. So we may end up here
     * a few times before reconnecting for real. */
    Serial.println(F("timeout: State change took to long, resetting..."));
    nextState = STATE_WR_LOGIN;
  }

  /* Handle state change */
  if (state != nextState) {
    Serial.print(F("state: "));
    Serial.print(state);
    Serial.print(F(" -> "));
    Serial.println(nextState);
    state = nextState;
    buffer_pos = 0;
    lastStateChange = millis();
  }
}

State on_hello(const char *data, size_t end, State st)
{
  /* buffer_data = "ISK5ME162-0033" (ISKRA ME-162)
   * - uppercase 'K' means slow-ish (200ms (not 20ms) response times)
   * - (for protocol mode C) suggest baud '5'
   *   (0=300, 1=600, 2=1200, 3=2400, 4=4800, 5=9600, 6=19200) */
  Serial.print(F("on_hello: "));
  Serial.println(data); // "ISK5ME162-0033" (without '/' nor "\r\n")

  /* Store identification string */
  identification[sizeof(identification - 1)] = '\0';
  strncpy(identification, data, sizeof(identification) - 1);

  /* Check if we can upgrade the speed */
  if (end >= 3 && data[3] == '5') {
    // Send ACK, and change speed.
    if (st == STATE_RD_IDENTIFICATION) {
      return STATE_WR_REQ_DATA_MODE;
    } else if (st == STATE_RD_IDENTIFICATION2) {
      return STATE_WR_PROG_MODE;
    }
  }
  /* If it was not a '5', we cannot upgrade to 9600 baud, and we cannot
   * enter programming mode to get values ourselves. */
  return STATE_RD_DATA_READOUT_SLOW;
}

State on_data_block_or_data_set(char *data, size_t pos, State st)
{
  data[pos - 2] = '\0'; /* drop ETX */

  switch (st) {
  case STATE_RD_DATA_READOUT:
    on_data_readout(data + 1, pos - 3);
    return STATE_WR_RESTART;

  case STATE_RD_PROG_MODE_ACK:
    if (pos >= 6 && memcmp(data, (S_SOH "P0" S_STX "()"), 6) == 0) {
      return STATE_WR_REQ_1_8_0;
    }
    return STATE_WR_PROG_MODE;

  case STATE_RD_VAL_1_8_0:
    on_response(data + 1, pos - 3, st);
    return STATE_WR_REQ_2_8_0;

  case STATE_RD_VAL_2_8_0:
    on_response(data + 1, pos - 3, st);
    return STATE_PUBLISH;

  default:
    /* shouldn't get here.. */
    break;
  }

  return st;
}

void on_data_readout(const char *data, size_t end)
{
  /* Data between STX and ETX. It should look like:
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
   * (With "\r\n" everywhere.) */
  Serial.print(F("on_data_readout: ["));
  Serial.print(identification);
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
  mqttClient.print(identification);
  mqttClient.print("&DATA=");
  mqttClient.print(data); // FIXME: unformatted data..
  mqttClient.endMessage();
#endif
}

static void on_response(const char *data, size_t end, State st)
{
  /* (0032835.698*kWh) */
  Serial.print(F("on_response: ["));
  Serial.print(identification);
  Serial.print(F(", "));
  Serial.print(st == STATE_RD_VAL_1_8_0 ? "1.8.0" : "2.8.0");
  Serial.print(F("]: "));
  Serial.println(data);

  if (end == 17 && data[0] == '(' && data[8] == '.' &&
      memcmp(data + 12, "*kWh)", 5) == 0) {
    long curValue = atol(data + 1) * 1000 + atol(data + 9);
    int idx = (st == STATE_RD_VAL_1_8_0 ? 0 : 1);

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

void publish()
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
  Serial.println(identification);
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
  mqttClient.print(F("&pulse_low="));
  mqttClient.print(pulse_low);
  mqttClient.print(F("&pulse_high="));
  mqttClient.print(pulse_high);
  mqttClient.endMessage();
#endif
}

static inline void serial_print_cescape(const char *p)
{
  char buf[200]; /* watch out, large local variable! */
  const char *restart = p;
  do {
    restart = cescape(buf, restart, 200);
    Serial.print(buf);
  } while (restart != NULL);
  Serial.println();
}

static inline void iskra_tx(const char *p)
{
  /* According to spec, the time between the reception of a message
   * and the transmission of an answer is: between 200ms (or 20ms) and
   * 1500ms. So adding an appropriate delay(200) before send should be
   * sufficient. */
  delay(20); /* on my local ME-162, delay(20) is sufficient */

  /* Delay before debug print; makes more sense in monitor logs. */
  Serial.print(F(">> "));
  serial_print_cescape(p);

  iskra.print(p);
}

static inline void trace_rx_buffer()
{
#if defined(ARDUINO_ARCH_ESP8266)
  /* On the ESP8266, the SoftwareSerial.available() never returns true
   * consecutive times: that means that we'd end up doing the trace()
   * for _every_ received character.
   * And when we have (slow) 9600 baud on the debug-Serial, this messes
   * up communication with the IR-Serial: some echoes appeared in a
   * longer receive buffer).
   * Solution: no tracing. */
#else
  if (buffer_pos) {
    Serial.print(F("<< "));
    Serial.print(buffer_data); // no cescape here for speed
    Serial.println(F(" (cont)"));
  }
#endif
}

#ifdef HAVE_MQTT
/**
 * Check that Wifi is up, or connect when not connected.
 */
static void ensure_wifi()
{
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
 * C-escape, for improved serial monitor readability
 *
 * Returns non-NULL to resume if we stopped because of truncation.
 */
static const char *cescape(char *buffer, const char *p, size_t maxlen)
{
  char ch;
  char *d = buffer;
  const char *de = d + maxlen - 5;
  while (d < de && (ch = *p) != '\0') {
    if (ch < 0x20 || ch == '\\' || ch == '\x7f') {
      d[0] = '\\';
      d[4] = ' ';
      switch (ch) {
#if 1
      // Extension
      case C_SOH: d[1] = 'S'; d[2] = 'O'; d[3] = 'H'; d += 4; break; // SOH
      case C_STX: d[1] = 'S'; d[2] = 'T'; d[3] = 'X'; d += 4; break; // STX
      case C_ETX: d[1] = 'E'; d[2] = 'T'; d[3] = 'X'; d += 4; break; // ETX
      case C_ACK: d[1] = 'A'; d[2] = 'C'; d[3] = 'K'; d += 4; break; // ACK
      case C_NAK: d[1] = 'N'; d[2] = 'A'; d[3] = 'K'; d += 4; break; // NAK
#endif
      // Regular backslash escapes
      case '\0': d[1] = '0'; d += 1; break;   // 0x00 (unreachable atm)
      case '\a': d[1] = 'a'; d += 1; break;   // 0x07
      case '\b': d[1] = 'b'; d += 1; break;   // 0x08
      case '\t': d[1] = 't'; d += 1; break;   // 0x09
      case '\n': d[1] = 'n'; d += 1; break;   // 0x0a
      case '\v': d[1] = 'v'; d += 1; break;   // 0x0b
      case '\f': d[1] = 'f'; d += 1; break;   // 0x0c
      case '\r': d[1] = 'r'; d += 1; break;   // 0x0d
      case '\\': d[1] = '\\'; d += 1; break;  // 0x5c
      // The rest in (backslash escaped) octal
      default:
        d[1] = '0' + (ch >> 6);
        d[2] = '0' + ((ch & 0x3f) >> 3);
        d[3] = '0' + (ch & 7);
        d += 3;
        break;
      }
    } else {
      *d = ch;
    }
    ++p;
    ++d;
  }
  *d = '\0';
  return (*p == '\0') ? NULL : p;
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
static int STR_EQ(const char *func, const char *got, const char *expected)
{
  if (strcmp(expected, got) == 0) {
    printf("OK (%s): \"\"\"%s\"\"\"\n", func, expected);
    return 1;
  } else {
    printf("FAIL (%s): \"\"\"%s\"\"\" != \"\"\"%s\"\"\"\n",
        func, expected, got);
    return 0;
  }
}

static int INT_EQ(const char *func, int got, int expected)
{
  if (expected == got) {
    printf("OK (%s): %d\n", func, expected);
    return 1;
  } else {
    printf("FAIL (%s): %d != %d\n", func, expected, got);
    return 0;
  }
}

static void test_cescape()
{
  char buf[512];
  const char *pos = buf;

  pos = cescape(buf, "a\x01", 6);
  STR_EQ("cescape", buf, "a");
  pos = cescape(buf, pos, 6); /* continue */
  STR_EQ("cescape", buf, "\\SOH ");

  pos = cescape(buf, "a\x01", 7);
  STR_EQ("cescape", buf, "a\\SOH ");

  pos = cescape(buf, "\001X\002ABC\\DEF\r\n\003", 512);
  STR_EQ("cescape", buf, "\\SOH X\\STX ABC\\\\DEF\\r\\n\\ETX ");

  printf("\n");
}

static void test_din_66219_bcc()
{
  INT_EQ("din_66219_bcc", din_66219_bcc("void"), -1);
  INT_EQ("din_66219_bcc", din_66219_bcc(S_STX "no_etx"), -2);
  INT_EQ("din_66219_bcc", din_66219_bcc(S_STX "!" S_ETX), '"');
  INT_EQ("din_66219_bcc", din_66219_bcc(S_STX "!" S_ETX "\""), '"');
  /* This sample data readout is 199 characters, including NUL. */
  INT_EQ("din_66219_bcc", din_66219_bcc(
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
    "L"), 'L');
  INT_EQ("din_66219_bcc", din_66219_bcc(S_SOH "B0" S_ETX "q"), 'q');
  printf("\n");
}

int main()
{
  test_cescape();
  test_din_66219_bcc();
  on_hello("ISK5ME162-0033", 14, STATE_RD_IDENTIFICATION);
  on_response("(0032826.545*kWh)", 17, STATE_RD_VAL_1_8_0);
  printf("%ld - %ld - %ld\n",
    lastValue[0], deltaValue[0], deltaTime[0]);
  on_response("(0032826.554*kWh)", 17, STATE_RD_VAL_1_8_0);
  printf("%ld - %ld - %ld\n",
    lastValue[0], deltaValue[0], deltaTime[0]);
  publish();
  return 0;
}

#endif

/* vim: set ts=8 sw=2 sts=2 et ai: */
