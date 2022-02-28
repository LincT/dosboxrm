

# Overview

I started this fork of DOSBox in order to run ReelMagic games. It would appear
that no one else has done this, so I took this on as I really wanted to play the
ReelMagic version of Return to Zork.

## Contributors

The following people have made this project a reality:

* Jon Dennis <jrdennisoss@gmail.com>
* Chris Guthrie <csguthrieoss@gmail.com>
* Joseph Whittaker <j.whittaker.us@ieee.org>

It is also worth mentioning Dominic Szablewski's (https://phoboslab.org) awesome
MPEG decoder library `PL_MPEG` was used for this, and of course this project is
built on top of work the DOSBox Team created.


## Current Known Issues and Limitations

* Really only tested working for Return to Zork (more compatibility to come)
* MPEG video decode is only about 95% complete. There are a few glitches here and there.
* Need to revisit architecture and approach for mixing VGA and MPEG.


## Running ReelMagic DOSBox

Runs just like normal DOSBox except that it supports ReelMagic MPEG games.
There are some optional configuration parameters. See `Configuration` section
under the `Changes to DOSBox` section below for more information on this.


## ReelMagic Driver API

All that has been discovered about how the ReelMagic driver works and interacts
with things is currently documented in the `RMDOS_API.md` file.

## ReelMagic Proprietary MPEG Format

Most ReelMagic MPEG file assets have been encoded in a non-standard way which
prevents them from playing in a standard MPEG media player such as VLC. In
order to get things properly working in DOSBox, I had to make a few changes
to the MPEG decoder. More information on this can be found in the
`NOTES_MPEG.md` file.


# Changes to DOSBox

Several modifications to DOSBox have been made to make this work.

## Configuration

A `[reelmagic]` configuration section has been added. It is not mandatory to
configure anything to have a working ReelMagic setup as the defaults will
enable ReelMagic emulation.
The parameters are:

* `enabled`        -- Enables/disables the ReelMagic emulator. By default this is `true`
* `alwaysresident` -- This forces `FMPDRV.EXE` to always be loaded.  By default this is `false`
* `vgadup5hack`    -- Duplicate every 5th VGA line to help give output a 4:3 ratio. By default this is `false`


For example:
```
[reelmagic]
enabled=true
alwaysresident=true
vgadup5hack=false
```

## Modified Files

The following existing DOSBox source code files have been modified for ReelMagic emulation functionality:

* `include/logging.h`                     -- Added `REELMAGIC` logging type + quick fix for variable length logging args
* `src/debug/debug_gui.cpp`               -- Added `REELMAGIC` logging type.
* `src/dosbox.cpp`                        -- ReelMagic init hook-in and config section.
* `src/hardware/Makefile.am`              -- Declared new ReelMagic *.cpp source code files

The following headers redirect all VGA output from DOSBox RENDER to ReelMagic:

* `src/hardware/vga_dac.cpp`              
* `src/hardware/vga_draw.cpp`             
* `src/hardware/vga_other.cpp`            


## New Files

The following new DOSBox source code files have been added for ReelMagic emulation functionality:

* `include/reelmagic.h`                   -- Header file for all ReelMagic stuff
* `include/vga_reelmagic_override.h`      -- Header file used to redirect all VGA output from DOSBox RENDER to ReelMagic
* `src/hardware/reelmagic_driver.cpp`     -- Implements the Driver + Hardware Emulation
* `src/hardware/reelmagic_pl_mpeg.cpp`    -- Modified version of PHOBOSLAB's `PL_MPEG` library found here: `https://github.com/phoboslab/pl_mpeg`
* `src/hardware/reelmagic_player.cpp`     -- Implements MPEG Media Decoder/Player Functionality
* `src/hardware/reelmagic_videomixer.cpp` -- Intercepts the VGA output and mixes in the decoded MPEG video.


# ReelMagic Emulator Architecture

ReelMagic emulation software components are wired up as such:
```
                             |-----------------|                                    |---------------|
                             |     PL_MPEG     |                                    |   Existing    |
                             | Decoder Library |                                    |    DOSBox     |
                             |-----------------|                                    |   "Render"    |
                                      ^                                             |---------------|
                                      |                        |-------------|              ^
                                      | Uses                   |             |              | Mixed VGA + MPEG
                                      |           Outputs      |   DOSBox    |              | Output Goes Here
|---------------|              |-------------|    Audio to     |    Mixer    |      |---------------|
|               |              |             | --------------->|             |      |               |
|   Driver +    |              |    MPEG     |                 |-------------|      |     Video     |
|   Hardware    | <----------> |  Player(s)  |                                      |    Underlay   |
|   Emulation   |   Controls   |             | -----------------------------------> |     Mixer     |
|               |              |-------------|               Outputs                |               |
|---------------|                                            Video to               |---------------|
        ^                                                                                   ^
        | Can                                                                               | Video Output
        | Use                                                                               | Intercepted By
|----------------|                                                                  |---------------|
|    Emulated    |                                                                  |   Existing    |
|  DOS Programs  |                                                                  |    DOSBox     |
|----------------|                                                                  |     VGA       |
                                                                                    |   Emulation   |
                                                                                    |---------------|
```





# Building DOSBox

## On Windows 10

See this guide: https://www.dosbox.com/wiki/Building_DOSBox_with_MinGW


## On Ubuntu 20.04
```
sudo apt install build-essential autoconf automake-1.15 autotools-dev m4
./autogen.sh
./configure
make
```

## Enabling DOSBox Debugger
```
./configure --enable-debug
-- OR --
./configure --enable-debug=heavy
```


# Online Resources

Here are some useful online resources:

## Information about ReelMagic

Here is a handful of links that helped me get started on the ReelMagic side of things:

* https://www.vogons.org/viewtopic.php?t=7364
* https://www.vogons.org/viewtopic.php?t=37363
* http://bitsavers.informatik.uni-stuttgart.de/pdf/c-cube/90-1450-101_CL450_MPEG_Video_Decoder_Users_Manual_1994.pdf
* http://www.oldlinux.org/Linux.old/docs/interrupts/int-html/rb-5094.htm
* http://www.oldlinux.org/Linux.old/docs/interrupts/int-html/rb-4251.htm


## PL ("Phobos Lab") MPEG Library

This is the awesome MPEG decoder library used for this project. This guy gets it.

* https://phoboslab.org/log/2019/06/pl-mpeg-single-file-library
* https://github.com/phoboslab/pl_mpeg

