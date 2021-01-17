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
 * - clean up publish and on_data_readout debug
 * - add more frequent sampling so we can lose the LED
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
 * consumption is low. A pulse causes the sleep to be cut short,
 * increasing the possibility that two consecutive readings are "right
 * after a new watt hour value."
 * > In the meter mode it [...] blinks with a pulse rate of 1000 imp/kWh,
 * > the pulse's width is 40 ms. */
const int PULSE_THRESHOLD = 100;  // analog value between 0 and 1023

const int STATE_CHANGE_TIMEOUT = 15; // reset state after 15s of no change

/* In config.h, you should have:
const char wifi_ssid[] = "<ssid>";
const char wifi_password[] = "<password>";
const char mqtt_broker[] = "192.168.1.2";
const int  mqtt_port = 1883;
const char mqtt_topic[] = "some/topic";
*/
#include "config.h"

#include "WattGauge.h"

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
  STATE_WR_REQ_OBIS,
  STATE_RD_RESP_OBIS,

  STATE_PUBLISH,
  STATE_SLEEP,
  STATE_WAIT_FOR_PULSE
};

/* Subset of OBIS (or EDIS) codes from IEC 62056 provided by the ISKRA ME-162.
 * [A-B:]C.D.E[*F] where the ISKRA ME-162 does not do [A-B:] medium addressing.
 * Note that I removed the T1 and T2 types here, as in the Netherlands,
 * Ripple Control (TF-signal) will be completely disabled for non-"smart"
 * meters by July 2021. Switching between T1 and T2 will not be possible on
 * dumb meters: you'll be stuck using a single tariff anyway. */
enum Obis {
  OBIS_C_1_0 = 0, // Meter serial number
  OBIS_F_F_0,     // Fatal error meter status
  OBIS_0_9_1,     // Time (returns (hh:mm:ss))
  OBIS_0_9_2,     // Date (returns (YY.MM.DD))
  // We read these two in a loop, the rest are irrelevant to us.
  OBIS_1_8_0,     // Positive active energy (A+) total [Wh]
  OBIS_2_8_0,     // Negative active energy (A+) total [Wh]
#if 0
  OBIS_1_8_1,     // Positive active energy (A+) in tariff T1 [Wh]
  OBIS_1_8_2,     // Positive active energy (A+) in tariff T2 [Wh]
  OBIS_1_8_3,     // Positive active energy (A+) in tariff T3 [Wh]
  OBIS_1_8_4,     // Positive active energy (A+) in tariff T4 [Wh]
  OBIS_2_8_1,     // Negative active energy (A+) in tariff T1 [Wh]
  // [...]
  OBIS_15_8_0,    // Total absolute active energy (= 1_8_0 + 2_8_0)
  // [...]
#endif
  // alas: the ME-162 does not do "1.7.0" current Watt usage (returns (ERROR))
  OBIS_LAST
};

/* Keep in sync with the Obis enum! */
const char *const Obis_[] = {
  "C.1.0", "F.F", "0.9.1", "0.9.2", "1.8.0", "2.8.0",
#if 0
  "1.8.1", "1.8.2", "1.8.3", "1.8.4", "2.8.1", "15.8.0",
#endif
  "UNDEF"
};

struct obis_values_t {
  unsigned long values[OBIS_LAST];
};

/* C-escape, for improved serial monitor readability */
static const char *cescape(char *buffer, const char *p, size_t maxlen);
/* Calculate and (optionally) check block check character (BCC) */
static int din_66219_bcc(const char *s);
/* Convert string to Obis number or OBIS_LAST if not found */
static inline enum Obis str2Obis(const char *key, int keylen);
/* Convert Obis string to number */
inline const char *Obis2str(Obis obis) { return Obis_[obis]; }
/* Parse data readout buffer and populate obis_values_t */
static void parse_data_readout(struct obis_values_t *dst, const char *src);

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
static void on_response(const char *data, size_t end, Obis obis);

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

