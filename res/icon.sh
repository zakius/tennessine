#!/bin/sh
# Redraws one of Chromium's app icon PNGs from res/app_icon/<its name>.svg, at the PNG's own size.
# tools/app_icon.py writes the SVGs. Needs imagemagick and librsvg, which the build image has.
svg=$(dirname "$0")/app_icon/$(basename "$1" .png).svg
w=$(identify -format %w "$1")
rsvg-convert -w "$w" -h "$w" -o "$1" "$svg"
echo "$1 (${w}px)"
