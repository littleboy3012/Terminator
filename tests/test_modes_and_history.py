import subprocess
import time
import os

print("==================== TEST: Thread Mode & Process Mode Startup ====================")

# Test 1: Thread mode startup banner
proc1 = subprocess.Popen(['timeout', '1', './terminator'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
out1, _ = proc1.communicate()
print("Thread Mode Output Banner:")
print(out1.strip())
assert "Mode: Thread Mode (default)" in out1, "Thread mode banner missing!"
print(">>> Thread mode startup verified!")

# Test 2: Process mode startup banner
proc2 = subprocess.Popen(['timeout', '1', './terminator', 'useProcess=True'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
out2, _ = proc2.communicate()
print("Process Mode Output Banner:")
print(out2.strip())
assert "Mode: Process Mode (useProcess=True)" in out2, "Process mode banner missing!"
print(">>> Process mode startup verified!")

# Test 3: Concurrency and Shared Memory across Threads and Processes
ret = subprocess.call(['bash', '-c', 'gcc -Wall -Wextra -Iinclude tests/test_tabs_thread_process.c src/history.c -o build/test_tabs -lpthread -lrt && ./build/test_tabs'])
assert ret == 0, "Thread & Process concurrency test failed!"

print("ALL THREAD & PROCESS MODE TESTS PASSED SUCCESSFULLY!")
