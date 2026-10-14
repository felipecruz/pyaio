#!/bin/bash
# Run pytest in CI and surface the essentials as GitHub annotations, which
# are visible on the run page (and through the public API) without opening
# the logs: kernel, io_uring availability, and each failing test.
set -o pipefail
log="${RUNNER_TEMP:-/tmp}/pytest.log"

python - <<'PY'
import platform
try:
    disabled = open('/proc/sys/kernel/io_uring_disabled').read().strip()
except OSError:
    disabled = 'n/a'
try:
    from pyaio.uring import Ring
    Ring().close()
    state = 'available'
except OSError as e:
    state = 'unavailable: %s' % e
print('::notice title=environment::kernel %s, kernel.io_uring_disabled=%s, '
      'io_uring %s' % (platform.release(), disabled, state))
PY

python -m pytest -q -rfEs "$@" 2>&1 | tee "$log"
status=$?
grep -E '^(FAILED|ERROR) ' "$log" | head -20 | while IFS= read -r line; do
    echo "::error title=pytest::$line"
done
exit $status