Obis nextObis;
WattGauge positive; /* feed it 1.8.0, get 1.7.0 */
WattGauge negative; /* feed it 2.8.0, get 2.7.0 */
long last_publish;


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

  // Initial values
  pulse_low = 1023;
  pulse_high = 0;
  state = nextState = STATE_WR_LOGIN;
  lastStateChange = last_publish = millis();
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
     * but that doesn't work on the ME-162. */
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
  case STATE_RD_RESP_OBIS:          /* \STX (0032835.698*kWh)\ETX $BCC */
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
  case STATE_WR_REQ_OBIS:
    writeState = state;
    {
      char buf[16];
      snprintf(buf, 15, S_SOH "R1" S_STX "%s()" S_ETX, Obis2str(nextObis));
      char bcc = din_66219_bcc(buf);
      int pos = strlen(buf);
      buf[pos] = bcc;
      buf[pos + 1] = '\0';
      iskra_tx(buf);
      nextState = STATE_RD_RESP_OBIS;
    }
    break;

  /* Continuous: publish data to remote */
  case STATE_PUBLISH:
    {
      long tdelta_ms = (millis() - last_publish);
      unsigned power_sum = positive.get_power() + negative.get_power(); /* Watt */
      unsigned significant_change = (
        positive.get_power_change_factor() <= 0.6 ||
        positive.get_power_change_factor() >= 1.6 ||
        negative.get_power_change_factor() <= 0.6 ||
        negative.get_power_change_factor() >= 1.6);
      /* DEBUG */
      Serial.print("current watt approximation: ");
      Serial.println(power_sum);
      /* Only push every 120s or more often when there are significant
       * changes. */
      if (tdelta_ms >= 120000 ||
            (tdelta_ms >= 60000 && power_sum >= 400) ||
            (tdelta_ms >= 25000 && significant_change)) {
        /* DEBUG */
        if (significant_change) {
          Serial.print("significant change: ");
          Serial.print(positive.get_power_change_factor());
          Serial.print(" or ");
          Serial.println(negative.get_power_change_factor());
        }
        publish();
        positive.reset();
        negative.reset();
        pulse_low = 1023;
        pulse_high = 0;
        last_publish = millis();
      }
    }
    nextState = STATE_SLEEP;
    break;

  /* Continuous: just sleep a slight bit */
  /* FIXME: this is not necessary: the pulse-wait will sleep 1000 or X+1000 */
  case STATE_SLEEP:
#ifdef HAVE_MQTT
    /* We don't publish faster than every 60s. So'll we'll need the
     * mqttClient (and Wifi) to stay up. Calling this continuously
     * doesn't hurt. */
    mqttClient.poll();
#endif
    delay(200);
    nextState = STATE_WAIT_FOR_PULSE;
    break;

  /* Continuous: just sleep a slight bit */
  case STATE_WAIT_FOR_PULSE:
    /* This is a mashup between simply doing the poll-for-new-totals
     * every second, and only-a-poll-after-pulse:
     * - polling every second or so gives us decent, but not awesome, averages
     * - polling after a pulse gives us great averages
     * But, the pulse may not work as we want once we have both positive
     * and negative power counts. Also, it's nice if the device works
     * without the extra pulse receiver.
     * But, as long as a pulse receiver is available, it'll increase the
     * accuracy of the averages. */
    {
      /* Pulse or not, after one second it's time. */
      bool have_waited_a_second = (millis() - lastStateChange) >= 1000;
      short val = analogRead(A0);
      /* Debug information, sent over MQTT. */
      if (val < pulse_low) {
        pulse_low = val;
      } else if (val > pulse_high) {
        pulse_high = val;
      }
      if (val >= PULSE_THRESHOLD || have_waited_a_second) {
        if (!have_waited_a_second) {
          /* Sleep cut short, for better average calculations. */
          Serial.print("pulse: Got value ");
          Serial.println(val);
          /* Add delay. It appears that after a Wh pulse, the meter takes at
           * most 1000ms to update the Wh counter. Without this delay, we'd
           * usually get the Wh count of the previous second, except
           * sometimes. That effect caused seemingly random high and then
           * low spikes in the Watt averages. */
          delay(1000);
        }
        nextObis = OBIS_1_8_0;
        nextState = STATE_WR_REQ_OBIS;
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
      nextObis = OBIS_1_8_0;
      return STATE_WR_REQ_OBIS;
    }
    return STATE_WR_PROG_MODE;

  case STATE_RD_RESP_OBIS:
    on_response(data + 1, pos - 3, nextObis);
    nextObis = (Obis)((int)nextObis + 1);
    if (nextObis < OBIS_LAST) {
      return STATE_WR_REQ_OBIS;
    }
    return STATE_PUBLISH;

  default:
    /* shouldn't get here.. */
    break;
  }

  return st;
}

