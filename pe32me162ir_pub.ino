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
 * - (optional: analog light sensor to attach to A0<->SIG (and 3VC and GND))
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
 * - Copy config.h.example to config.h and adapt it to your situation and
 *   preferences.
 * - Copy arduino_secrets.h.example to arduino_secrets.h and fill in your
 *   credentials and MQTT information.
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
 * - clean up debug
 * - add/standardize first MQTT push (with data_readout and date and time)
 */
#include "pe32me162ir_pub.h"

#define VERSION "v3~pre3"

/* On the ESP8266, the baud rate needs to be sufficiently high so it
 * doesn't affect the SoftwareSerial. (Probably because this Serial is
 * blocking/serial? 9600 is too low.)
 * On the Arduino Uno, the baud rate is free to choose. Just make sure
 * you don't try to cram large values into a 16-bits int. */
static const long SERMON_BAUD = 115200; // serial monitor for debugging

#if defined(ARDUINO_ARCH_ESP8266)
static const int PIN_IR_RX = 5;  // D1 / GPIO5
static const int PIN_IR_TX = 4;  // D2 / GPIO4
#else /*defined(ARDUINO_ARCH_AVR)*/
static const int PIN_IR_RX = 9;  // digital pin 9
static const int PIN_IR_TX = 10; // digital pin 10
#endif

DECLARE_PGM_CHAR_P(wifi_ssid, SECRET_WIFI_SSID);
DECLARE_PGM_CHAR_P(wifi_password, SECRET_WIFI_PASS);
DECLARE_PGM_CHAR_P(mqtt_broker, SECRET_MQTT_BROKER);
static const int mqtt_port = SECRET_MQTT_PORT;
DECLARE_PGM_CHAR_P(mqtt_topic, SECRET_MQTT_TOPIC);

#ifdef OPTIONAL_LIGHT_SENSOR
static const int PULSE_THRESHOLD = 100;  // analog value between 0 and 1023
#endif //OPTIONAL_LIGHT_SENSOR

static const int STATE_CHANGE_TIMEOUT = 15; // reset state after 15s of no change

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

  STATE_MAYBE_PUBLISH,
  STATE_SLEEP
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
  // Available in ME-162, but not that useful to us:
  OBIS_1_8_1,     // Positive active energy (A+) in tariff T1 [Wh]
  OBIS_1_8_2,     // Positive active energy (A+) in tariff T2 [Wh]
  OBIS_1_8_3,     // Positive active energy (A+) in tariff T3 [Wh]
  OBIS_1_8_4,     // Positive active energy (A+) in tariff T4 [Wh]
  OBIS_2_8_1,     // Negative active energy (A+) in tariff T1 [Wh]
  // [...]
  OBIS_15_8_0,    // Total absolute active energy (= 1_8_0 + 2_8_0)
#endif
  // Alas, not in ME-162 (returns (ERROR) when queried):
  //OBIS_1_7_0,   // Positive active instantaneous power (A+) [W]
  //OBIS_2_7_0,   // Negative active instantaneous power (A+) [W]
  //OBIS_16_7_0,  // Sum active instantaneous power [W] (= 1_7_0 - 2_7_0)
  //OBIS_16_8_0,  // Sum of active energy without blockade (= 1_8_0 - 2_8_0)
  OBIS_LAST
};

/* Trick to allow defining an array of PROGMEM strings. */
typedef struct { char pgm_str[7]; } obis_pgm_t;
/* Keep in sync with the Obis enum! */
const obis_pgm_t Obis_[] PROGMEM = {
  {"C.1.0"}, {"F.F"}, {"0.9.1"}, {"0.9.2"}, {"1.8.0"}, {"2.8.0"},
#if 0
  {"1.8.1"}, {"1.8.2"}, {"1.8.3"}, {"1.8.4"}, {"2.8.1"}, {"15.8.0"},
#endif
  {"UNDEF"}
};

struct obis_values_t {
  unsigned long values[OBIS_LAST];
};

