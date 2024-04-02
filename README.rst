pe32me162ir_pub
===============

**Project Energy 32:** *Read electricity meter (ISKRA ME162), through
optical port, export power usage using MQTT.*

This project contains *C* code to read values from the *ISKRA ME-162
electricity meter* using an *ESP8622* (or *Arduino*) and push them to an
*MQTT broker*.

(There is also a *Python* version of this project at `pe32me162irpy_pub
<https://github.com/wdoekes/pe32me162irpy_pub>`_.)

Features:

- Although the *ISKA ME162* does not include power readouts (in Watt), we
  query the total energy consumed/produced (in Watt-hour) every second
  or so. This allows us to get a fair estimate of instantanous power
  usage. This can in fact give a better estimate than just sampling
  instantaneous power every now and then (which would be possible on other
  meters).

Required hardware: a so-called optical probe.

- *2023* - Bret McGee offers pre-soldered *optical probes* at `ebay UK
  <https://www.ebay.co.uk/itm/204371156344>`_. You'll likely want to 3D
  print a `plastic cover <https://www.thingiverse.com/thing:2652216>`_
  with magnets for easy attachment to your meter.

  *(I've been told Aalborg hackers stopped shipping their packages:
  "Bemærk: Vi sælger ikke længere kits.")*

- *2021* - On the `Hal9k Kamstrup Project page
  <https://wiki.hal9k.dk/projects/kamstrup>`_ (by Aalborg hackers) you can
  find instructions to build an *optical probe* (infrared transceiver) to
  communicate with *Kamstrup electricity meters* using the optical
  communications port. Such an optical communication port is available on
  several other electricity meters, like the *ISKRA ME-162* commonly found
  in the Netherlands. This optical probe can also be used on those.


-----
HOWTO
-----

1.  First, you order the soldered probe from `Bret on ebay UK
    <https://www.ebay.co.uk/itm/204371156344>`_ and 3D print the cover.

    **BEWARE: If you're doing a solder based on the Kamstrup images,
    know that the images on their page actually have the two BC547
    transistors depicted in reverse. If you look at the diagram, you'll
    find they should be rotated 180 degrees, with the Collectors
    connected to the 10k resistors.**

    .. image:: assets/kamstrupkitv2.jpg

    .. image:: assets/kamstrup-diagram-as-png.png

    .. image:: assets/kamstrup-pcb-mirror-invert.png

    *(the PCB, as seen from the bottom)*

    .. image:: assets/pe32-soldered-ir-pcb.png

    *(my shabby soldering, with the BC547s turned the right way)*

    .. image:: assets/pe32-magnets.png

    *(gently inserting the magnets into the plastic shell, after
    widening the openings a bit)*

