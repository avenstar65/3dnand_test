#!/usr/bin/env python3
"""Read a marked multi-plane group through QEMU MMIO, outside NAND Core."""
import argparse
import base64
import hashlib
import signal
import subprocess

from q3n_qtest_protocol import ResponseBuffer, StderrCapture, read_response, teardown_qtest

parser = argparse.ArgumentParser()
parser.add_argument("--qemu", required=True)
parser.add_argument("--image", required=True)
parser.add_argument("--logical-block", required=True, type=int)
parser.add_argument("--expected-erased-digest", required=True)
args = parser.parse_args()

BAR = 0xfebe0000
PCI_CONFIG = 0x80002000
REG_CMD = 0x10
REG_LEN = 0x1c
REG_READ_FLAGS = 0xbc
REG_MP_DIE = 0xc4
REG_MP_BLOCK = 0xc8
REG_MP_PAGE = 0xcc
REG_MP_DONE_MASK = 0xd0
REG_MP_FAIL_MASK = 0xd4
REG_DATA = 0x1000
CMD_MP_READ_PAGE = 9
MP_MAIN_SIZE = 65536
CHUNK_SIZE = 16384

if args.logical_block < 0 or args.logical_block >= 416:
    raise SystemExit("logical block outside multi-plane geometry")
if (len(args.expected_erased_digest) != 64 or
        any(c not in "0123456789abcdef" for c in args.expected_erased_digest)):
    raise SystemExit("invalid expected erased digest")

proc = None
capture = None
buffer = ResponseBuffer()
try:
    proc = subprocess.Popen([
        args.qemu, "-machine", "q35", "-nodefaults", "-nographic",
        "-accel", "qtest", "-qtest", "stdio",
        "-blockdev", "driver=file,filename=" + args.image + ",node-name=q3nfile",
        "-blockdev", "driver=raw,file=q3nfile,node-name=q3nmedia",
        "-device", "q3n-nand-pci,drive=q3nmedia,addr=4.0"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        bufsize=0)
    capture = StderrCapture(proc.stderr)

    def command(line):
        proc.stdin.write((line + "\n").encode("ascii"))
        proc.stdin.flush()
        while True:
            response = read_response(proc.stdout, line, 15, capture, buffer)
            if response.startswith("IRQ "):
                continue
            if not response.startswith("OK"):
                raise RuntimeError(line + ": " + response)
            return response.split(maxsplit=1)

    def write(offset, value):
        command(f"writel 0x{BAR + offset:x} 0x{value:x}")

    def read(offset):
        return int(command(f"readl 0x{BAR + offset:x}")[1], 0)

    command("inb 0x80")
    command(f"outl 0xcf8 0x{PCI_CONFIG:x}")
    if int(command("inl 0xcfc")[1], 0) != 0x003d1b36:
        raise RuntimeError("Q3N PCI identity mismatch")
    command(f"outl 0xcf8 0x{PCI_CONFIG | 0x10:x}")
    command(f"outl 0xcfc 0x{BAR:x}")
    command(f"outl 0xcf8 0x{PCI_CONFIG | 0x04:x}")
    command("outw 0xcfc 0x6")

    die = args.logical_block % 2
    block_in_plane = args.logical_block // 2
    write(REG_READ_FLAGS, 1)
    write(REG_MP_DIE, die)
    write(REG_MP_BLOCK, block_in_plane)
    write(REG_MP_PAGE, 0)
    write(REG_LEN, MP_MAIN_SIZE)
    write(REG_CMD, CMD_MP_READ_PAGE)
    if read(REG_MP_DONE_MASK) != 0x0f or read(REG_MP_FAIL_MASK) != 0:
        raise RuntimeError("QEMU raw multi-plane read did not complete all four planes")

    main = bytearray()
    for offset in range(0, MP_MAIN_SIZE, CHUNK_SIZE):
        response = command(f"b64read 0x{BAR + REG_DATA + offset:x} 0x{CHUNK_SIZE:x}")
        if len(response) != 2:
            raise RuntimeError("QEMU did not return multi-plane data")
        chunk = base64.b64decode(response[1], validate=True)
        if len(chunk) != CHUNK_SIZE:
            raise RuntimeError("QEMU returned a truncated multi-plane chunk")
        main.extend(chunk)
    actual = hashlib.sha256(main).hexdigest()
    if actual != args.expected_erased_digest:
        raise SystemExit("multi-plane lower-level erased digest mismatch: got " +
                         actual + ", expected " + args.expected_erased_digest)
    print("q3n multi-plane media erase verified logical_block=" +
          str(args.logical_block) + " main_digest=" + actual)
finally:
    errors = teardown_qtest(proc, capture)
    if errors:
        raise SystemExit("qtest teardown failed: " + "; ".join(errors))
