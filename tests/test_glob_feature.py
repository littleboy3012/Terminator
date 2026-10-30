import subprocess

p = subprocess.Popen(['./terminator'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
cmds = "cd src\necho \"*.c\"\necho '*.c'\nls *.c | wc -l\n"
out, err = p.communicate(cmds)
print("STDOUT:")
print(out)
if err:
    print("STDERR:")
    print(err)
