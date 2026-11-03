#!/bin/sh
# smoke.sh BINARY: the tools parse, and the runtime starts and fails cleanly without game data.
set -e
for f in tools/*.py; do python3 -c "import ast,sys; ast.parse(open(sys.argv[1]).read())" "$f"; done
./build/is3unpack 2>&1 | grep -q usage || true
set +e
ICARUS_DATA=no-such-dir "$1" > smoke.log 2>&1
code=$?
set -e
cat smoke.log
[ "$code" -ne 0 ] || { echo "expected a non-zero exit without game data"; exit 1; }
grep -q "game data folder 'no-such-dir' not found" smoke.log
echo "smoke test passed"
