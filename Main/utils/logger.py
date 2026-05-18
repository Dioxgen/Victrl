"""Logging setup for Victrl."""

import logging
import os
import sys


def setup_logging(debug: bool = False, log_file: str = "/var/log/victrl/agent.log") -> logging.Logger:
    """Configure logging to file and terminal.

    Args:
        debug: If True, set log level to DEBUG. Otherwise INFO.
        log_file: Path to the log file.

    Returns:
        Configured root logger instance.
    """
    log_dir = os.path.dirname(log_file)
    if log_dir and not os.path.exists(log_dir):
        os.makedirs(log_dir, exist_ok=True)

    level = logging.DEBUG if debug else logging.INFO
    fmt = logging.Formatter(
        "%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )

    root_logger = logging.getLogger("victrl")
    root_logger.setLevel(level)

    # File handler
    try:
        fh = logging.FileHandler(log_file)
        fh.setLevel(level)
        fh.setFormatter(fmt)
        root_logger.addHandler(fh)
    except PermissionError:
        print(f"Warning: Cannot write to {log_file}, logging to file disabled.", file=sys.stderr)

    # Console handler
    ch = logging.StreamHandler(sys.stdout)
    ch.setLevel(level)
    ch.setFormatter(fmt)
    root_logger.addHandler(ch)

    return root_logger
