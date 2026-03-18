# TG11 shell demo for terminal visuals + CP437 aliases
# Run with: run c64-demo.sh

echo --- C64 style demo start ---
theme c64
color show

echo Drawing a box with aliases:
echo \boxul\boxh\boxh\boxh\boxh\boxh\boxh\boxh\boxh\boxur
echo \boxv TG11 C64 THEME \boxv
echo \boxll\boxh\boxh\boxh\boxh\boxh\boxh\boxh\boxh\boxlr

echo Blocks and shades:
echo \blk\blk\blk  \shade1\shade2\shade3

echo Raw glyph bytes:
glyph 0xDB
glyph 0xB0
glyph 0xC4

echo Alias help:
charmap

echo FAT mount cycle demo:
fatmount
pwd
ls
fatunmount

theme default
color text 0x0F
color prompt 0x0B
echo --- C64 style demo end ---
