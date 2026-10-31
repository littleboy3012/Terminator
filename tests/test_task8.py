import subprocess
import time
import signal
import sys

def run_test(name, commands, sleep_before_exit=0.3, send_sig=None, sleep_after_sig=0.3):
    print(f"==================== {name} ====================")
    p = subprocess.Popen(
        ["./build/test_shell_runner"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    for cmd in commands:
        p.stdin.write(cmd + "\n")
        p.stdin.flush()
        time.sleep(0.2)

    if send_sig is not None:
        time.sleep(sleep_before_exit)
        p.send_signal(send_sig)
        time.sleep(sleep_after_sig)

    p.stdin.write("exit\n")
    p.stdin.flush()
    p.stdin.close()
    out, err = p.communicate(timeout=3)
    print("STDOUT:\n" + out)
    if err:
        print("STDERR:\n" + err)
    return out, err

out, _ = run_test("TEST 1: Background execution (cmd &) and jobs", ["sleep 10 &", "jobs"])
assert "[1] " in out, "Job ID not printed for background cmd"
assert "Running" in out, "Job state Running not shown in jobs"
assert "sleep 10" in out, "Command name not in jobs"
print(">>> TEST 1 PASSED!\n")

out, _ = run_test("TEST 2: Background completion notification", ["sleep 1 &"], sleep_before_exit=1.2)
p2 = subprocess.Popen(["./build/test_shell_runner"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
p2.stdin.write("sleep 1 &\n")
p2.stdin.flush()
time.sleep(1.2)
p2.stdin.write("echo check\n")
p2.stdin.flush()
time.sleep(0.2)
p2.stdin.write("exit\n")
p2.stdin.flush()
p2.stdin.close()
out2, _ = p2.communicate(timeout=3)
print("TEST 2 OUTPUT:\n" + out2)
assert "Done" in out2, "Done notification not displayed"
print(">>> TEST 2 PASSED!\n")

# TEST 3: Ctrl+Z (suspend) and jobs
p3 = subprocess.Popen(["./build/test_shell_runner"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
p3.stdin.write("sleep 50\n")
p3.stdin.flush()
time.sleep(0.3)
p3.send_signal(signal.SIGTSTP)
time.sleep(0.3)
p3.stdin.write("jobs\n")
p3.stdin.flush()
time.sleep(0.2)
p3.stdin.write("exit\n")
p3.stdin.flush()
p3.stdin.close()
out3, _ = p3.communicate(timeout=3)
print("TEST 3 OUTPUT:\n" + out3)
assert "Stopped" in out3, "Job not marked as Stopped after SIGTSTP"
print(">>> TEST 3 PASSED!\n")

# TEST 4: bg %1 command
p4 = subprocess.Popen(["./build/test_shell_runner"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
p4.stdin.write("sleep 50\n")
p4.stdin.flush()
time.sleep(0.3)
p4.send_signal(signal.SIGTSTP)
time.sleep(0.3)
p4.stdin.write("bg %1\n")
p4.stdin.flush()
time.sleep(0.2)
p4.stdin.write("jobs\n")
p4.stdin.flush()
time.sleep(0.2)
p4.stdin.write("exit\n")
p4.stdin.flush()
p4.stdin.close()
out4, _ = p4.communicate(timeout=3)
print("TEST 4 OUTPUT:\n" + out4)
assert "[1]+ sleep 50 &" in out4, "bg output not formatted properly"
assert "Running" in out4, "Job not running after bg"
print(">>> TEST 4 PASSED!\n")

# TEST 5: fg %1 command and Ctrl+C
p5 = subprocess.Popen(["./build/test_shell_runner"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
p5.stdin.write("sleep 50 &\n")
p5.stdin.flush()
time.sleep(0.2)
p5.stdin.write("fg %1\n")
p5.stdin.flush()
time.sleep(0.3)
p5.send_signal(signal.SIGINT)
time.sleep(0.3)
p5.stdin.write("jobs\n")
p5.stdin.flush()
time.sleep(0.2)
p5.stdin.write("exit\n")
p5.stdin.flush()
p5.stdin.close()
out5, _ = p5.communicate(timeout=3)
print("TEST 5 OUTPUT:\n" + out5)
assert "sleep 50 &" not in out5.split("jobs")[-1], "Job was not terminated by SIGINT in foreground"
print(">>> TEST 5 PASSED!\n")

print("ALL TASK 8 TESTS COMPLETED SUCCESSFULLY!")
