"""Control-plane backends for the DBQA accelerator.

A :class:`Backend` hides how register reads/writes reach the hardware. The
:class:`VerilatorBackend` drives the Verilated model through the
``axil_server`` co-simulation harness (built with ``make axil-server``); a
real hardware MMIO backend can be added for synthesis (Phase 11).
"""

from __future__ import annotations

import os
import subprocess
from abc import ABC, abstractmethod

from . import regs


class Backend(ABC):
    """Abstract register interface shared by every backend."""

    @abstractmethod
    def write_word(self, word: int, data: int) -> None:
        """Write ``data`` to register at word offset ``word``."""

    @abstractmethod
    def read_word(self, word: int) -> int:
        """Read the register at word offset ``word``."""

    @abstractmethod
    def next_group(self):
        """Return the next GROUP BY group beat or ``None``.

        Each call advances the accelerator by one clock cycle, so polling is
        safe even while the GROUP BY dump phase has not started. The returned
        value is ``(key, count, sum, min, max, last)``.
        """

    def load_table(self, table) -> None:
        """Load a table through the load registers (one row per commit).

        ``table`` is an iterable of iterables of column values; the number of
        columns must equal ``NUM_COLS``.
        """
        for r, row in enumerate(table):
            cols = list(row)
            if len(cols) != regs.NUM_COLS:
                raise ValueError(
                    f"row {r} has {len(cols)} columns, expected {regs.NUM_COLS}"
                )
            self.write_word(regs.REG_LOAD_ADDR, r)
            for c, value in enumerate(cols):
                self.write_word(regs.REG_LOAD_DATA0 + c, value & 0xFFFFFFFF)
            self.write_word(regs.REG_LOAD_ROW, 0)


class RecordingBackend(Backend):
    """Records every register operation for unit tests and golden sequences.

    ``read_word`` returns the configured ``status`` for ``REG_STATUS`` (done
    is set by default so classic queries complete immediately) and 0 for
    every other register.
    """

    def __init__(self, status=0):
        self.writes = []  # (word, data)
        self.reads = []  # word
        self.status = status | (1 << regs.STATUS_DONE)

    def write_word(self, word, data):
        self.writes.append((word, data))

    def read_word(self, word):
        self.reads.append(word)
        return self.status if word == regs.REG_STATUS else 0

    def next_group(self):
        # A single terminal group so GROUP BY sequences complete immediately.
        return (0, 0, 0, 0, 0, True)


class VerilatorBackend(Backend):
    """Drives the Verilated model via the ``axil_server`` stdio harness."""

    def __init__(self, server: str | None = None, timeout: float = 120.0):
        self._server = server or self._default_server_path()
        self._timeout = timeout
        self._proc = None

    @staticmethod
    def _default_server_path() -> str:
        env = os.environ.get("DBQA_AXIL_SERVER")
        if env:
            return env
        # Assume a repository checkout with a CMake build tree.
        repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        return os.path.join(repo, "build", "axil_server", "Vaxil_server")

    def _start(self):
        if self._proc is not None and self._proc.poll() is None:
            return
        if not os.path.exists(self._server):
            raise RuntimeError(
                f"axil_server not found at {self._server!r}; run `make axil-server` "
                "or set DBQA_AXIL_SERVER"
            )
        self._proc = subprocess.Popen(
            [self._server],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )

    def _cmd(self, line: str) -> str:
        self._start()
        assert self._proc and self._proc.stdin and self._proc.stdout
        self._proc.stdin.write(line + "\n")
        self._proc.stdin.flush()
        reply = self._proc.stdout.readline()
        if not reply:
            stderr = self._proc.stderr.read() if self._proc.stderr else ""
            raise RuntimeError(f"axil_server closed: {stderr.strip()}")
        return reply.strip()

    def write_word(self, word, data):
        reply = self._cmd(f"W {word & 0xFFFF:x} {data & 0xFFFFFFFF:x}")
        if reply != "OK":
            raise RuntimeError(f"write of register 0x{word:x} failed: {reply}")

    def read_word(self, word):
        reply = self._cmd(f"R {word & 0xFFFF:x}")
        return int(reply, 16)

    def next_group(self):
        reply = self._cmd("G")
        if reply == "NONE":
            return None
        parts = reply.split()
        if len(parts) != 6:
            raise RuntimeError(f"unexpected group reply: {reply!r}")
        key, count, sm, mn, mx, last = (int(p, 16) for p in parts)
        return (key, count, sm, mn, mx, last == 1)

    def close(self):
        if self._proc is not None and self._proc.poll() is None:
            try:
                self._proc.stdin.write("Q\n")
                self._proc.stdin.flush()
                self._proc.wait(timeout=5)
            except Exception:
                self._proc.kill()
        self._proc = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
