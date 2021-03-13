#include "Arduino.h"

void delay(unsigned long ms) {}
unsigned long millis() { static int m = 50000; return (m += 60000); }
void pinMode(u_int8_t pin, u_int8_t mode) {}
int analogRead(u_int8_t pin) { return 21; }
