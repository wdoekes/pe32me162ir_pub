pe32me162ir_pub
===============

Project Energy 32: *Talk to ISKRA ME162 through optical port, export to MQTT.*

On the `Hal9k Kamstrup Project page
<https://wiki.hal9k.dk/projects/kamstrup>`_ (by Aalborg hackers) you can
find instructions to build an *optical probe* (infrared transceiver) to
communicate with *Kamstrup electricity meters* using the optical
communications port. Such an optical communication port is available on
several other electricity meters, like the *ISKRA ME-162* commonly found
in the Netherlands. This optical probe can also be used on those.

This project contains Arduino/ESP8266 code to read values from the
*ISKRA ME-162 electricity meter* and push them to an MQTT broker.


HOWTO
-----

1.  First, you order the probe kit from the Danes: on their `project page
    <https://wiki.hal9k.dk/projects/kamstrup>`_ they describe the
    contents and how to order.

    .. image:: assets/kamstrupkitv2.jpg

2.  Second, you solder the components together. Again, refer to the
    *Kamstrup Project page*.

    **BEWARE: The images on their page actually have the two BC547
    transistors depicted in reverse. If you look at the diagram, you'll
    find they should be rotated 180 degrees, with the Collectors
    connected to the 10k resistors.**

    .. image:: assets/kamstrup-diagram-as-png.png

    .. image:: assets/kamstrup-pcb-mirror-invert.png

    *(the PCB, as seen from the bottom)*

    .. image:: assets/pe32-soldered-ir-pcb.png

    *(my shabby soldering, with the BC547s turned the right way)*

    .. image:: assets/pe32-magnets.png

    *(gently inserting the magnets into the plastic shell, after
    widening the openings a bit)*

