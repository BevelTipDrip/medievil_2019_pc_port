"""Runs a command, showing its output and writing it to a UTF-8 log, as the launcher does for
<saves folder>/last_run.log (run.py itself only prints). Exit code is the command's.

    py -3.14 tee_run.py <log> <command> [args...]
"""
import subprocess
import sys

log_path, command = sys.argv[1], sys.argv[2:]
with open(log_path, 'w', encoding='utf-8', buffering=1) as log:
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               stdin=subprocess.DEVNULL, text=True, encoding='utf-8', errors='replace')
    for line in process.stdout:
        sys.stdout.write(line)
        log.write(line)
    sys.exit(process.wait())
