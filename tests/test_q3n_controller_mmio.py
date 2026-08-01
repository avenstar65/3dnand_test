import os
import base64
import signal
import subprocess
import tempfile
import time

QEMU = "/workspace/work/build/qemu-11.0.2/qemu-system-x86_64"
BAR = 0xfebe0000
PCI_CONFIG = 0x80002000

REG_CAP = 0x04
REG_STATUS = 0x0c
REG_CMD = 0x10
REG_ADDR_LO = 0x14
REG_ADDR_HI = 0x18
REG_LEN = 0x1c
REG_OOB_LEN = 0x30
REG_IRQ_STATUS = 0x38
REG_IRQ_MASK = 0x3c
REG_FAULT_ADDR_LO = 0x60
REG_FAULT_ADDR_HI = 0x64
REG_FAULT_CTRL = 0x68
REG_ECC_STATUS = 0x90
REG_ECC_FAILED_STEP = 0x9c
REG_RETRY_MODE = 0xc0
REG_MP_DIE = 0xc4
REG_MP_BLOCK = 0xc8
REG_MP_PAGE = 0xcc
REG_MP_DONE_MASK = 0xd0
REG_MP_FAIL_MASK = 0xd4
REG_MP_ECC_SELECT = 0xd8
REG_MP_ECC_STATUS = 0xdc
REG_MP_ECC_FAILED_STEP = 0xe8
REG_DATA = 0x1000

CMD_READ_PAGE = 2
CMD_RESET = 5
CMD_MP_READ_PAGE = 9
CMD_MP_PROGRAM_PAGE = 10
CMD_MP_READ_OOB = 11
CMD_MP_PROGRAM_OOB = 12
CMD_MP_ERASE_GROUP = 13

READY = 1
ERROR = 2
DONE_ERROR = 3
MP_MAIN_SIZE = 65536
MP_OOB_SIZE = 4096


def require(value, message):
    if not value:
        raise AssertionError(message)


with tempfile.NamedTemporaryFile(prefix="q3n-controller-qtest-",
                                 dir="/workspace/work", delete=False) as media:
    media_path = media.name

