#!/usr/bin/env python3
"""Victrl — Hardware AI Agent entry point.

Parses command-line arguments, initializes subsystems, and starts the agent loop.
"""

import argparse
import logging
import os
import sys
import signal

from config import (
    API_HOST,
    API_PORT,
    DEFAULT_API_ENDPOINT,
    DEFAULT_API_KEY,
    DEFAULT_MODEL_NAME,
    DEFAULT_UVC_DEVICE,
    DEFAULT_SCREEN_WIDTH,
    DEFAULT_SCREEN_HEIGHT,
    HISTORY_MAX_LEN,
    MAX_ACTIONS,
    PLAN_DIR,
    PROFILE_DIR,
)
from core.agent import VictrlAgent
from api.server import start_api_server
from utils.logger import setup_logging
from utils.system_utils import check_uinput, find_uvc_devices


def parse_args() -> argparse.Namespace:
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(
        description="Victrl — Hardware AI Agent (UVC + HID + LLM)",
    )

    # UVC
    parser.add_argument(
        "--uvc_device", default=DEFAULT_UVC_DEVICE,
        help=f"V4L2 device path (default: {DEFAULT_UVC_DEVICE})",
    )
    parser.add_argument(
        "--screen_width", type=int, default=DEFAULT_SCREEN_WIDTH,
        help=f"Capture width (default: {DEFAULT_SCREEN_WIDTH})",
    )
    parser.add_argument(
        "--screen_height", type=int, default=DEFAULT_SCREEN_HEIGHT,
        help=f"Capture height (default: {DEFAULT_SCREEN_HEIGHT})",
    )

    # API
    parser.add_argument(
        "--api_endpoint", default=DEFAULT_API_ENDPOINT,
        help="Model API endpoint URL",
    )
    parser.add_argument(
        "--api_key", default=None,
        help=f"Model API authentication key (default from config)" ,
    )
    parser.add_argument(
        "--model_name", default=DEFAULT_MODEL_NAME,
        help=f"Model identifier (default: {DEFAULT_MODEL_NAME})",
    )

    # Task
    parser.add_argument("--task", default=None, help="Task description to execute")

    # Paths
    parser.add_argument("--plan_dir", default=PLAN_DIR, help="Plan file directory")
    parser.add_argument("--profile_dir", default=PROFILE_DIR, help="Profile directory")
    parser.add_argument("--log_file", default="/var/log/victrl/agent.log", help="Log file path")

    # Limits
    parser.add_argument("--max_actions", type=int, default=MAX_ACTIONS,
                        help=f"Maximum actions per task (default: {MAX_ACTIONS})")
    parser.add_argument("--history_max_len", type=int, default=HISTORY_MAX_LEN,
                        help=f"Max short-term memory entries (default: {HISTORY_MAX_LEN})")

    # HID backend
    parser.add_argument(
        "--hid-backend", default="uinput", choices=["uinput", "serial"],
        help="HID output backend: uinput (default) or serial (ESP32 BLE HID)",
    )
    parser.add_argument(
        "--serial-port", default="/dev/ttyUSB0",
        help="Serial port for ESP32 (default: /dev/ttyUSB0)",
    )

    # Modes
    parser.add_argument("--debug", action="store_true", help="Enable debug logging")
    parser.add_argument("--dry-run", action="store_true", help="Mock HID and model calls")

    # API server
    parser.add_argument("--no-api", action="store_true", help="Disable HTTP API server")
    parser.add_argument("--api-host", default=API_HOST, help=f"API bind address (default: {API_HOST})")
    parser.add_argument("--api-port", type=int, default=API_PORT,
                        help=f"API bind port (default: {API_PORT})")

    return parser.parse_args()


def main() -> None:
    """Program entry point."""
    args = parse_args()

    # Setup logging
    logger = setup_logging(debug=args.debug, log_file=args.log_file)
    logger.info("Victrl starting...")

    # Check system prerequisites
    if not args.dry_run:
        if args.hid_backend == "uinput":
            if not check_uinput():
                logger.warning("uinput check failed; may need sudo modprobe uinput")
        elif args.hid_backend == "serial":
            if not os.path.exists(args.serial_port):
                logger.warning(f"Serial port {args.serial_port} not found; check connection")
        devices = find_uvc_devices()
        if devices:
            logger.info(f"Found video devices: {devices}")
        else:
            logger.warning("No /dev/video* devices found")

    # Create agent
    api_key = args.api_key if args.api_key is not None else DEFAULT_API_KEY
    agent = VictrlAgent(
        uvc_device=args.uvc_device,
        screen_width=args.screen_width,
        screen_height=args.screen_height,
        api_endpoint=args.api_endpoint,
        api_key=api_key,
        model_name=args.model_name,
        max_actions=args.max_actions,
        plan_dir=args.plan_dir,
        profile_dir=args.profile_dir,
        history_max_len=args.history_max_len,
        dry_run=args.dry_run,
        hid_backend=args.hid_backend,
        serial_port=args.serial_port,
    )

    logger.info(f"Agent created. Dry-run={args.dry_run}, "
                f"hid_backend={args.hid_backend}, "
                f"screen={args.screen_width}x{args.screen_height}, "
                f"model={args.model_name}")

    # Start API server
    if not args.no_api:
        start_api_server(agent, host=args.api_host, port=args.api_port)

    # Run task if provided
    if args.task:
        logger.info(f"Starting task: {args.task}")
        try:
            agent.run(task_goal=args.task)
        except KeyboardInterrupt:
            logger.info("Interrupted by user")
    else:
        logger.info("No --task provided. Waiting for API /start request...")
        logger.info(f"API server listening on {args.api_host}:{args.api_port}")

        # Keep main thread alive for API
        try:
            while True:
                signal.pause()
        except (KeyboardInterrupt, SystemExit):
            logger.info("Shutting down...")
        finally:
            agent.stop()

    logger.info("Victrl exited.")


if __name__ == "__main__":
    main()
