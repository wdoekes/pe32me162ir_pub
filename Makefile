# Use a symlink from pe32me162ir_pub.ino to pe32me162ir_pub.cc (and not
# to the more common .cpp) because g++ and make will correctly guess
# their type, while the Arduino IDE does not open the .cpp file as well
# (it already has this file open as the ino file).
OBJECTS = pe32me162ir_pub.o

# --- Arduino Uno AVR (8-bit RISC, by Atmel) ---
# /snap/arduino/current/hardware/arduino/avr/boards.txt:
# uno.build.mcu=atmega328p
# uno.build.f_cpu=16000000L
# uno.build.board=AVR_UNO
# uno.build.core=arduino
# uno.build.variant=standard
#CXX = /snap/arduino/current/hardware/tools/avr/bin/avr-gcc
#CPPFLAGS = \
#    -DARDUINO=10813 -DARDUINO_ARCH_AVR=1 -DARDUINO_AVR_UNO=1 \
#    -D__AVR_ATmega328P__ -DF_CPU=16000000  -D__ATTR_PROGMEM__=
#CXXFLAGS = \
#    -O \
#    -I/snap/arduino/current/hardware/tools/avr/avr/include \
#    -I/snap/arduino/current/hardware/arduino/avr/cores/arduino \
#    -I/snap/arduino/current/hardware/arduino/avr/variants/standard \
#    -I../../libraries/CustomSoftwareSerial/src

# --- NodeMcu ESP8266 Xtensa (32-bit RISC) ---
#CXX = $(HOME)/snap/arduino/current/.arduino15/packages/esp8266/tools/\
#xtensa-lx106-elf-gcc/2.5.0-4-b40a506/bin/xtensa-lx106-elf-gcc

# --- Test mode ---
CXX = g++
CPPFLAGS = -DTEST_BUILD -g
CXXFLAGS = -Wall -Os

test: ./pe32me162ir_pub.test
	./pe32me162ir_pub.test

clean:
	$(RM) $(OBJECTS) ./pe32me162ir_pub.test

example.diff: example.log
	bash -c "diff -u \
	  <(git show HEAD:example.log | sed -e 's/^[^-]*-> //') \
	  <(sed -e 's/^[^-]*-> //' example.log)" | less; true

example.log: raw.log
	./raw2example < raw.log > example.log

$(OBJECTS): WattGauge.h config.h

pe32me162ir_pub.test: $(OBJECTS)
	$(LINK.cc) -o $@ $^
