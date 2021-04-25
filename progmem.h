#ifndef INCLUDED_PROGMEM_H
#define INCLUDED_PROGMEM_H

#include <avr/pgmspace.h>

/* Declare the common name for PROGMEM/flash strings. */
class __FlashStringHelper;

/* Typedef it to a friendlier name. */
typedef __FlashStringHelper pgm_char;

/* Defines for casting. */
#define from_pgm_char_p(x) reinterpret_cast<const char*>(x)
#define to_pgm_char_p(x) reinterpret_cast<const pgm_char*>(x)

/* Defines for initialization.
 *
 * Problem:
 *   const char my_string[] PROGMEM = "flash string here";
 *   // now we don't know that my_string is "special"
 * Poor solution:
 *   const char char_my_string[] PROGMEM = "flash string here";
 *   const pgm_char *const my_string = to_pgm_char(char_my_string);
 * Better solution:
 *   DECLARE_PGM_STR(my_string, "flash string here");
 */
#define DECLARE_PGM_CHAR_P(identifier, value) \
    const char (char_ ## identifier)[] PROGMEM = value; \
    const pgm_char *const identifier = to_pgm_char_p(char_ ## identifier)

#endif //INCLUDED_PROGMEM_H
