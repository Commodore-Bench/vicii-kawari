#!/bin/sh
. ./config.sh
NAME=Trion6567R8
xrandr --delmode $OUTPUT $NAME
xrandr --newmode $NAME 26.59 776 779 843 845 506 508 524 526 -hsync -vsync
xrandr --addmode $OUTPUT $NAME
xrandr --output $OUTPUT --mode $NAME