proc = None
try:
    require(os.path.exists(QEMU), "build QEMU first with scripts/build-qemu.sh")
    proc = subprocess.Popen([
        QEMU, "-machine", "q35", "-nodefaults", "-nographic",
        "-accel", "qtest", "-qtest", "stdio",
        "-blockdev", "driver=file,filename=" + media_path + ",node-name=q3nfile",
        "-blockdev", "driver=raw,file=q3nfile,node-name=q3nmedia",
        "-device", "q3n-nand-pci,drive=q3nmedia,addr=4.0"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True)
    time.sleep(0.2)
    if proc.poll() is not None:
        raise AssertionError("qtest startup: " + proc.stderr.read())

    def command(line):
        events = []
        proc.stdin.write(line + "\n")
        proc.stdin.flush()
        while True:
            response = proc.stdout.readline().strip()
            require(response, "qtest closed while processing " + line)
            if response.startswith("IRQ "):
                events.append(response)
            elif response.startswith("OK"):
                parts = response.split()
                return (int(parts[1], 0) if len(parts) > 1 else None, events)
            else:
                raise AssertionError(line + ": " + response)

    def outl(port, value):
        return command(f"outl 0x{port:x} 0x{value:x}")

    def outw(port, value):
        return command(f"outw 0x{port:x} 0x{value:x}")

    def inl(port):
        return command(f"inl 0x{port:x}")[0]

    def write(offset, value):
        return command(f"writel 0x{BAR + offset:x} 0x{value:x}")

    def read(offset):
        return command(f"readl 0x{BAR + offset:x}")[0]

    def clear_irq():
        write(REG_IRQ_STATUS, DONE_ERROR)

    def completion(events, name):
        require(sum(event.startswith("IRQ raise ") for event in events) == 1,
                name + " must issue exactly one completion IRQ: " + repr(events))
        require(not any(event.startswith("IRQ lower ") for event in events),
                name + " lowered IRQ during completion: " + repr(events))

    command("irq_intercept_in ioapic")
    outl(0xcf8, PCI_CONFIG)
    require(inl(0xcfc) == 0x003d1b36, "PCI identity")
    outl(0xcf8, PCI_CONFIG | 0x10)
    outl(0xcfc, BAR)
    outl(0xcf8, PCI_CONFIG | 0x04)
    outw(0xcfc, 6)
    require(read(REG_CAP) & 0x10, "CAP_MULTIPLANE")
    write(REG_IRQ_MASK, DONE_ERROR)

    # Existing command dispatch and scalar ECC path.
    write(REG_ADDR_LO, 0)
    write(REG_ADDR_HI, 0)
    clear_irq()
    _, events = write(REG_CMD, CMD_READ_PAGE)
    completion(events, "legacy READ_PAGE")
    require(read(REG_ECC_STATUS) == 0 and read(REG_ECC_FAILED_STEP) == 0xffffffff,
            "legacy scalar ECC")

    # Main READ traverses real MMIO into four engine members and exposes both
    # ends of the enlarged 64 KiB DATA window.
    clear_irq()
    write(REG_MP_DIE, 0)
    write(REG_MP_BLOCK, 0)
    write(REG_MP_PAGE, 0)
    write(REG_LEN, MP_MAIN_SIZE)
    _, events = write(REG_CMD, CMD_MP_READ_PAGE)
    completion(events, "MP_READ_PAGE")
    require(read(REG_MP_DONE_MASK) == 0x0f and read(REG_MP_FAIL_MASK) == 0,
            "MP read masks")
    require(read(REG_MP_ECC_STATUS) == 0, "MP ECC latch")
    saved_ecc = read(REG_MP_ECC_FAILED_STEP)
    require(saved_ecc == 0xffffffff, "MP ECC failed step")
    require(read(REG_DATA) == 0xffffffff and
            read(REG_DATA + MP_MAIN_SIZE - 4) == 0xffffffff,
            "64 KiB DATA window")

    # OOB uses the same window.  OOB/program/erase keep READ's MP ECC latch.
    clear_irq()
    write(REG_MP_BLOCK, 1)
    write(REG_OOB_LEN, MP_OOB_SIZE)
    oob = b"".join(b"\xff" + b"\xa5" * 1023 for _ in range(4))
    command(f"b64write 0x{BAR + REG_DATA:x} 0x{MP_OOB_SIZE:x} " +
            base64.b64encode(oob).decode("ascii"))
    _, events = write(REG_CMD, CMD_MP_PROGRAM_OOB)
    completion(events, "MP_PROGRAM_OOB")
    require(read(REG_MP_DONE_MASK) == 0x0f and read(REG_MP_FAIL_MASK) == 0,
            "MP OOB program masks")
    require(read(REG_MP_ECC_FAILED_STEP) == saved_ecc, "OOB program ECC latch")
    clear_irq()
    write(REG_OOB_LEN, MP_OOB_SIZE)
    _, events = write(REG_CMD, CMD_MP_READ_OOB)
    completion(events, "MP_READ_OOB")
    require(read(REG_DATA) == 0xa5a5a5ff, "OOB DATA window reuse")
    require(read(REG_MP_ECC_FAILED_STEP) == saved_ecc, "OOB read ECC latch")
    clear_irq()
    _, events = write(REG_CMD, CMD_MP_ERASE_GROUP)
    completion(events, "MP_ERASE_GROUP")
    require(read(REG_MP_DONE_MASK) == 0x0f and read(REG_MP_FAIL_MASK) == 0,
            "MP erase masks")
    require(read(REG_MP_ECC_FAILED_STEP) == saved_ecc, "erase ECC latch")

    # This targets current physical plane 2 (block 494, address 0x303e00000)
    # and proves partial failure is fully latched before the single completion.
    clear_irq()
    write(REG_MP_BLOCK, 0)
    write(REG_FAULT_ADDR_LO, 0x03e00000)
    write(REG_FAULT_ADDR_HI, 3)
    write(REG_FAULT_CTRL, 2)
    write(REG_LEN, MP_MAIN_SIZE)
    command(f"memset 0x{BAR + REG_DATA:x} 0x{MP_MAIN_SIZE:x} 0x5a")
    _, events = write(REG_CMD, CMD_MP_PROGRAM_PAGE)
    completion(events, "partial MP_PROGRAM_PAGE")
    require(read(REG_MP_DONE_MASK) == 0x0b and read(REG_MP_FAIL_MASK) == 0x04,
            "partial program masks")
    require(read(REG_STATUS) & (READY | ERROR) == READY | ERROR,
            "partial program completion")
    require(read(REG_MP_ECC_FAILED_STEP) == saved_ecc, "program ECC latch")

    # Prevalidation creates no member result; invalid selector retains 2.
    clear_irq()
    write(REG_MP_DIE, 2)
    write(REG_LEN, MP_MAIN_SIZE)
    _, events = write(REG_CMD, CMD_MP_READ_PAGE)
    completion(events, "invalid MP_READ_PAGE")
    require(read(REG_MP_DONE_MASK) == 0 and read(REG_MP_FAIL_MASK) == 0,
            "prevalidation masks")
    require(read(REG_STATUS) & (READY | ERROR) == READY | ERROR,
            "prevalidation status")
    write(REG_MP_ECC_SELECT, 2)
    write(REG_MP_ECC_SELECT, 4)
    require(read(REG_MP_ECC_SELECT) == 2 and read(REG_STATUS) & ERROR,
            "illegal selector")

    write(REG_RETRY_MODE, 3)
    write(REG_MP_BLOCK, 7)
    write(REG_MP_PAGE, 9)
    write(REG_LEN, MP_MAIN_SIZE)
    command(f"memset 0x{BAR + REG_DATA:x} 4 0x11")
    clear_irq()
    _, events = write(REG_CMD, CMD_RESET)
    completion(events, "RESET")
    require(read(REG_MP_DIE) == 0 and read(REG_MP_BLOCK) == 0 and
            read(REG_MP_PAGE) == 0 and read(REG_MP_DONE_MASK) == 0 and
            read(REG_MP_FAIL_MASK) == 0, "reset MP state")
    require(read(REG_MP_ECC_SELECT) == 0 and read(REG_MP_ECC_STATUS) == 0 and
            read(REG_MP_ECC_FAILED_STEP) == 0, "reset MP ECC")
    require(read(REG_RETRY_MODE) == 0 and read(REG_DATA) == 0xffffffff,
            "reset retry and cursor")
finally:
    if proc is not None and proc.poll() is None:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=10)
    if proc is not None and proc.returncode not in (0, -signal.SIGINT):
        raise SystemExit(proc.stderr.read())
    os.unlink(media_path)
