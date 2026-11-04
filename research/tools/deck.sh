#!/usr/bin/env bash
# Steam Deck test loop over SSH (plan-linux-deck.md "Test settings by file"). Run from Git Bash on the
# PC; needs the `deck` host in ~/.ssh/config and the WSL build tree. Never launches the game: the user
# presses Play (every launch needs the user's go).
#
#   deck.sh status                         game running?, deployed build, test settings, toggles, clocks
#   deck.sh deploy [wsl_tree]              copy the WSL AppImage to the Deck (game must be closed); the
#                                          previous one is kept as MediEvil-x86_64.AppImage.<its commit>
#   deck.sh env "comment" KEY=VAL ...      write ~/.local/share/medievil-pc/extra-env.txt
#   deck.sh toggle "W1 W2"                 write the toggle file (GOW3_TOGGLE_FILE) now
#   deck.sh ab "A" "B" [period] [cycles]   alternate the toggle file B, A, B, A ... (default 30 s, 5 cycles;
#                                          ends on A); the game must be running
#   deck.sh pull NAME                      copy the game log to research/docs/data/NAME.log
#   deck.sh summary NAME|PATH [args]       per-window and per-toggle-state summary (deck_summary.py)
#   deck.sh threads [seconds]              per-thread CPU and context switches during a run (/proc)
#   deck.sh checks                         read-only platform facts (study-deck-platform-optimization.md §0)
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
repo=$(cd -- "$here/../.." && pwd)
data_dir=.local/share/medievil-pc
toggle_file=/home/deck/$data_dir/toggles.txt
remote() { ssh -o ConnectTimeout=10 deck "$@"; }
running() { remote 'pgrep -x medievil-runtim >/dev/null'; }

cmd=${1:-status}
shift || true
case $cmd in
status)
    remote "pgrep -x medievil-runtim >/dev/null && echo 'game: RUNNING' || echo 'game: not running'
        echo \"deployed: \$(cat ~/MediEvil/DEPLOYED 2>/dev/null || echo unknown) \$(sha256sum ~/MediEvil/MediEvil-x86_64.AppImage | cut -c1-16)\"
        echo '--- extra-env.txt'; cat ~/$data_dir/extra-env.txt 2>/dev/null
        echo \"--- toggles: \$(cat $toggle_file 2>/dev/null)\"
        echo \"--- launcher: \$(grep -o '\"prep_workers\": \"[0-9]*\"\|\"stage_b_core\": [a-z]*' ~/.config/medievil-launcher/settings.json | tr '\n' ' ')\"
        echo \"--- ini: \$(grep -E '^(render_scale|vsync|fps_limit|ue4_quality|frames_queued)=' ~/$data_dir/medievil.ini | tr '\n' ' ')\"
        d=\$(ls -d /sys/class/drm/card*/device | head -1)
        echo \"--- GPU: \$(cat \$d/power_dpm_force_performance_level) \$(grep '\\*' \$d/pp_dpm_sclk)\""
    ;;
deploy)
    tree=${1:-/root/src/medievil}
    running && { echo 'The game is running on the Deck: not deploying.' >&2; exit 1; }
    commit=$(MSYS_NO_PATHCONV=1 wsl -d Debian -u root -- git -C "$tree" rev-parse --short HEAD | tr -d '\r\0')
    old=$(remote 'cat ~/MediEvil/DEPLOYED 2>/dev/null || echo previous')
    remote "cp ~/MediEvil/MediEvil-x86_64.AppImage ~/MediEvil/MediEvil-x86_64.AppImage.$old"
    MSYS_NO_PATHCONV=1 wsl -d Debian -u root -- cat "$tree/dist/MediEvil-x86_64.AppImage" |
        remote "pgrep -x medievil-runtim >/dev/null && exit 1
            cat > ~/MediEvil/MediEvil-x86_64.AppImage.new && chmod +x ~/MediEvil/MediEvil-x86_64.AppImage.new &&
            mv ~/MediEvil/MediEvil-x86_64.AppImage.new ~/MediEvil/MediEvil-x86_64.AppImage && echo $commit > ~/MediEvil/DEPLOYED"
    local_sum=$(MSYS_NO_PATHCONV=1 wsl -d Debian -u root -- sha256sum "$tree/dist/MediEvil-x86_64.AppImage" | cut -c1-16)
    deck_sum=$(remote 'sha256sum ~/MediEvil/MediEvil-x86_64.AppImage' | cut -c1-16)
    [[ $local_sum == "$deck_sum" ]] || { echo "Checksum mismatch: $local_sum vs $deck_sum" >&2; exit 1; }
    echo "Deployed $commit ($deck_sum); previous kept as MediEvil-x86_64.AppImage.$old"
    ;;
