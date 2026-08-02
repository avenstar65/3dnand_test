import os
import signal
import subprocess
import tempfile
import time

from q3n_qtest_protocol import cleanup_qtest


def media_path():
    handle = tempfile.NamedTemporaryFile(prefix="q3n-qtest-cleanup-", delete=False)
    handle.close()
    return handle.name


path = media_path()
class ForcedTimeout:
    returncode = None

    def poll(self):
        return None

    def send_signal(self, _signal):
        pass

    def wait(self, timeout):
        if self.returncode is None:
            raise subprocess.TimeoutExpired("fixture", timeout)
        return self.returncode

    def kill(self):
        self.returncode = -signal.SIGKILL


proc = ForcedTimeout()
errors = cleanup_qtest(proc, None, path, timeout=0.01)
assert not errors, errors
assert proc.returncode == -signal.SIGKILL, proc.returncode
assert not os.path.exists(path), path


class SignalFailure:
    returncode = None

    def poll(self):
        return None

    def send_signal(self, _signal):
        raise OSError("forced signal failure")

    def kill(self):
        self.returncode = -signal.SIGKILL

    def wait(self, _timeout):
        return self.returncode


path = media_path()
errors = cleanup_qtest(SignalFailure(), None, path, timeout=0.01)
assert any("send_signal" in error for error in errors), errors
assert not os.path.exists(path), path


class KillFailureAfterTimeout:
    returncode = None

    def poll(self):
        return None

    def send_signal(self, _signal):
        pass

    def wait(self, timeout):
        raise subprocess.TimeoutExpired("fixture", timeout)

    def kill(self):
        raise OSError("forced kill failure")


path = media_path()
start = time.monotonic()
errors = cleanup_qtest(KillFailureAfterTimeout(), None, path, timeout=0.01)
assert time.monotonic() - start < 1
assert any("kill: forced kill failure" in error for error in errors), errors
assert not os.path.exists(path), path


class JoinFailure:
    def join(self):
        raise OSError("forced join failure")


path = media_path()
start = time.monotonic()
errors = cleanup_qtest(None, JoinFailure(), path, timeout=0.01)
assert time.monotonic() - start < 1
assert any("stderr join: forced join failure" in error for error in errors), errors
assert not os.path.exists(path), path

print("ok: Q3N qtest cleanup removes media after timeout and teardown errors")
