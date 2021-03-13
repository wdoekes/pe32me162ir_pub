#ifndef INCLUDED_BOGO_SOFTWARESERIAL_H
#define INCLUDED_BOGO_SOFTWARESERIAL_H

const int SWSERIAL_7E1 = 0;

class SoftwareSerial {
public:
  SoftwareSerial(int, int, bool) {}
  void begin(long, unsigned short) {}
  int available() { return true; }
  int read() { return 0; }
  void print(char const *) {}
};

#endif //INCLUDED_BOGO_SOFTWARESERIAL_H
