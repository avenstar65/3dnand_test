import subprocess
import sys
import time

from q3n_qtest_protocol import MAX_RESPONSE_BYTES, StderrCapture, read_response

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


proc = subprocess.Popen([
    sys.executable, "-c",
    "import sys,time; sys.stderr.write('partial-stderr\\n'); "
    "sys.stderr.flush(); sys.stdout.write('X'); sys.stdout.flush(); time.sleep(3)"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    bufsize=0)
capture = StderrCapture(proc.stderr)
start = time.monotonic()
try:
    try:
        read_response(proc.stdout, "readl 0x1000", 0.2, capture)
    except TimeoutError as error:
        elapsed = time.monotonic() - start
        message = str(error)
        assert elapsed < 2, elapsed
        assert "readl 0x1000" in message, message
        assert "partial-stderr" in message, message
        assert "partial" in message, message
    else:
        raise AssertionError("partial qtest response did not time out")
finally:
    proc.terminate()
    proc.wait(timeout=5)
    capture.join()

print("ok: partial Q3N qtest response cannot block past its deadline")


proc = subprocess.Popen([
    sys.executable, "-c",
    "import sys; sys.stdout.buffer.write(b'X' * ({0} + 1) + b'\\n'); "
    "sys.stdout.flush()".format(MAX_RESPONSE_BYTES)],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    bufsize=0)
capture = StderrCapture(proc.stderr)
try:
    try:
        read_response(proc.stdout, "oversized", 1, capture)
    except RuntimeError as error:
        assert "exceeds" in str(error), error
    else:
        raise AssertionError("oversized qtest response was accepted")
finally:
    proc.wait(timeout=5)
    capture.join()

print("ok: oversized Q3N qtest response is rejected")
