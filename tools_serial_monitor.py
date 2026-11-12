#!/usr/bin/env python3
"""Monitor serial minimal (pengganti screen/minicom yang tidak terpasang).

Hanya memakai stdlib. Cocok untuk melihat keluaran konsol USB CDC-ACM
(B2LUsbSerialDxe / M3) dan mengetik balik ke perangkat.

Kullanim:
    sudo python3 tools_serial_monitor.py /dev/ttyACM0 115200

Catatan:
  - Keluar: Ctrl-]
  - Port /dev/ttyACM0 biasanya milik root:uucp, jadi jalankan dengan sudo
    ATAU tambahkan user ke grup uucp (perlu login ulang):
        sudo usermod -aG uucp $USER
"""

import atexit
import os
import select
import sys
import termios
import tty

QUIT = b"\x1d"  # Ctrl-]


def fail(msg, code=1):
    print(msg, file=sys.stderr)
    sys.exit(code)


def set_raw_serial(fd, baud):
    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = termios.tcgetattr(fd)
    iflag = 0
    oflag = 0
    lflag = 0
    cflag = termios.CS8 | termios.CREAD | termios.CLOCAL
    speed = getattr(termios, "B%d" % baud, termios.B115200)
    ispeed = speed
    ospeed = speed
    cc = list(cc)
    cc[termios.VMIN] = 0
    cc[termios.VTIME] = 0
    termios.tcsetattr(
        fd, termios.TCSANOW, [iflag, oflag, cflag, lflag, ispeed, ospeed, cc]
    )
    termios.tcflush(fd, termios.TCIOFLUSH)


def main():
    if len(sys.argv) < 2:
        fail("kullanim: %s <port> [baud]" % sys.argv[0], 2)

    port = sys.argv[1]
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

    try:
        fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    except OSError as exc:
        fail("port acilamadi (%s): %s" % (port, exc))

    try:
        set_raw_serial(fd, baud)
    except termios.error as exc:
        os.close(fd)
        fail("port ayarlanamadi (%s): %s" % (port, exc))

    stdin_fd = sys.stdin.fileno()
    old_stdin = None
    if os.isatty(stdin_fd):
        old_stdin = termios.tcgetattr(stdin_fd)
        tty.setraw(stdin_fd)

    def restore():
        if old_stdin is not None:
            termios.tcsetattr(stdin_fd, termios.TCSADRAIN, old_stdin)

    atexit.register(restore)
    print("[bagli: %s @ %d  -  keluar: Ctrl-]]" % (port, baud), file=sys.stderr)

    try:
        while True:
            ready, _, _ = select.select([fd, stdin_fd], [], [], 0.2)

            if fd in ready:
                try:
                    data = os.read(fd, 4096)
                except BlockingIOError:
                    data = b""
                except OSError as exc:
                    print("\n[port terputus: %s]" % exc, file=sys.stderr)
                    break
                if data:
                    os.write(1, data)
                    sys.stdout.flush()

            if stdin_fd in ready:
                chunk = os.read(stdin_fd, 1024)
                if not chunk:
                    break
                if QUIT in chunk:
                    pre = chunk.split(QUIT)[0]
                    if pre:
                        os.write(fd, pre)
                    break
                os.write(fd, chunk)
    except KeyboardInterrupt:
        pass
    finally:
        restore()
        os.close(fd)


if __name__ == "__main__":
    main()
