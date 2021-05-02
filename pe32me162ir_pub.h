#ifndef INCLUDED_PE32ME162IR_PUB_H
#define INCLUDED_PE32ME162IR_PUB_H

#include <Arduino.h> /* Serial, pinMode, INPUT, OUTPUT, ... */

/* Include files specific to the platform (ESP8266, Arduino or TEST) */
#if defined(ARDUINO_ARCH_ESP8266)
# include <SoftwareSerial.h>
# define HAVE_MQTT
# define HAVE_WIFI
#elif defined(ARDUINO_ARCH_AVR)
# include <CustomSoftwareSerial.h>
# define SoftwareSerial CustomSoftwareSerial
# define SWSERIAL_7E1 CSERIAL_7E1
#elif defined(TEST_BUILD)
# include <SoftwareSerial.h>
#else
# error Unsupported platform
#endif

/* Include files specific to Wifi/MQTT */
#ifdef HAVE_WIFI
# include <ESP8266WiFi.h>
# ifdef HAVE_MQTT
#  include <ArduinoMqttClient.h>
# endif
#endif

/* Helper for PROGMEM/flash strings. */
#include "progmem.h"  // possibly used in config.h

#include "WattGauge.h"

#include "config.h"

#endif //INCLUDED_PE32ME162IR_PUB_H
