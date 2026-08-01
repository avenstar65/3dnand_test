import select
import threading
from collections import deque


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


def read_response(stream, command, timeout, stderr_capture):
    ready, _, _ = select.select([stream], [], [], timeout)
    if not ready:
        raise TimeoutError("qtest timeout waiting for {!r}; stderr: {}".format(
            command, stderr_capture.text()))
    response = stream.readline()
    if isinstance(response, bytes):
        response = response.decode("utf-8", "replace")
    response = response.strip()
    if not response:
        raise RuntimeError("qtest closed while processing {!r}; stderr: {}".format(
            command, stderr_capture.text()))
    return response