env)
    comment=${1:?comment}; shift
    { echo '# Test settings, read by run.py on every start (written by Claude over SSH).'; echo "# $comment"
      for kv in "$@"; do echo "$kv"; done; } | remote "cat > ~/$data_dir/extra-env.txt && cat ~/$data_dir/extra-env.txt"
    ;;
toggle)
    remote "echo '${1:?\"W1 W2\"}' > $toggle_file && cat $toggle_file"
    ;;
ab)
    a=${1:?A}; b=${2:?B}; period=${3:-30}; cycles=${4:-5}
    running || { echo 'The game is not running on the Deck.' >&2; exit 1; }
    remote "for i in \$(seq $cycles); do echo '$b' > $toggle_file; echo \"\$(date +%T) $b\"; sleep $period
        echo '$a' > $toggle_file; echo \"\$(date +%T) $a\"; sleep $period; done; echo \"\$(date +%T) done (left at $a)\""
    ;;
pull)
    name=${1:?NAME}
    remote "cat ~/$data_dir/user/last_run.log" > "$repo/research/docs/data/$name.log"
    echo "research/docs/data/$name.log ($(wc -l < "$repo/research/docs/data/$name.log") lines)"
    ;;
summary)
    target=${1:?NAME or path}; shift
    [[ -f $target ]] || target=$repo/research/docs/data/$target.log
    py -3.14 "$here/deck_summary.py" "$target" "$@"
    ;;
threads)
    seconds=${1:-3}
    remote "p=\$(pgrep -x medievil-runtim | head -1); [ -z \"\$p\" ] && { echo 'game not running'; exit 1; }
        snap(){ for t in /proc/\$p/task/*; do n=\$(cat \$t/comm 2>/dev/null); s=(\$(cut -d' ' -f14,15 \$t/stat 2>/dev/null))
            v=\$(grep -E '^(voluntary|nonvoluntary)_ctxt_switches' \$t/status 2>/dev/null | awk '{print \$2}' | tr '\n' ' ')
            echo \"\${t##*/} \${s[0]:-0} \${s[1]:-0} \$v \$n\"; done; }
        snap > /tmp/deck-s1; sleep $seconds; snap > /tmp/deck-s2
        echo 'CPU% (of one CPU)  voluntary/s involuntary/s  thread'
        join <(sort /tmp/deck-s1) <(sort /tmp/deck-s2) | awk -v s=$seconds '{printf \"%5.0f%%  %7d %7d  %s\\n\", (\$7+\$8-\$2-\$3)/s, (\$9-\$4)/s, (\$10-\$5)/s, \$6}' | sort -rn | head -24
        rm -f /tmp/deck-s1 /tmp/deck-s2
        echo \"CPU MHz: \$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq | awk '{printf \"%d \", \$1/1000}')\""
    ;;
checks)
    remote 'cd /sys/kernel/mm/transparent_hugepage 2>/dev/null && grep . enabled defrag shmem_enabled khugepaged/defrag 2>/dev/null
        echo "mTHP sizes: $(ls /sys/kernel/mm/transparent_hugepage/ 2>/dev/null | grep hugepages- | tr "\n" " ")"
        echo "cpufreq: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver) / $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
        echo "amd_pstate: $(cat /sys/devices/system/amd_pstate/status 2>/dev/null) EPP: $(cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference 2>/dev/null)"
        echo "sched_ext: $(cat /sys/kernel/sched_ext/state 2>/dev/null) $(cat /sys/kernel/sched_ext/root/ops 2>/dev/null)"
        systemctl list-units "scx*" --no-pager --plain 2>/dev/null | head -3
        echo "perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid)  max_map_count: $(cat /proc/sys/vm/max_map_count)  RLIMIT_NICE: $(ulimit -e)"
        echo "SMT: $(cat /sys/devices/system/cpu/smt/control)"
        echo "tools: $(which gamemoderun gamemoded scx_lavd perf 2>/dev/null | tr "\n" " ")"
        pacman -Q gamemode scx-scheds 2>/dev/null
        d=$(ls -d /sys/class/drm/card*/device | head -1); echo "fclk:"; cat $d/pp_dpm_fclk; echo "sclk:"; cat $d/pp_dpm_sclk; cat $d/power_dpm_force_performance_level
        pid=$(pgrep -o medievil-runti)
        if [ -n "$pid" ]; then
            echo "--- running game $pid"
            tr "\0" "\n" < /proc/$pid/environ | grep -i -E "gamescope|wsi|vk_|mesa|radv|glibc|ld_preload"
            echo "WSI layers: $(grep -i -E "gamescope|wsi" /proc/$pid/maps | awk "{print \$6}" | sort -u | tr "\n" " ")"
            grep -E "AnonHugePages|ShmemPmdMapped|FilePmdMapped|Rss" /proc/$pid/smaps_rollup; echo "maps: $(wc -l < /proc/$pid/maps)"
        fi'
    ;;
*)
    sed -n 2,17p "$0"; exit 1
    ;;
esac