/* C-escape, for improved serial monitor readability */
static const char *cescape(
    char *buffer, const char *p, size_t maxlen, bool progmem = false);
static inline const pgm_char *cescape(
    char *buffer, const pgm_char *p, size_t maxlen) {
  return to_pgm_char_p(cescape(buffer, from_pgm_char_p(p), maxlen, true));
}
/* Calculate and (optionally) check block check character (BCC) */
static int din_66219_bcc(const char *s);
/* Convert string to Obis number or OBIS_LAST if not found */
static inline enum Obis str2Obis(const char *key, int keylen);
/* Convert Obis string to number */
inline const pgm_char *Obis2str(Obis obis) {
  return to_pgm_char_p(Obis_[obis].pgm_str);
}
/* Parse data readout buffer and populate obis_values_t */
static void parse_data_readout(struct obis_values_t *dst, const char *src);

#ifdef HAVE_MQTT /* and HAVE_WIFI */
static void ensure_wifi();
static void ensure_mqtt();
#else
static inline void ensure_wifi() {} /* noop */
static inline void ensure_mqtt() {} /* noop */
#endif

/* Helpers */
template<class T> static inline void iskra_tx(const T *p);
template<class T> static inline void serial_print_cescape(const T *p);
static inline void trace_rx_buffer();
/* Helper to add a little type safety to memcmp. */
static inline int memcmp_cstr(const char *s1, const char *s2, size_t len) {
  return memcmp(s1, s2, len);
}
/* Neat trick to let us do multiple Serial.print() using the << operator:
 * Serial << x << " " << y << LF; */
template<class T> inline Print &operator << (Print &obj, T arg) {
  obj.print(arg);
  return obj;
};

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

const char C_ENDL = '\n';
#define    S_ENDL   "\n"

/* We use the guid to store something unique to identify the device by.
 * For now, we'll populate it with the ESP8266 Wifi MAC address,
 * if available. */
static char guid[24] = "<no_wifi_found>"; // "EUI48:11:22:33:44:55:66"

#ifdef HAVE_WIFI
# ifdef MQTT_TLS
WiFiClientSecure wifiClient;
# else
WiFiClient wifiClient;
# endif
# ifdef HAVE_MQTT
MqttClient mqttClient(wifiClient);
# endif
#endif

#ifdef MQTT_TLS
static const uint8_t mqtt_fingerprint[20] PROGMEM = SECRET_MQTT_FINGERPRINT;
#endif

#ifdef MQTT_AUTH
# ifndef MQTT_TLS
#  error MQTT_AUTH requires MQTT_TLS
# else
DECLARE_PGM_CHAR_P(mqtt_user, SECRET_MQTT_USER);
DECLARE_PGM_CHAR_P(mqtt_pass, SECRET_MQTT_PASS);
# endif
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

/* Current state, scheduled state, current "write" state for retries */
State state, next_state, write_state;
unsigned long last_statechange;

/* Storage for incoming data. If the data readout is larger than this size
 * bytes, then the rest of the code won't cope. (The observed data is at most
 * 200 octets long, so this should be sufficient.) */
size_t buffer_pos;
const int buffer_size = 800;
char buffer_data[buffer_size + 1];

/* IEC 62056-21 6.3.2 + 6.3.14:
 * 3chars + 1char-baud + (optional) + 16char-ident */
char identification[32];

#ifdef OPTIONAL_LIGHT_SENSOR
/* Record low and high pulse values so we can debug/monitor the light
 * sensor values from the MQTT data. */
short pulse_low = 1023;
short pulse_high = 0;
#endif //OPTIONAL_LIGHT_SENSOR

Obis next_obis;
EnergyGauge gauge; /* feed it 1.8.0 and 2.8.0, get 1.7.0 and 2.7.0 */
unsigned long last_publish;


