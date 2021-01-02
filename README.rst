pe32me162ir_pub
===============

*Talk to optical port of ISKRA ME-162, export to MQTT.*


Bugs
----

Occasionally, we still see spikes that cannot be explained by bugs in
the code::

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

These always appear to be early counts, not late ones.

*Possibly we're always getting a value too early: if the pulse is sent
before the Wh is counted. And only during the spikes we're getting the
right value? We could test this by adding a second delay before the
first get.*