void on_data_readout(const char *data, size_t end)
{
  struct obis_values_t vals;
  long t = (millis() & 0x7fffffffL); /* make sure t is positive */

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
  parse_data_readout(&vals, data);
  /* Ooh. The first samples are in! */
  positive.set_watthour(t, vals.values[OBIS_1_8_0]);
  negative.set_watthour(t, vals.values[OBIS_2_8_0]);

  /* Keep this for debugging mostly. Bonus points if we also add current
   * time 0.9.x */
  Serial.print(F("on_data_readout: ["));
  Serial.print(identification);
  Serial.print(F("]: "));
  Serial.print(data);

  ensure_wifi();
  ensure_mqtt();
#ifdef HAVE_MQTT
  // Use simple application/x-www-form-urlencoded format, except for
  // the DATA bit (FIXME).
  // FIXME: NOTE: This is limited to 256 chars in MqttClient.cpp
  // (TX_PAYLOAD_BUFFER_SIZE).
  mqttClient.beginMessage(mqtt_topic);
  mqttClient.print("device_id=");
  mqttClient.print(guid);
  mqttClient.print("&id=");
  mqttClient.print(identification);
  mqttClient.print("&DATA=");
  mqttClient.print(data); // FIXME: unformatted data..
  mqttClient.endMessage();
#endif
}

