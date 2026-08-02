import os
import select
import signal
import subprocess
import threading
import time
from collections import deque


MAX_RESPONSE_BYTES = 64 * 1024


class ResponseBuffer:
    def __init__(self):
        self.data = bytearray()


class StderrCapture:
    def __init__(self, stream):
        self._lines = deque(maxlen=32)
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._drain,
                                        args=(stream,), daemon=True)
        self._thread.start()

    def _drain(self, stream):
        while True:
            line = stream.readline()
            if not line:
                return
            if isinstance(line, bytes):
                line = line.decode("utf-8", "replace")
            with self._lock:
                self._lines.append(line)

    def text(self):
        with self._lock:
            return "".join(self._lines).strip() or "<no stderr output>"

    def join(self):
        self._thread.join(timeout=1)


def _diagnostic(command, stderr_capture, data):
    partial = "<none>"
    if data:
        partial = repr(bytes(data[-256:]).decode("utf-8", "replace"))
    return "command {!r}; partial response: {}; stderr: {}".format(
        command, partial, stderr_capture.text())


def read_response(stream, command, timeout, stderr_capture, response_buffer=None):
    """Return one newline-terminated qtest response before *timeout* expires."""
    if response_buffer is None:
        response_buffer = ResponseBuffer()
    data = response_buffer.data
    fd = stream.fileno()
    os.set_blocking(fd, False)
    deadline = time.monotonic() + timeout

    while True:
        newline = data.find(b"\n")
        if newline >= 0:
            response = bytes(data[:newline])
            del data[:newline + 1]
            response = response.decode("utf-8", "replace").strip()
            if not response:
                raise RuntimeError("qtest closed while processing {}".format(
                    _diagnostic(command, stderr_capture, data)))
            return response
        if len(data) > MAX_RESPONSE_BYTES:
            raise RuntimeError("qtest response exceeds {} bytes while processing {}".format(
                MAX_RESPONSE_BYTES, _diagnostic(command, stderr_capture, data)))

        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("qtest timeout waiting for {}".format(
                _diagnostic(command, stderr_capture, data)))
        ready, _, _ = select.select([fd], [], [], remaining)
        if not ready:
            raise TimeoutError("qtest timeout waiting for {}".format(
                _diagnostic(command, stderr_capture, data)))
        try:
            chunk = os.read(fd, min(4096, MAX_RESPONSE_BYTES - len(data) + 1))
        except BlockingIOError:
            continue
        if not chunk:
            raise RuntimeError("qtest closed while processing {}".format(
                _diagnostic(command, stderr_capture, data)))
        data.extend(chunk)


def _record(errors, action, operation):
    try:
        operation()
        return True
    except Exception as error:
        errors.append("{}: {}".format(action, error))
        return False


def teardown_qtest(proc, stderr_capture, timeout=10):
    errors = []
    if proc is not None and proc.poll() is None:
        sent = _record(errors, "send_signal", lambda: proc.send_signal(signal.SIGINT))
        if sent:
            try:
                proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                if _record(errors, "kill", proc.kill):
                    _record(errors, "wait after kill",
                            lambda: proc.wait(timeout=timeout))
            except Exception as error:
                errors.append("wait: {}".format(error))
        elif _record(errors, "kill after signal failure", proc.kill):
            _record(errors, "wait after signal failure",
                    lambda: proc.wait(timeout=timeout))
    if stderr_capture is not None:
        _record(errors, "stderr join", stderr_capture.join)
    return errors


def cleanup_qtest(proc, stderr_capture, media_path, timeout=10):
    """Best-effort qtest shutdown which never lets teardown skip media cleanup."""
    errors = []
    try:
        try:
            errors.extend(teardown_qtest(proc, stderr_capture, timeout))
        except Exception as error:
            errors.append("teardown: {}".format(error))
    finally:
        if media_path is not None and os.path.exists(media_path):
            _record(errors, "media unlink", lambda: os.unlink(media_path))
    return errors
