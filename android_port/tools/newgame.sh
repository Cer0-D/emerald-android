#!/usr/bin/env bash
# Drives a fresh start of the app to the overworld (Littleroot, after the
# truck) with injected button presses, for testing over adb. Keys go to the
# top display (-d 0): the Thor's dual-screen assistant keeps focus on display 4.
# Progress is read from the game's log (its periodic "map=group/num" line).
set -euo pipefail
K() { adb shell input -d 0 keyevent --longpress "$1"; sleep "${2:-1}"; }
on_map() { adb logcat -d -s Emerald:* | grep -q "map=$1"; }

adb shell am force-stop com.cer0.emerald.dev
adb logcat -c
adb shell am start -n com.cer0.emerald.dev/com.cer0.emerald.MainActivity >/dev/null
sleep 4
K KEYCODE_BUTTON_START 3      # skip the intro
K KEYCODE_BUTTON_START 4      # title -> main menu
K KEYCODE_BUTTON_A 3          # NEW GAME
named=0
for i in $(seq 1 80); do
    # The naming screen: one letter, then OK.
    if [[ $named == 0 && $i -gt 25 ]] && adb logcat -d -s Emerald:* | grep -q "CENTRED_NAMING\|naming"; then
        named=1
    fi
    K KEYCODE_BUTTON_A 1.0
    if (( i == 30 )); then K KEYCODE_BUTTON_START 0.8; K KEYCODE_BUTTON_A 1.5; fi
    if on_map "25/40"; then echo "newgame: in the truck"; break; fi
done
sleep 14                      # the truck ride
for _ in 1 2 3; do K KEYCODE_DPAD_RIGHT 0.4; done
for _ in 1 2 3 4; do K KEYCODE_BUTTON_A 1.0; done
echo "newgame: done"