3.  Third, you test that the infrared transmitter works, by attaching it
    to an Arduino or similar, and running something like this:

    .. code-block:: c

        const int PIN_TX = 10;
        const int PIN_LED = 13;
        int val;

        void setup() {
          pinMode(PIN_TX, OUTPUT);
          pinMode(PIN_LED, OUTPUT);
        }

        void loop() {
          val = HIGH;
          digitalWrite(PIN_TX, val);
          digitalWrite(PIN_LED, val);
          delay(1000);

          val = LOW;
          digitalWrite(PIN_TX, val);
          digitalWrite(PIN_LED, val);
          delay(1000);
        }

    This causes the Arduino LED and the TX LED to take turns lighting
    up. Because the TX LED will be off when the TX PIN is high.

    *When you look at the infrared LED with a digital photo camera (on
    your phone), you should be able to see the light as pink. (You can
    confirm that your camera sees it by looking at a TV remote control
    when it's transmitting.)*

    .. image:: assets/kamstrup-pink-tx-light.jpg

4.  Fourth, you check that the infrared reception works. Run the following code:

    .. code-block:: c

        const int PIN_RX = 9;
        const int PIN_TX = 10;
        const int PIN_LED = 13;

        void setup() {
          pinMode(PIN_RX, INPUT);
          pinMode(PIN_TX, OUTPUT);
          pinMode(PIN_LED, OUTPUT);

          // TX must be LOW, or RX will always be LOW.
          digitalWrite(PIN_TX, LOW);
          digitalWrite(PIN_LED, LOW);
        }

        void loop() {
          int val = digitalRead(PIN_RX);
          digitalWrite(PIN_LED, val);
        }

    When the RX photo transistor receives (infrared, but also other)
    light, the RX PIN will be pulled low. The sketch will pull the LED
    PIN low: LED off. (And vice versa: no RX light causes the LED to
    turn on.)

When you have completed the above steps, you should be able to hook it
up to your electricity meter. Check the commands in the
`pe32me162ir_pub.ino <pe32me162ir_pub.ino>`_ source code for PIN details
and configuration.

After hooking everything up, your meter cupboard might look like this:

.. image:: assets/pe32-meter-cupboard.png

*Note that setting up a MQTT broker and a subscriber for the pushed data
is beyond the scope of this HOWTO. Personally, I use Mosquitto (broker),
a custom subscriber, PostgreSQL (with timescale) and Grafana for
visualisation.*


MQTT messages
-------------

At the moment, the MQTT messages will look as follows.

Initial publish after device startup::

    device_id=...&power_hello=ISK5ME162-0033&DATA=
      C.1.0(28342193)\r\n0.0.0(28342193)\r\n1.8.0(0032916.425*kWh)\r\n
      1.8.1(0000000.000*kWh)\r\n1.8.2(0032916.425*kWh)\r\n2.8.0(0000000.001*kWh)\r\n
      2.8.1(0000000.000*kWh)\r\n2.8.2(0000000.001*kWh)\r\nF.F(0000000)

Consecutive publishes look like::

    device_id=...&watthour[0]=32916429&watt[0]=364.41&
      watthour[1]=1&watt[1]=0.00&uptime=54170&
      pulse_low=8&pulse_high=205

(Except for the second publish, which will not have the ``watt[0]`` and
``watt[1]`` values, because they are calculated from a delta, and the
second publish doesn't have two values to compare yet.)

**BEWARE: The MQTT message format is not well thought out nor
standardized. I will change it at some point without prior notice! 😈**


The issue with the odd spikes
-----------------------------

Occasionally, we would see these odd spikes::

    +34.0  16:00:53 {'watthour[0]': 32917428, 'watt[0]': 428.78, 'uptime': 6807478, 'pulse_low': '1', 'pulse_high': '101'}
    +34.0  16:01:27 {'watthour[0]': 32917432, 'watt[0]': 428.79, 'uptime': 6841062, 'pulse_low': '1', 'pulse_high': '133'}
    +33.0  16:02:00 {'watthour[0]': 32917437, 'watt[0]': 535.79, 'uptime': 6874655, 'pulse_low': '1', 'pulse_high': '111'}
    +34.0  16:02:34 {'watthour[0]': 32917440, 'watt[0]': 321.58, 'uptime': 6908240, 'pulse_low': '1', 'pulse_high': '171'}
    +33.0  16:03:07 {'watthour[0]': 32917444, 'watt[0]': 427.36, 'uptime': 6941936, 'pulse_low': '1', 'pulse_high': '192'}
    +34.0  16:03:41 {'watthour[0]': 32917448, 'watt[0]': 427.5,  'uptime': 6975619, 'pulse_low': '1', 'pulse_high': '161'}
    +34.0  16:04:15 {'watthour[0]': 32917452, 'watt[0]': 429.2,  'uptime': 7009170, 'pulse_low': '1', 'pulse_high': '157'}
    +33.0  16:04:48 {'watthour[0]': 32917457, 'watt[0]': 536.94, 'uptime': 7042692, 'pulse_low': '1', 'pulse_high': '118'}
    +34.0  16:05:22 {'watthour[0]': 32917460, 'watt[0]': 321.6,  'uptime': 7076275, 'pulse_low': '1', 'pulse_high': '174'}
    +34.0  16:05:56 {'watthour[0]': 32917464, 'watt[0]': 424.99, 'uptime': 7110158, 'pulse_low': '1', 'pulse_high': '133'}
    +36.0  16:06:32 {'watthour[0]': 32917468, 'watt[0]': 395.62, 'uptime': 7146556, 'pulse_low': '1', 'pulse_high': '134'}

That is, at ``16:02:00``, there appears to be a Wh value too many (+5
instead of +4) which is compensated for at ``16:02:34`` (+3 instead of
+4). And, again at ``16:04:48`` and ``16:05:22``. Instead of 535 and 321
Watt, we'd expect 423 and 436 Watt.

.. image:: ./assets/bugs-unexplained-spikes-1600.png

These always appear to be early counts, not late ones.

*A possible cause could be that we're always getting a value too early:
if the LED pulse is sent before the Wh is counter is incremented, we might
"normally" get a pulse too little, and only sometimes we'd get the right
value (i.e. one more).*

.. image:: ./assets/bugs-delay-500-does-not-fix-spikes.png

The above graph initially seemed to disprove that theory, but after
increasing the delay to a full second, the spikes disappeared.

.. image:: ./assets/bugs-spikes-fixed.png

Now the new graph is more in line with the "old" counter (which was
still in use last week) which `read the LED pulses
<https://github.com/wdoekes/pe32me162led_pub>`_ to indicate power
consumption.