void setup()
{
  Serial.begin(SERMON_BAUD);
  while (!Serial)
    delay(0);

#ifdef HAVE_WIFI
  strncpy(guid, "EUI48:", 6);
  strncpy(guid + 6, WiFi.macAddress().c_str(), sizeof(guid) - (6 + 1));
# ifdef MQTT_TLS
  wifiClient.setFingerprint(mqtt_fingerprint);
# endif
# ifdef MQTT_AUTH
  mqttClient.setUsernamePassword(mqtt_user, mqtt_pass);
# endif
#endif

  pinMode(PIN_IR_RX, INPUT);
  pinMode(PIN_IR_TX, OUTPUT);

  // Welcome message
  delay(200); /* tiny sleep to avoid dupe log after double restart */
  Serial << F("Booted pe32me162ir_pub " VERSION " guid ") << guid << C_ENDL;

  // Initial connect (if available)
  ensure_wifi();
  ensure_mqtt();

  // Send termination command, in case we were already connected and
  // in 9600 baud previously.
  iskra.begin(9600, SWSERIAL_7E1);
  iskra_tx(F(S_SOH "B0" S_ETX "q"));

  // Initial values
  state = next_state = STATE_WR_LOGIN;
  last_statechange = last_publish = millis();
}

void loop()
{
  switch (state) {

  /* #1: At 300 baud, we send "/?!\r\n" or "/?1!\r\n" */
  case STATE_WR_LOGIN:
  case STATE_WR_LOGIN2:
    write_state = state;
    /* Communication starts at 300 baud, at 1+7+1+1=10 bits/septet. So, for
     * 30 septets/second, we could wait 33.3ms when there is nothing. */
    iskra.begin(300, SWSERIAL_7E1);
    iskra_tx(F("/?!\r\n"));
    next_state = (state == STATE_WR_LOGIN
      ? STATE_RD_IDENTIFICATION : STATE_RD_IDENTIFICATION2);
    break;

  /* #2: We receive "/ISK5ME162-0033\r\n" */
  case STATE_RD_IDENTIFICATION:
  case STATE_RD_IDENTIFICATION2:
    if (iskra.available()) {
      while (iskra.available() && buffer_pos < buffer_size) {
        char ch = iskra.read();
        if (0) {
#if defined(ARDUINO_ARCH_AVR)
          /* On the Arduino Uno, we tend to three of these after sending
           * STATE_WR_LOGIN (at 300 baud), before reception. */
        } else if (buffer_pos == 0 && ch == 0x7f) {
          Serial << F("<< (skipping 0x7f)" S_ENDL); // only observed on Arduino
#endif
        } else if (ch == '\0') {
          Serial << F("<< (unexpected NUL, ignoring)" S_ENDL);
        } else {
          buffer_data[buffer_pos++] = ch;
          buffer_data[buffer_pos] = '\0';
        }
        if (ch == '\n' && buffer_pos >= 2 &&
            buffer_data[buffer_pos - 2] == '\r') {
          buffer_data[buffer_pos - 2] = '\0'; /* drop "\r\n" */
          next_state = on_hello(buffer_data + 1, buffer_pos - 3, state);
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
    write_state = state;
    /* ACK V Z Y:
     *   V = protocol control (0=normal, 1=2ndary, ...)
     *   Z = 0=NAK or 'ISK5ME162'[3] for ACK speed change (9600 for ME-162)
     *   Y = mode control (0=readout, 1=programming, 2=binary)
     * "\ACK 001\r\n" should NAK speed, but go into programming mode,
     * but that doesn't work on the ME-162. */
    if (state == STATE_WR_REQ_DATA_MODE) {
      iskra_tx(F(S_ACK "050\r\n")); // 050 = 9600baud + data readout mode
      next_state = STATE_RD_DATA_READOUT;
    } else {
      iskra_tx(F(S_ACK "051\r\n")); // 051 = 9600baud + programming mode
      next_state = STATE_RD_PROG_MODE_ACK;
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
        if (0) {
#if defined(ARDUINO_ARCH_AVR)
          /* On the Arduino Uno, we tend to six of these after sending
           * STATE_WR_REQ_OBIS (at 9600 baud), before reception. */
        } else if (buffer_pos == 0 && ch == 0x7f) {
          Serial << F("<< (skipping 0x7f)" S_ENDL); // only observed on Arduino
#endif
        } else if (ch == '\0') {
          Serial << F("<< (unexpected NUL, ignoring)" S_ENDL);
        } else {
          buffer_data[buffer_pos++] = ch;
          buffer_data[buffer_pos] = '\0';
        }
        if (ch == C_NAK) {
          Serial << F("<< ");
          serial_print_cescape(buffer_data);
          next_state = write_state;
          buffer_pos = 0;
          break;
        }
        if (buffer_pos >= 2 && buffer_data[buffer_pos - 2] == C_ETX) {
          /* If the last non-BCC token is EOT, we should send an ACK
           * to get the rest. But seeing that message ends with ETX, we
           * should not ACK. */
          Serial << F("<< ");
          serial_print_cescape(buffer_data);

          /* We're looking at a BCC now. Validate. */
          int res = din_66219_bcc(buffer_data);
          if (res < 0) {
            Serial << F("bcc fail: ") << res << C_ENDL;
            /* Hope for a restransmit. Reset buffer. */
            buffer_pos = 0;
            break;
          }

          /* Valid BCC. Call appropriate handlers and switch state. */
          next_state = on_data_block_or_data_set(
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
    write_state = state;
    iskra_tx(F(S_SOH "B0" S_ETX "q"));
    next_state = STATE_WR_LOGIN2;
    break;

  /* Continuous: send "\SOH R1\STX 1.8.0()\ETX " for 1.8.0 register */
  case STATE_WR_REQ_OBIS:
    write_state = state;
    {
      char buf[16];
#if !defined(TEST_BUILD)
      /* Type safety is not available for the *_P functions.. Probably
       * because this is C-compatible. So, we'll use PSTR() instead of F().
       * PSTR() also puts the string in PROGMEM, but does not cast to the
       * __FlashStringHelper. */
      snprintf_P(buf, 15, PSTR(S_SOH "R1" S_STX "%S()" S_ETX),
          Obis2str(next_obis));
#else
      snprintf(buf, 15, (S_SOH "R1" S_STX "%s()" S_ETX),
          from_pgm_char_p(Obis2str(next_obis)));
#endif
      char bcc = din_66219_bcc(buf);
      int pos = strlen(buf);
      buf[pos] = bcc;
      buf[pos + 1] = '\0';
      iskra_tx(buf);
      next_state = STATE_RD_RESP_OBIS;
    }
    break;

  /* Continuous: maybe publish data to remote */
  case STATE_MAYBE_PUBLISH:
#ifdef HAVE_MQTT
    /* We don't necessarily publish every 60s, but we _do_ need to keep
     * the MQTT connection alive. poll() is safe to call often. */
    mqttClient.poll();
#endif
    {
      int tdelta_s = (millis() - last_publish) / 1000;
      int power = gauge.get_instantaneous_power();

      /* DEBUG */
      Serial << F("time to publish? ") << power <<
          F(" Watt, ") << tdelta_s << F(" seconds");
      if (gauge.has_significant_change())
        Serial << F(", has significant change");
      Serial << C_ENDL;

      /* Only push every 120s or more often when there are significant
       * changes. */
      if (tdelta_s >= 120 ||
            /* power is higher than 400: then we have more detail */
            (tdelta_s >= 60 && !(-400 < power && power < 400)) ||
            (tdelta_s >= 25 && gauge.has_significant_change())) {
        publish();
        gauge.reset();
#ifdef OPTIONAL_LIGHT_SENSOR
        pulse_low = 1023;
        pulse_high = 0;
#endif
        last_publish = millis();
      }
    }
    next_state = STATE_SLEEP;
    break;

  /* Continuous: just sleep a slight bit */
  case STATE_SLEEP:
#ifdef OPTIONAL_LIGHT_SENSOR
    /* This is a mashup between simply doing the poll-for-new-totals
     * every second, and only-a-poll-after-pulse:
     * - polling every second or so gives us decent, but not awesome, averages
     * - polling after a pulse gives us great averages
     * But, the pulse may not work properly once we have both positive
     * and negative power counts. Also, if we can do without the extra
     * photo transistor, it makes installation simpler.
     * (But, while it is available, it might increase the accuracy of
     * the averages. Needs confirmation!) */
    {
      /* Pulse or not, after one second it's time. */
      bool have_waited_a_second = (millis() - last_statechange) >= 1200;
      short val = analogRead(A0);
      /* Debug information, sent over MQTT. */
      pulse_low = min(pulse_low, val);
      pulse_high = max(pulse_high, val);
      if (val >= PULSE_THRESHOLD || have_waited_a_second) {
        if (!have_waited_a_second) {
          /* Sleep cut short, for better average calculations. */
          Serial << F("pulse: Got value ") << val << C_ENDL;
          /* Add delay. It appears that after a Wh pulse, the meter takes at
           * most 1000ms to update the Wh counter. Without this delay, we'd
           * usually get the Wh count of the previous second, except
           * sometimes. That effect caused seemingly random high and then
           * low spikes in the Watt averages. */
          delay(1000);
        }
        next_obis = OBIS_1_8_0;
        next_state = STATE_WR_REQ_OBIS;
      }
    }
#else //!OPTIONAL_LIGHT_SENSOR
    /* Wait 1.2s and then schedule a new request. */
    if ((millis() - last_statechange) >= 1200) {
      next_obis = OBIS_1_8_0;
      next_state = STATE_WR_REQ_OBIS;
    }
#endif //!OPTIONAL_LIGHT_SENSOR
    break;
  }

  /* Always check for state change timeout */
  if (state == next_state &&
      (millis() - last_statechange) > (STATE_CHANGE_TIMEOUT * 1000)) {
    if (buffer_pos) {
      Serial << F("<< (stale buffer sized ") << buffer_pos << F(") ");
      serial_print_cescape(buffer_data);
    }
    /* Note that after having been connected, it may take up to a minute
     * before a new connection can be established. So we may end up here
     * a few times before reconnecting for real. */
    Serial << F("timeout: State change took to long, resetting..." S_ENDL);
    next_state = STATE_WR_LOGIN;
  }

  /* Handle state change */
  if (state != next_state) {
    Serial << F("state: ") << state << F(" -> ") << next_state << C_ENDL;
    state = next_state;
    buffer_pos = 0;
    last_statechange = millis();
  }
}

State on_hello(const char *data, size_t end, State st)
{
  /* buffer_data = "ISK5ME162-0033" (ISKRA ME-162) (no "/" or "\r\n")
   * - uppercase 'K' means slow-ish (200ms (not 20ms) response times)
   * - (for protocol mode C) suggest baud '5'
   *   (0=300, 1=600, 2=1200, 3=2400, 4=4800, 5=9600, 6=19200) */
  Serial << F("on_hello: ") << data << C_ENDL;

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
    if (pos >= 6 && memcmp_P(data, F(S_SOH "P0" S_STX "()"), 6) == 0) {
      next_obis = OBIS_1_8_0;
      return STATE_WR_REQ_OBIS;
    }
    return STATE_WR_PROG_MODE;

  case STATE_RD_RESP_OBIS:
    on_response(data + 1, pos - 3, next_obis);
    next_obis = (Obis)((int)next_obis + 1);
    if (next_obis < OBIS_LAST) {
      return STATE_WR_REQ_OBIS;
    }
    return STATE_MAYBE_PUBLISH;

  default:
    /* shouldn't get here.. */
    break;
  }

  return st;
}

void on_data_readout(const char *data, size_t /*end*/)
{
  struct obis_values_t vals;
  unsigned long t = millis();

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
  gauge.set_positive_active_energy_total(t, vals.values[OBIS_1_8_0]);
  gauge.set_negative_active_energy_total(t, vals.values[OBIS_2_8_0]);

  /* Keep this for debugging mostly. Bonus points if we also add current
   * time 0.9.x */
  Serial << F("on_data_readout: [") << identification << F("]: ") <<
    data << C_ENDL;

  ensure_wifi();
  ensure_mqtt();
#ifdef HAVE_MQTT
  // Use simple application/x-www-form-urlencoded format, except for
  // the DATA bit (FIXME).
  // FIXME: NOTE: This is limited to 256 chars in MqttClient.cpp
  // (TX_PAYLOAD_BUFFER_SIZE).
  // NOTE: We use String(mqtt_topic).c_str()) so you can use either
  // PROGMEM or SRAM strings.
  mqttClient.beginMessage(String(mqtt_topic).c_str());
  mqttClient.print(F("device_id="));
  mqttClient.print(guid);
  // FIXME: move identification to another message; the one where we
  // also add 0.9.1 and 0.9.2
  mqttClient.print(F("&id="));
  mqttClient.print(identification);
  mqttClient.print(F("&DATA="));
  // FIXME: replace CRLF in data with ", ". replace "&" with ";"
  mqttClient.print(data); // FIXME: unformatted data..
  mqttClient.endMessage();
#endif //HAVE_MQTT
}

static void on_response(const char *data, size_t end, Obis obis)
{
  /* (0032835.698*kWh) */
  Serial << F("on_response[") << Obis2str(obis) << F("]: ") <<
    data << C_ENDL;

  if ((obis == OBIS_1_8_0 || obis == OBIS_2_8_0) && (
        end == 17 && data[0] == '(' && data[8] == '.' &&
        memcmp_P(data + 12, F("*kWh)"), 5) == 0)) {
    unsigned long t = millis();
    long watthour = atol(data + 1) * 1000 + atol(data + 9);

    if (obis == OBIS_1_8_0) {
      gauge.set_positive_active_energy_total(t, watthour);
    } else if (obis == OBIS_2_8_0) {
      gauge.set_negative_active_energy_total(t, watthour);
    }
  }
}

/**
 * Publish the latest data.
 *
 * Map:
 * - 1.8.0 = e_pos_act_energy_wh = Positive active energy [Wh]
 * - 2.8.0 = e_neg_act_energy_wh = Negative active energy [Wh]
 * - 1.7.0 = e_pos_inst_power_w = Positive active instantaneous power [Watt]
 * - 2.7.0 = e_neg_inst_power_w = Negative active instantaneous power [Watt]
 */
void publish()
{
  ensure_wifi();
  ensure_mqtt();

  Serial <<
    F("pushing: [1.8.0] ") << gauge.get_positive_active_energy_total() <<
    F(" Wh, [2.8.0] ") << gauge.get_negative_active_energy_total() <<
    F(" Wh, [16.7.0] ") << gauge.get_instantaneous_power() <<
    F(" Watt" S_ENDL);

#ifdef HAVE_MQTT
  // Use simple application/x-www-form-urlencoded format.
  // NOTE: We use String(mqtt_topic).c_str()) so you can use either
  // PROGMEM or SRAM strings.
  mqttClient.beginMessage(String(mqtt_topic).c_str());
  mqttClient.print(F("device_id="));
  mqttClient.print(guid);
  mqttClient.print(F("&e_pos_act_energy_wh="));
  mqttClient.print(gauge.get_positive_active_energy_total());
  mqttClient.print(F("&e_neg_act_energy_wh="));
  mqttClient.print(gauge.get_negative_active_energy_total());
  mqttClient.print(F("&e_inst_power_w="));
  mqttClient.print(gauge.get_instantaneous_power());
  mqttClient.print(F("&dbg_uptime="));
  mqttClient.print(millis());
#ifdef OPTIONAL_LIGHT_SENSOR
  mqttClient.print(F("&dbg_pulse="));
  mqttClient.print(pulse_low);
  mqttClient.print(F(".."));
  mqttClient.print(pulse_high);
#endif //OPTIONAL_LIGHT_SENSOR
  mqttClient.endMessage();
#endif //HAVE_MQTT
}

template<class T> static inline void serial_print_cescape(const T *p)
{
  char buf[200]; /* watch out, large local variable! */
  const T *restart = p;
  do {
    restart = cescape(buf, restart, 200);
    Serial << buf;
  } while (restart != NULL);
  Serial << C_ENDL;
}

template<class T> static inline void iskra_tx(const T *p)
{
  /* According to spec, the time between the reception of a message
   * and the transmission of an answer is: between 200ms (or 20ms) and
   * 1500ms. So adding an appropriate delay(200) before send should be
   * sufficient. */
  delay(20); /* on my local ME-162, delay(20) is sufficient */

  /* Delay before debug print; makes more sense in monitor logs. */
  Serial << F(">> ");
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
  /* On the Arduino Uno, we will see this kind of receive buildup,
   * but only during the 300 baud connect handshake.
   * 13:43:46.625 -> << / (cont)
   * 13:43:46.658 -> << /I (cont)
   * 13:43:46.692 -> << /IS (cont)
   * 13:43:46.725 -> << /ISK (cont)
   * ... */
  if (buffer_pos) {
    /* No cescape() this time, for performance reasons. */
    Serial << F("<< ") << buffer_data << F(" (cont)" S_ENDL);
  }
#endif
}

#ifdef HAVE_MQTT /* and HAVE_WIFI */
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
      Serial << F("Wifi UP on \"") << wifi_ssid << F("\", Local IP: ") <<
        WiFi.localIP() << C_ENDL;
    } else {
      Serial << F("Wifi NOT UP on \"") << wifi_ssid << F("\"." S_ENDL);
    }
  }
}

/**
 * Check that the MQTT connection is up or connect if it isn't.
 */
static void ensure_mqtt()
{
  mqttClient.poll();
  if (!mqttClient.connected()) {
    // NOTE: We use String(mqtt_broker).c_str()) so you can use either
    // PROGMEM or SRAM strings.
    if (mqttClient.connect(String(mqtt_broker).c_str(), mqtt_port)) {
      Serial << F("MQTT connected: ") << mqtt_broker << C_ENDL;
    } else {
      Serial << F("MQTT connection to ") << mqtt_broker <<
        F(" failed! Error code = ") << mqttClient.connectError() << C_ENDL;
    }
  }
}
#endif //HAVE_MQTT

/**
 * C-escape, for improved serial monitor readability
 *
 * Returns non-NULL to resume if we stopped because of truncation.
 */
static const char *cescape(
    char *buffer, const char *p, size_t maxlen, bool progmem)
{
  char ch = '\0';
  char *d = buffer;
  const char *de = d + maxlen - 5;
  while (d < de) {
    if (progmem) {
      ch = pgm_read_byte(p);
    } else {
      ch = *p;
    }
    if (ch == '\0') {
      break;
    }
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
  return (ch == '\0') ? NULL : p;
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
    if (memcmp_P(key, to_pgm_char_p(Obis_[i].pgm_str), keylen) == 0)
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
      if (len == 15 && value[7] == '.' &&
          memcmp_P(value + 11, F("*kWh"), 4) == 0) {
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
        func, got, expected);
    return 0;
  }
}

static int FSTR_EQ(
    const char *func, const pgm_char *fgot, const char *expected)
{
#ifndef TEST_BUILD
# error "We treat all pointers equal: don't do this on a microcontroller"
#endif
  return STR_EQ(func, from_pgm_char_p(fgot), expected);
}

static int INT_EQ(const char *func, int got, int expected)
{
  if (expected == got) {
    printf("OK (%s): %d\n", func, expected);
    return 1;
  } else {
    printf("FAIL (%s): %d != %d\n", func, got, expected);
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
  FSTR_EQ("Obis2str", Obis2str(OBIS_C_1_0), "C.1.0");
  FSTR_EQ("Obis2str", Obis2str(OBIS_1_8_0), "1.8.0");
  FSTR_EQ("Obis2str", Obis2str(OBIS_2_8_0), "2.8.0");
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
#endif //TEST_BUILD

/* vim: set ts=8 sw=2 sts=2 et ai: */
