echo "=== Running Terminator Step 1 Functional Tests ==="

set +e
./build/test_shell_runner << 'EOF'
echo Hello from Terminator shell
ls -d include src Makefile
notacommand
echo Last exit code was: $?
echo testing pipes | tr a-z A-Z
echo testing output redirection > test_file.txt
cat < test_file.txt
exit 42
EOF

STATUS=$?
echo "Terminator exited with code: $STATUS"

if [ "$STATUS" -eq 42 ]; then
    echo "=== ALL STEP 1 TESTS PASSED SUCCESSFULLY ==="
else
    echo "=== ERROR: Unexpected exit code $STATUS ==="
    exit 1
fi
