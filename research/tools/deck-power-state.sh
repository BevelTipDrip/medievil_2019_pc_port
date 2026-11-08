#!/bin/bash
# The Steam Deck's power management as found (no root needed), run over SSH by deck.sh: `status` prints it
# and `steps` puts it at the top of its output. The same CPU work drew 15 W at a 60 cap in Launch 15 and
# 11.5 W on 2026-11-07, and nothing recorded why (plan-linux-deck.md "Cuts redo").
c=/sys/devices/system/cpu
d=$(ls -d /sys/class/drm/card[0-9]/device | head -1)
echo "boot $(uptime -s)"
echo "EPP $(sort $c/cpu*/cpufreq/energy_performance_preference | uniq -c | tr -s ' \n' ' ')"
echo "governor $(cat $c/cpu0/cpufreq/scaling_governor), CPU min/max $(cat $c/cpu0/cpufreq/scaling_min_freq)/$(cat $c/cpu0/cpufreq/scaling_max_freq) kHz (cpu0)"
echo "APU power cap $(cat /sys/class/hwmon/hwmon*/power1_cap 2>/dev/null | head -1) uW"
echo "GPU $(cat "$d/power_dpm_force_performance_level") $(grep '\*' "$d/pp_dpm_sclk" | tr -d '\n')"
if p=$(pgrep -o medievil-runti); then
    echo "game toggle file: $(tr '\0' '\n' < "/proc/$p/environ" | grep '^GOW3_TOGGLE_FILE=' | cut -d= -f2)"
fi