static void on_response(const char *data, size_t end, Obis obis)
{
  /* (0032835.698*kWh) */
  Serial.print(F("on_response: ["));
  Serial.print(identification);
  Serial.print(F(", "));
  Serial.print(Obis2str(obis));
  Serial.print(F("]: "));
  Serial.println(data);

  if ((obis == OBIS_1_8_0 || obis == OBIS_2_8_0) && (
        end == 17 && data[0] == '(' && data[8] == '.' &&
      memcmp(data + 12, "*kWh)", 5) == 0)) {
    long t = (millis() & 0x7fffffffL); /* make sure t is positive */
    long watthour = atol(data + 1) * 1000 + atol(data + 9);

    if (obis == OBIS_1_8_0) {
      positive.set_watthour(t, watthour);
    } else if (obis == OBIS_2_8_0) {
      negative.set_watthour(t, watthour);
    }
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
  Serial.print("pushing 1.8.0 (Wh) ");
  Serial.print(positive.get_energy_total());
  Serial.print(" and 1.7.0 (Watt) ");
  Serial.println(positive.get_power());
  Serial.print("pushing 2.8.0 (Wh) ");
  Serial.print(negative.get_energy_total());
  Serial.print(" and 2.7.0 (Watt) ");
  Serial.println(negative.get_power());

#ifdef HAVE_MQTT
  // FIXME: we definitely need the "1.8.0" in here too
  mqttClient.print("&watthour[0]=");
  mqttClient.print(positive.get_energy_total());
  mqttClient.print("&watt[0]=");
  mqttClient.print(positive.get_power());
  mqttClient.print("&watthour[1]=");
  mqttClient.print(negative.get_energy_total());
  mqttClient.print("&watt[1]=");
  mqttClient.print(negative.get_power());
#endif

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

/**
 * Convert string to Obis enum or OBIS_LAST if not found
 *
 * Example: str2Obis("1.8.0", 5) == OBIS_1_8_0
 * Example: Obis2str[OBIS_1_8_0] == "1.8.0"
 */
static inline Obis str2Obis(const char *key, int keylen)
{
  for (int i = 0; i < OBIS_LAST; ++i) {
    if (memcmp(key, Obis_[i], keylen) == 0)
      return (Obis)i;
  }
  return OBIS_LAST;
}

/**
 * Parse data readout buffer and populate obis_values_t
 */
static void parse_data_readout(struct obis_values_t *dst, const char *src)
{
  memset(dst, 0, sizeof(*dst));
  while (*src != '\0') {
    int len = 0;
    const char *key = src;
    while (*src != '\0' && *src != '(')
      ++src;
    if (*src == '\0')
      break;
    len = (src++ - key);

    int i = str2Obis(key, len);
    if (i < OBIS_LAST) {
      const char *value = src;
      while (*src != '\0' && *src != ')')
        ++src;
      if (*src == '\0')
        break;
      len = (src++ - value);

      long lval = atol(value);
      /* "0032826.545*kWh" */
      if (len == 15 && value[7] == '.' && memcmp(value + 11, "*kWh", 4) == 0) {
        lval = lval * 1000 + atol(value + 8);
      }
      dst->values[i] = lval;
    }
    while (*src != '\0' && *src++ != '\r')
      ;
    if (*src++ != '\n')
      break;
  }
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

static void test_obis()
{
  STR_EQ("Obis2str", Obis2str(OBIS_C_1_0), "C.1.0");
  STR_EQ("Obis2str", Obis2str(OBIS_1_8_0), "1.8.0");
  STR_EQ("Obis2str", Obis2str(OBIS_2_8_0), "2.8.0");
  INT_EQ("str2Obis", str2Obis("C.1.0", 5), OBIS_C_1_0);
  INT_EQ("str2Obis", str2Obis("1.8.0", 5), OBIS_1_8_0);
  INT_EQ("str2Obis", str2Obis("2.8.0", 5), OBIS_2_8_0);
  printf("\n");
}

static void test_data_readout_to_obis()
{
  struct obis_values_t vals;
  parse_data_readout(&vals, (
    "C.1.0(28342193)\r\n"
    "0.0.0(28342193)\r\n"
    "1.8.0(0032826.545*kWh)\r\n"
    "1.8.1(0000000.000*kWh)\r\n"
    "1.8.2(0032826.545*kWh)\r\n"
    "3.8.2(bogus_value_xxx)\r\n" /* ignore */
    "2.8.0(0000000.001*kWh)\r\n"
    "2.8.1(0000000.000*kWh)\r\n"
    "2.8.2(0000000.001*kWh)\r\n"
    "F.F(0000000)\r\n!\r\n")); /* the "!\r\n" is also optional */
  INT_EQ("parse_data_readout", vals.values[OBIS_C_1_0], 28342193);
  INT_EQ("parse_data_readout", vals.values[OBIS_F_F_0], 0);
  INT_EQ("parse_data_readout", vals.values[OBIS_1_8_0], 32826545);
  INT_EQ("parse_data_readout", vals.values[OBIS_2_8_0], 1);
#if 0
  INT_EQ("parse_data_readout", vals.values[OBIS_1_8_1], 0);
  INT_EQ("parse_data_readout", vals.values[OBIS_1_8_2], 32826545);
  INT_EQ("parse_data_readout", vals.values[OBIS_2_8_1], 0);
  INT_EQ("parse_data_readout", vals.values[OBIS_2_8_2], 1);
#endif
  printf("\n");
}

int main()
{
  test_cescape();
  test_din_66219_bcc();
  test_obis();
  test_data_readout_to_obis();
  test_wattgauge();

  on_hello("ISK5ME162-0033", 14, STATE_RD_IDENTIFICATION);
//  on_response("(0032826.545*kWh)", 17, OBIS_1_8_0);
//  printf("%ld - %ld - %ld\n",
//    lastValue[OBIS_1_8_0], deltaValue[OBIS_1_8_0], deltaTime[OBIS_1_8_0]);
//  on_response("(0032826.554*kWh)", 17, OBIS_1_8_0);
//  printf("%ld - %ld - %ld\n",
//    lastValue[OBIS_1_8_0], deltaValue[OBIS_1_8_0], deltaTime[OBIS_1_8_0]);
  publish();
  return 0;
}

#endif

/* vim: set ts=8 sw=2 sts=2 et ai: */
