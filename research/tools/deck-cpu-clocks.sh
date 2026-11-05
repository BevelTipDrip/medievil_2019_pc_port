#!/bin/bash
# Per-core CPU clock settings for Steam Deck tests (amd-pstate-epp). Copied to the Deck by
# `deck.sh cpu-script`; the user runs it with sudo in a terminal (needs root; resets at reboot).
# Leave the Quick Access Menu's manual CPU clock off while testing: it writes the same files.
#
#   sudo ~/MediEvil/deck-cpu-clocks.sh show              governor, EPP, min / max clock per CPU
#   sudo ~/MediEvil/deck-cpu-clocks.sh save              keep the current values (done by perf / split)
#   sudo ~/MediEvil/deck-cpu-clocks.sh perf              every CPU: EPP performance, full clock
#   sudo ~/MediEvil/deck-cpu-clocks.sh split [MHz] [cpu] one CPU (default 6: the launcher's dedicated
#                                                         core for stage B) at EPP performance and full
#                                                         clock; every other CPU capped at MHz (default
#                                                         2000) with its EPP unchanged, to leave power
#                                                         budget to the GPU
#   sudo ~/MediEvil/deck-cpu-clocks.sh restore           back to the saved values
set -euo pipefail
saved=/home/deck/MediEvil/cpu-clocks.saved
cpus=$(ls -d /sys/devices/system/cpu/cpu[0-9]* | sort -V)

show() {
    printf '%-6s %-12s %-22s %8s %8s %8s\n' cpu governor EPP min_MHz max_MHz now_MHz
    for c in $cpus; do
        f=$c/cpufreq
        printf '%-6s %-12s %-22s %8d %8d %8d\n' "${c##*/}" "$(cat $f/scaling_governor)" \
            "$(cat $f/energy_performance_preference 2>/dev/null || echo -)" \
            $(( $(cat $f/scaling_min_freq) / 1000 )) $(( $(cat $f/scaling_max_freq) / 1000 )) \
            $(( $(cat $f/scaling_cur_freq) / 1000 ))
    done
    echo "hardware max: $(( $(cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq) / 1000 )) MHz"
}

save() {
    [ -f "$saved" ] && { echo "already saved in $saved (restore first to save again)"; return; }
    for c in $cpus; do
        f=$c/cpufreq
        echo "${c##*/} $(cat $f/scaling_governor) $(cat $f/energy_performance_preference 2>/dev/null || echo -) $(cat $f/scaling_min_freq) $(cat $f/scaling_max_freq)"
    done > "$saved"
    chown deck: "$saved"
    echo "saved to $saved"
}

[ "$(id -u)" = 0 ] || [ "${1:-show}" = show ] || { echo 'run with sudo'; exit 1; }
case ${1:-show} in
show) show ;;
save) save ;;
perf)
    save
    for c in $cpus; do
        f=$c/cpufreq
        cat $f/cpuinfo_max_freq > $f/scaling_max_freq
        echo performance > $f/energy_performance_preference
    done
    show ;;
split)
    mhz=${2:-2000}; own=cpu${3:-6}
    save
    for c in $cpus; do
        f=$c/cpufreq
        if [ "${c##*/}" = "$own" ]; then
            cat $f/cpuinfo_max_freq > $f/scaling_max_freq
            echo performance > $f/energy_performance_preference
        else
            echo $(( mhz * 1000 )) > $f/scaling_max_freq
        fi
    done
    show ;;
restore)
    [ -f "$saved" ] || { echo "nothing saved in $saved"; exit 1; }
    while read -r cpu gov epp min max; do
        f=/sys/devices/system/cpu/$cpu/cpufreq
        echo "$gov" > $f/scaling_governor
        [ "$epp" != - ] && echo "$epp" > $f/energy_performance_preference
        echo "$min" > $f/scaling_min_freq
        echo "$max" > $f/scaling_max_freq
    done < "$saved"
    rm -f "$saved"
    show ;;
*) sed -n 2,16p "$0"; exit 1 ;;
esac
