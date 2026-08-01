import subprocess
import sys
import time

from q3n_qtest_protocol import StderrCapture, read_response

proc = subprocess.Popen([
    sys.executable, "-c",
    "import sys,time; sys.stderr.write('fixture-stderr\\n'); "
    "sys.stderr.flush(); time.sleep(30)"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    text=True)
capture = StderrCapture(proc.stderr)
start = time.monotonic()
try:
    try:
        read_response(proc.stdout, "writel 0x10 0x9", 0.2, capture)
    except TimeoutError as error:
        elapsed = time.monotonic() - start
        message = str(error)
        assert elapsed < 2, elapsed
        assert "writel 0x10 0x9" in message, message
        assert "fixture-stderr" in message, message
    else:
        raise AssertionError("nonresponsive qtest fixture did not time out")
finally:
    proc.terminate()
    proc.wait(timeout=5)
    capture.join()

print("ok: Q3N qtest protocol timeout is bounded and diagnostic")
