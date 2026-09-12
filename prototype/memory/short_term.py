"""L1 Short-term memory: semantic action history with auto-compression."""

import logging

from config import HISTORY_MAX_LEN

logger = logging.getLogger("victrl.memory.short_term")


class ShortTermMemory:
    """Stores recent action summaries and auto-compresses when limit exceeded."""

    def __init__(self, max_len: int = HISTORY_MAX_LEN):
        """Initialize short-term memory.

        Args:
            max_len: Maximum number of history entries before compression.
        """
        self.max_len = max_len
        self.history: list[str] = []

    def add(self, event: str) -> None:
        """Add a new event summary and trigger compression if needed.

        Args:
            event: Semantic summary string for the action.
        """
        self.history.append(event)
        if len(self.history) > self.max_len:
            self.compress()

    def get_last(self, n: int | None = None) -> list[str]:
        """Return the most recent n summaries.

        Args:
            n: Number of entries to return. If None, returns all.

        Returns:
            List of summary strings.
        """
        if n is None:
            return list(self.history)
        return self.history[-n:]

    def compress(self) -> None:
        """Merge the two oldest entries into one to stay within max_len."""
        if len(self.history) >= 2:
            merged = f"{self.history[0]}; {self.history[1]}"
            self.history = [merged] + self.history[2:]
            logger.debug(f"Compressed history: new length={len(self.history)}")

    def clear(self) -> None:
        """Clear all history."""
        self.history.clear()