2.  Second, you test that the infrared transmitter works, by attaching it
    to an Arduino or similar, and running something like this:

    .. code-block:: c
        :force:

        const int PIN_TX = 10;
        const int PIN_LED = 13;
        int val = LOW;

        void setup() {
          pinMode(PIN_TX, OUTPUT);
          pinMode(PIN_LED, OUTPUT);
        }

        void loop() {
          val = (val == LOW) ? HIGH : LOW;
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

    .. image:: assets/pe32-ir-test-tx.gif

3.  Third, you check that the infrared reception works. Run the
    following code:

    .. code-block:: c

        const int PIN_RX = 9;
        const int PIN_TX = 10;
        const int PIN_LED = 13;

        void setup() {
          pinMode(PIN_RX, INPUT);
          pinMode(PIN_TX, OUTPUT);
          pinMode(PIN_LED, OUTPUT);

          // TX must be HIGH (=no transmitted light), or RX will always
          // be HIGH (=no light reception).
          digitalWrite(PIN_TX, HIGH);
          digitalWrite(PIN_LED, HIGH);
        }

        void loop() {
          int val = digitalRead(PIN_RX);
          // Daylight or a bright lamp makes the Arduino LED go out.
          // Alternately, reception of a TV remote control infrared light
          // will cause visible flicker of the Arduino LED.
          if (val == LOW) {
            digitalWrite(PIN_LED, LOW);
            delay(50);
          } else {
            digitalWrite(PIN_LED, HIGH);
          }
        }

    When the RX photo transistor receives (infrared, but also other)
    light, the RX PIN will be pulled low. The sketch will pull the LED
    PIN low: LED off. (And vice versa: no IR light causes the LED to
    turn on.)

    .. image:: assets/pe32-ir-test-rx.gif

When you have completed the above steps, you should be able to hook it
up to your electricity meter. Check the comments at the top of the
`pe32me162ir_pub.ino <pe32me162ir_pub.ino>`_ source file for PIN details
and configuration.

After hooking everything up, your meter cupboard might look like this:

.. image:: assets/pe32-meter-cupboard.png


-------------
MQTT messages
-------------

At the moment, the MQTT messages will look as follows.

Initial publish after device startup::

    device_id=EUI48:11:22:33:44:55:66&id=ISK5ME162-0033&DATA=
      C.1.0(47983850)\r\n0.0.0(47983850)\r\n1.8.0(0033271.483*kWh)\r\n
      1.8.1(0000000.000*kWh)\r\n1.8.2(0033271.483*kWh)\r\n
      2.8.0(0000007.784*kWh)\r\n2.8.1(0000000.000*kWh)\r\n
      2.8.2(0000007.784*kWh)\r\nF.F(0000000)\r\n!\r\n

Consecutive publishes look like::

    device_id=EUI48:11:22:33:44:55:66&
      e_pos_act_energy_wh=33271493&e_neg_act_energy_wh=7784&
      e_inst_power_w=1397&dbg_uptime=31267

Where the keys mean:

- e_pos_act_energy_wh (1.8.0) = Positive active energy [Wh]
- e_neg_act_energy_wh (2.8.0) = Negative active energy [Wh]
- e_inst_power_w (16.7.0) = Sum of active instantaneous power [Watt]


-------------
Local testing
-------------

For testing/compiling while developing, we use the *bogoduino*
submodule::

    $ git submodule init
    Submodule 'bogoduino' (https://github.com/wdoekes/bogoduino.git) registered for path 'bogoduino'

    $ git submodule update
    Cloning into 'pe32me162ir_pub/bogoduino'...
    Submodule path 'bogoduino': checked out '7bec2a5'

Now you can run ``make`` to run some test code::

    $ make
    ./pe32me162ir_pub.test
    OK (cescape): """a"""
    ...


-----------------------------
The issue with the odd spikes
-----------------------------

(Note, the following issue was only relevant up until commit `d844533
<https://github.com/wdoekes/pe32me162ir_pub/commit/d84453351f3ede232571281e643d02eb6fb785e4>`_.
After that commit, visible LED pulses are not that important because we
query the meter for totals every second. You'd now need to enable
``OPTIONAL_LIGHT_SENSOR`` for this functionality.)

Occasionally, we would see these odd spikes::

    +34.0  16:00:53 {'e_pos_act_energy_wh': 32917428, 'e_inst_power_w': 428, 'dbg_uptime': 6807478, 'dbg_pulse': '1..101'}
    +34.0  16:01:27 {'e_pos_act_energy_wh': 32917432, 'e_inst_power_w': 428, 'dbg_uptime': 6841062, 'dbg_pulse': '1..133'}
    +33.0  16:02:00 {'e_pos_act_energy_wh': 32917437, 'e_inst_power_w': 535, 'dbg_uptime': 6874655, 'dbg_pulse': '1..111'}
    +34.0  16:02:34 {'e_pos_act_energy_wh': 32917440, 'e_inst_power_w': 321, 'dbg_uptime': 6908240, 'dbg_pulse': '1..171'}
    +33.0  16:03:07 {'e_pos_act_energy_wh': 32917444, 'e_inst_power_w': 427, 'dbg_uptime': 6941936, 'dbg_pulse': '1..192'}
    +34.0  16:03:41 {'e_pos_act_energy_wh': 32917448, 'e_inst_power_w': 427, 'dbg_uptime': 6975619, 'dbg_pulse': '1..161'}
    +34.0  16:04:15 {'e_pos_act_energy_wh': 32917452, 'e_inst_power_w': 429, 'dbg_uptime': 7009170, 'dbg_pulse': '1..157'}
    +33.0  16:04:48 {'e_pos_act_energy_wh': 32917457, 'e_inst_power_w': 536, 'dbg_uptime': 7042692, 'dbg_pulse': '1..118'}
    +34.0  16:05:22 {'e_pos_act_energy_wh': 32917460, 'e_inst_power_w': 321, 'dbg_uptime': 7076275, 'dbg_pulse': '1..174'}
    +34.0  16:05:56 {'e_pos_act_energy_wh': 32917464, 'e_inst_power_w': 424, 'dbg_uptime': 7110158, 'dbg_pulse': '1..133'}
    +36.0  16:06:32 {'e_pos_act_energy_wh': 32917468, 'e_inst_power_w': 395, 'dbg_uptime': 7146556, 'dbg_pulse': '1..134'}

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

----

*Project energy 32* is a suite of personal home readout/automation
tools. Batteries are *not* included. You need to set up an *MQTT
broker*, a database to store the readouts, a backend that subscribes and
inserts the values, vacuuming/pruning code, and something to display the
values (like *Grafana*).
