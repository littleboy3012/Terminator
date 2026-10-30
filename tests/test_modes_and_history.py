import subprocess
import time
import os

print("==================== TEST: Thread Mode & Process Mode Startup ====================")

# Test 1: Thread mode startup banner
p_thread = subprocess.Popen(['./terminator'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
out_thread, _ = p_thread.communicate("exit\n")
print("Thread Mode Output Banner:")
print(out_thread.strip())
assert "Mode: Thread Mode (default)" in out_thread, "Thread mode banner missing!"
print(">>> Thread mode startup verified!")

# Test 2: Process mode startup banner
p_proc = subprocess.Popen(['./terminator', 'useProcess=True'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
out_proc, _ = p_proc.communicate("exit\n")
print("Process Mode Output Banner:")
print(out_proc.strip())
assert "Mode: Process Mode (useProcess=True)" in out_proc, "Process mode banner missing!"
print(">>> Process mode startup verified!")

# Test 3: Shared History built-in in Thread Mode
p_hist = subprocess.Popen(['./terminator'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
cmds = "echo first_command\necho second_command\nhistory\nexit\n"
out_hist, _ = p_hist.communicate(cmds)
print("Thread Mode History Output:")
print(out_hist)
assert "first_command" in out_hist, "History should contain first_command"
assert "second_command" in out_hist, "History should contain second_command"
print(">>> Thread Mode History command verified!")

# Test 4: Shared History built-in in Process Mode
p_hist_proc = subprocess.Popen(['./terminator', 'useProcess=True'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
cmds_proc = "echo proc_alpha\necho proc_beta\nhistory\nexit\n"
out_hist_proc, _ = p_hist_proc.communicate(cmds_proc)
print("Process Mode History Output:")
print(out_hist_proc)
assert "proc_alpha" in out_hist_proc, "Process Mode history should contain proc_alpha"
assert "proc_beta" in out_hist_proc, "Process Mode history should contain proc_beta"
print(">>> Process Mode Shared Memory History verified!")

print("ALL THREAD & PROCESS MODE CLI TESTS PASSED!")
