#CXX = /snap/arduino/current/hardware/tools/avr/bin/avr-gcc
CXX = g++

# /snap/arduino/current/hardware/arduino/avr/boards.txt:
# uno.build.mcu=atmega328p
# uno.build.f_cpu=16000000L
# uno.build.board=AVR_UNO
# uno.build.core=arduino
# uno.build.variant=standard
CPPFLAGS = -DTEST_BUILD -D__ATTR_PROGMEM__= -D__AVR_ATmega328P__ -DF_CPU=16000000
CXXFLAGS = \
    -O \
    -I/snap/arduino/current/hardware/tools/avr/avr/include \
    -I/snap/arduino/current/hardware/arduino/avr/cores/arduino \
    -I/snap/arduino/current/hardware/arduino/avr/variants/standard \
    -I$(HOME)/Arduino/libraries/CustomSoftwareSerial

test: pe32me162ir_pub.test
	./pe32me162ir_pub.test

pe32me162ir_pub.test: pe32me162ir_pub.cc
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

clean:
	$(RM) pe32me162ir_pub.test

