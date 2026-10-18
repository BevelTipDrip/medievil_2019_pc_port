"""Runs a command, showing its output and writing it to a UTF-8 log, as the launcher does for
<saves folder>/last_run.log (run.py itself only prints). Exit code is the command's.
<log>.times gets "<line number> <local time>" per line, to line the log up with traces (the log
itself stays as the game printed it).

    py -3.14 tee_run.py <log> <command> [args...]
"""
import datetime
import subprocess
import sys

log_path, command = sys.argv[1], sys.argv[2:]
with open(log_path, 'w', encoding='utf-8', buffering=1) as log, \
        open(log_path + '.times', 'w', encoding='utf-8', buffering=1) as times:
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               stdin=subprocess.DEVNULL, text=True, encoding='utf-8', errors='replace')
    for number, line in enumerate(process.stdout, 1):
        sys.stdout.write(line)
        log.write(line)
        times.write(f'{number} {datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]}\n')
    sys.exit(process.wait())
