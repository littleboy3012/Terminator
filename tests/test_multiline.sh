#!/bin/bash
echo "=== Running Multiline & Substitution Tests ==="

./build/test_shell_runner << 'EOF'
echo "Line 1 continuation \
Line 2 of command"
echo "Unclosed double quotes
spanning two lines"
echo 'literal: $HOME' "expanded: $HOME"
echo Files count: $(ls | wc -l)
echo "Files in $(basename $(pwd)): $(ls | wc -l)"
exit 0
EOF

STATUS=$?
echo "Terminator exited with code: $STATUS"

if [ "$STATUS" -eq 0 ]; then
    echo "=== ALL MULTILINE TESTS PASSED ==="
else
    echo "=== MULTILINE TEST FAILED ==="
    exit 1
fi
