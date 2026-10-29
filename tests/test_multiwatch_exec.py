import subprocess
import time
import signal
import sys

proc = subprocess.Popen(
    ["./terminator", "--cli"],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True
)

# Send multiWatch command
proc.stdin.write('multiWatch ["date +%T", "ls | wc -l"]\n')
proc.stdin.flush()

# Let it run for 3 seconds so both commands run and output
time.sleep(3)

# Send SIGINT (Ctrl+C)
proc.send_signal(signal.SIGINT)
time.sleep(0.5)

# Then send exit
try:
    proc.stdin.write("exit\n")
    proc.stdin.flush()
except Exception:
    pass

stdout, stderr = proc.communicate(timeout=3)
print("=== STDOUT ===")
print(stdout)
print("=== STDERR ===")
print(stderr)
