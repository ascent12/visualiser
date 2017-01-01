#!/bin/sh

ffmpeg -i "$1" -sample_fmt s16 -ar 44100 -map_metadata -1 -fflags +bitexact "$2"
