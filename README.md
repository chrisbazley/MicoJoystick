# MicoJoystick
PC Joystick driver for Mico

Author: Chris Bazley

Version: 2.02 (18th September 2004)

Introduction
============

  This is a module that adds support for up to two standard PC analogue
joysticks connected to the gameport of a MicroDigital Mico. Because it
implements the standard Joystick SWIs it should be compatible with all
existing and future RISC OS software.

-----------------------------------------------------------------------------
Requirements
============

  In terms of hardware you need a Microdigital Mico with ISA sound card, on
which a gameport is mounted. Of course you also need a PC analogue joystick
to plug into this (nothing fancy - hats/throttles aren't supported). In fact
the Joystick module will use any hardware address set up by the Plug'n'Play
manager, so in future it may also work with PCI cards in the Omega.

  The Plug'n'Play support software must have been run in order to set up the
address of the gameport. If the system variable PnPManager$GamesPort_Address
is not set then the Joystick module will fail to initialise with the error
"Plug'n'Play not initialised correctly, or no gameport fitted".

-----------------------------------------------------------------------------
Usage instructions
==================

  Connect your joystick(s) to the gameport. Ensure that all your joysticks
are centred before initialising the Joystick module. Although the general
characteristics of joysticks will be auto-detected, you may also wish to run
the supplied 'JoyTest' program to calibrate your joystick(s) to return the
full range of values.

  You may now play games using your analogue joystick, providing that they
support the standard `Joystick_Read` SWI. The 'JoyTest' program also
provides a simple demonstration of whether or not the joystick driver is
working properly.

  If you connect a joystick with different characteristics or forget to plug
in your joystick before loading the module then you must `*JoystickReInit` the
relevant stick number in order to detect it.

  Once you have your joystick(s) calibrated to your satisfaction you may
wish to save the Joystick module's configuration. To do this you can write
an Obey file (perhaps executed on boot-up) that contains the appropriate
`*JoystickConfig` and `*JoystickCalib` commands.

-----------------------------------------------------------------------------
Overview
========

  Analogue joysticks contain potentiometers (one for each axis of movement),
the resistance of which varies as the joystick is moved around. The joystick
position is read by timing how long capacitors take to charge from the
current flowing through these potentiometers. A software loop must wait and
watch for the capacitors to finish in order to obtain timings.

  This type of joystick hardware is simple and cheap, but inevitably rather
inaccurate. The foremost problem is 'jitter', which is the phenomenon of
axis timings jumping about within a limited range without the joystick
actually being moved. Therefore the raw axis timings go through a variety of
processing stages to smooth out the rough edges before they are passed to
client programs (such as games).

  Firstly a smoothing function averages together values that are in close
proximity to each other, in order to alleviate the effects of jitter
somewhat. Smoothing may be disabled for individual axes, or globally if
desired. For more about the smoothing algorithm see "Further technical
details".

  When the `Joystick_Read` SWI is called the axis timings are converted to a
standard 8-bit or 16-bit range of values suitable for use by client
programs. This conversion process compensates for any variation between
joysticks in the range of timings obtained (as determined by joystick
calibration).

  'Dead zones' make it easier to hold a steady centre value or to
consistently reach the extremities of a joystick's range despite residual
jitter. As part of the conversion to a standard range, timings within a
certain range of the centre and end points automatically snap to proscribed
absolute centre/maximum/minimum values.

  For more information on how to read, set and interpret calibration values,
see the chapter "Calibration values."

Flow diagram of joystick driver
-------------------------------
```
      _______________
     | Read gameport |
     |_______________|
            || Raw axis timings
  __________\/____________
 | Smooth adjacent values |
 |________________________|
            || Smoothed timings
      ______||______
      |+----------+|
      || [1]      || [0] (Joystick_Read reason code)
  ____\/____  ____\/____
 | Convert  || Convert  |
 |    to    ||    to    |
 |  16-bit  ||   8-bit  |
 | unsigned ||  signed  |
 |__________||__________|
      ||          || Calibrated values
      \/          \/
```
-----------------------------------------------------------------------------
Calibration Values
==================

  Not all joysticks have resistances in the same range or return the same
timings for a given position. Therefore average maximum, minimum and centre
timings are recorded for each joystick axis and used in conversion of
timings to a standard range of values suitable for use by client programs.
The amount of jitter (even after smoothing) is also recorded at each of
these key positions, in order to implement dead zones (see "Overview").
These values, together with a few others used in processing raw axis
timings, are known collectively as the 'calibration values' for an axis.

  Generally you will calibrate the joystick using the supplied 'JoyTest'
program, but it may occasionally be useful to understand the actual meaning
of the resultant values. You can view a table of the current calibration
values for all joysticks using the command `*JoystickInfo`. Values may be
altered manually using the `*JoystickCalib` command with a stick number, axis
(X or Y), and any combination of the various named value arguments supported.

  The following diagram shows a joystick axis, with the main calibration
values labelled (using the names used for the `*JoystickCalib` command):
```
    min                 ctr                 max
     |    <----------    |    ---------->    |
      \__/           \__/ \__/           \__/
     endzone      ctrzone ctrzone       endzone
```
  In addition to these calibration values each axis also has a 'smooth'
value that is the base range of the smoothing algorithm.

Full calibration is a three-stage process:
- The `*JoystickReInit` command sets the 'ctr', 'ctrzone' and 'smooth' values
  for both X and Y axes. The 'min' values are set to 0 and the 'max' values
  are set to double the 'ctr' value for that axis.
- The `Joystick_CalibrateTopRight` SWI sets the Y axis 'min' and X axis 'max'
  values.
- The `Joystick_CalibrateBottomLeft` SWI sets the X axis 'min' and Y axis
  'max' values. When calibration is completed the 'endzone' value for each
  axis is set to the larger of the jitter ranges recorded at either end (to
  keep things symmetrical).

  You may set the 'ctrzone' and 'endzone' values to 0, effectively disabling
the dead zones for that axis. Timings close to the centre or endpoints will
then no longer be snapped to absolute values, if this is not to your taste.

  By default the smoothing range is half the jitter range recorded upon
joystick initialisation. Thus smoothing is always enabled by default, unless
your joystick truely is perfect. You can disable smoothing for an individual
axis by setting the 'smooth' range to 0.

-----------------------------------------------------------------------------
General Configuration
=====================

  There are a number of configuration options that affect the whole Joystick
module and hence the values returned by all joysticks. These options may be
set individually or all together using the `*JoystickConfig` command, which
accepts a list of arguments.

Smoothing and dead zones
------------------------
  Smoothing may be configured on or off using the `-[no]smooth` switch.
Disabling it will cause raw axis timings to be passed directly to the
8-bit/16-bit conversion process, which may lead to improved responsiveness
from the joysticks at the expense of programs getting more 'noisy' readings.
Slower computers may benefit slightly from a reduction in work required.

  Centre and end dead zones may be configured on or off using the
`-[no]ctrzone` and `-[no]endzone` switches. Disabling dead zones will cause
more of the joystick's actual range to be used, and make the stick more
'responsive' near the centre. However it will make it harder to reliably
centre the stick or consistently reach the extremities. Enabling dead zones
will not slow down the joystick driver.

  Note that disabling smoothing will not zero the smooth ranges for
individual axes; these values will simply not be used. Similarly, ranges for
centre and end zones may exist regardless of whether `-noctrzone` or
`-noendzone` has been configured.

  Since calibration values are calculated from actual (smoothed) timings it
may be wise to recalibrate all joysticks after enabling/disabling smoothing.
Most noticeably, it is likely that smaller dead zones will be required when
smoothing is enabled.

Timing limits
-------------
  Because interrupts are necessarily enabled while timing the joystick axes,
the ARM may be servicing an interrupt at the crucial moment. To catch
inaccurate timings, there is a maximum tolerated delay between when the
gameport was last read and recording an axis timing.

  This time interval may be configured using the `-tolerance <interval>`
option. If the tolerance value is too small then slow machines won't be able
to read the joystick at all, and the joystick may feel unresponsive under
heavy interrupt load. If the tolerance value is too large then inaccurate
timings will not be discarded. The default tolerance is 15 microseconds.

  The maximum time to wait (in microseconds/2) for a response from all joystick axes
before giving up may be configured using the `-timeout <delay>` option. The
default is 1000 microseconds - you are unlikely to need to change this unless your
joystick is very 'slow' and hence is not detected, or its upper range is
being ignored.

Polling frequency
-----------------
  The frequency (in centiseconds) with which the joystick(s) are read may be
configured using the `-poll <frequency>` option. The default polling speed
is every 7cs. You may wish to reduce this in order to increase joystick
responsiveness, especially with large smooth ranges. However, there is a
performance penalty for reading the joystick(s) more often - at the maximum
frequency of 2cs up to 5% of CPU time may be used!

  For more about polling see "Further technical details".

-----------------------------------------------------------------------------
Further technical details
=========================

Polling
-------
  The joystick is polled periodically in the background - `Joystick_Read`
simply returns the last stick positions read. This ensures that
responsiveness is not determined by how often the `Joystick_Read` SWI is
called, and also that a succession of calls to `Joystick_Read` (e.g. to read a
number of different joysticks) do not each trigger a time-consuming read
operation.

  OS_CallEvery is used to install a ticker event routine that is called
regularly with interrupts disabled - by default every 7 centiseconds
(70,000 microseconds). In turn, this adds a transient callback to read the joystick as
soon as possible. Reading the joystick cannot be done in the actual ticker
event routine because this would mean spending too long with interrupts
disabled.

  The Joystick module does not start polling the joystick until
`Joystick_Read` is first called, so simply having the module loaded will not
affect the computer's performance. Polling is automatically disabled after a
period of inactivity lasting 10 seconds, typically after having quit a game.
Polling is also disabled during calibration in order to avoid disruption to
the joystick read operations going on in the foreground.

  Since a joystick read operation times out after 1000 microseconds (by default), in
normal operation the Joystick module should take no more than about 1.5% of
CPU time. Fire buttons are read instantaneously whenever `Joystick_Read` is
called, at virtually no cost.

Smoothing
---------
  Rather than a single smoothing function, different levels of smoothing are
applied depending on the distance between current and new values. The range
within which smoothing operates is defined on a per-axis basis by the
'smooth' calibration value.

  Within the configured smoothing range maximum smoothing is applied - 3/4
of the previous value to only 1/4 of the new. Moderate smoothing is applied
to values within double this range (1/2 old value, 1/2 new value). Only
slight smoothing is applied to values within quadruple the configured range
(1/4 old value, 3/4 new value).

  Outside this range the new value is used verbatim, allowing swift response
to violent actions such as suddenly pushing the stick right over.

Filtering
---------
  Development versions of the module included a filtering mechanism that
buffered the three last axis timings (grandfather, father, son) in order to
detect and remove apparently anomalous values. This feature never made it
into release versions, partly because the introduction of timing limits
(`-tolerance <interval>`) reduces the need for it, and partly because it
introduces an additional (though small) delay in joystick responsiveness.

  The filtering code still exists, however, and may possibly appear in
subsequent versions of the module and its source code, if only as a compile-
time option.

-----------------------------------------------------------------------------
Star Commands
=============

JoystickConfig
--------------
Syntax: `*JoystickConfig [-smooth|-nosmooth] [-ctrzone|-noctrzone]
        [-endzone|-noendzone] [-tolerance <interval>] [-timeout <delay>]
        [-poll <frequency>]`

This command configures the analogue joystick driver, or with no parameters
displays the current settings. For further details see "General
Configuration". Time values are in units of 1/2 microsecond, except for poll
frequency which is in centiseconds.

JoystickCalib
-------------
Syntax: `*JoystickCalib <stick number> X|Y [-min <time>] [-ctr <time>]
        [-max <time>] [-ctrzone <interval>] [-endzone <interval>]
        [-smooth <interval>]`

This command allows you to manually set one or more of the calibration
values for a joystick axis. For further details see "Calibration Values".
Time values are in units of 1/2 microsecond.

JoystickReInit
--------------
Syntax: `*JoystickReInit [<stick number>]`

  This command auto-detects a joystick's characteristics and calibrates it
using the current stick position as centre. If no stick number is specified
then all joysticks are re-initialised.

JoystickInfo
------------
Syntax: `*JoystickInfo`

This command displays the current calibration values for all joysticks in
tabular form, for example:
```
Axis Minimum Centre Maximum Ctr zone End zone Smooth
---- ------- ------ ------- -------- -------- ------
 0 X       0    703    1406       48        0     48
 0 Y       0    847    1694       48        0      9
 1 X       0    800    1600        0        0      0
 1 Y       0    800    1600        0        0      0
```
-----------------------------------------------------------------------------
Joystick SWIs
=============
The following is a brief summary of the more detailed documentation in the
PRMs.

Joystick_Read (SWI &43F40)
--------------------------
Reads the current state of a joystick.
```
On entry:
  R0 = joystick number and reason code:
       bits 0-7   - joystick number (0 = first, 1 = second, etc)
       bits 8-15  - reason code:
         0 - read 8-bit state of a switched or analogue joystick
         1 - read 16-bit state of an analogue joystick
       bits 16-31 - reserved (0)

On exit:
  Registers depend on reason code (see below)
```
Joystick_Read 0
---------------
Reads the 8-bit state of a switched or analogue joystick.
```
On exit:
  R0 = 8-bit joystick state:
       bits 0-7  - signed 8-bit y value in the range -127 (back) to +127
                   (forward)
       bits 8-15 - signed 8-bit x value in the range -127 (left) to +127
                   (right)
       bits 16-23 - fire buttons (bits set reflect buttons pushed)
       bits 24-31 - reserved (0)

  When reading only forward/back/left/right state, it is recommended that
the 'at rest' state should span a middle range (say from -32 to +32) since
analogue joysticks do not reliably produce the value 0 when in a neutral
position.
```
Joystick_Read 1
---------------
Reads the 16-bit state of an analogue joystick.
```
On exit:
  R0 = 16-bit joystick position:
       bits 0-15  - 16-bit y value in the range 0 (back) to 65535 (forward)
       bits 16-31 - 16-bit x value in the range 0 (left) to 65535 (right)
  R1 = fire buttons:
       bits 0-7  - fire buttons (bits set reflect buttons pushed)
       bits 8-31 - reserved (0)
```
  When reading only forward/back/left/right state, it is recommended that
the 'at rest' state should span a middle range (say from 24576 to 40960)
since analogue joysticks do not reliably produce the value 32767 when in a
neutral position.

Joystick_CalibrateTopRight (SWI &43F41)
---------------------------------------
Part of analogue joystick calibration procedure.
```
On entry:
  --

On exit:
  --
```
  To calibrate an analogue joystick, call this SWI (with the stick held in
the forward right position) and then `Joystick_CalibrateBottomLeft`. After
calling only one of the pair `Joystick_Read` will return a error until the
calibration process is properly completed.

Joystick_CalibrateBottomLeft (SWI &43F42)
-----------------------------------------
Part of analogue joystick calibration procedure.
```
On entry:
  --

On exit:
  --
```
  To calibrate an analogue joystick, call this SWI (with the stick held in
the back left position) and then `Joystick_CalibrateTopRight`. After calling
only one of the pair `Joystick_Read` will return a error until the calibration
process is properly completed.

-----------------------------------------------------------------------------
History
=======

2.00 (9th September 2002)
 - Initial release. (Third party replacements for the Joystick module should
   have version numbers greater than 2.00.)

2.01 (11th November 2002)
 - Recompiled using the official Castle release of 32-bit Acorn C/C++.

2.01 (24th December 2002)
 - Released with tidied up source code under the GNU Public Licence.
 - Added information on old filtering mechanism.

15th March 2003
 - Recompiled without stack-limit checking and embedded function names (both
   redundant in SVC mode), saving a little time and memory.

2.02 (18th September 2004)
 - Rebuilt as 26 bit code for linking with the ROM version of the shared C
   library. It saves memory if we don't require the APCS-32 shared C library
   to be loaded first, and means that the module should now work on a
   'vanilla' Mico computer. There seems no prospect of a machine with a
   32 bit OS being able to make use of this driver anyway.
 - Formatted this text for a fixed-width 77 column display (Zap's default).

-----------------------------------------------------------------------------
Credits
=======

This joystick driver for the MicroDigital Mico is (C) 2002 Chris Bazley

Thanks to the following people:
   Dave Prosser of MicroDigital (info on Mico gameport)
   Steve McGowan and Mark Feldman (PC Game Programmer's Encyclopedia)
   Jack Morrison (joystick test C program)
   Tomi Engdahl (info on PC analogue joystick interface)
   Vojtech Pavlik (Linux joystick driver author)

-----------------------------------------------------------------------------
Disclaimer
==========

  This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public Licence as published by the Free
Software Foundation; either version 2 of the Licence, or (at your option)
any later version.

  This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public Licence for
more details (in the text file "GNUlicence").

-----------------------------------------------------------------------------
Contact details
===============

  Feel free to contact me with any bug reports, suggestions or anything else.

  Email: mailto:cs99cjb@gmail.com

  WWW:   http://starfighter.acornarcade.com/mysite/

