"""Global configuration — loads values from config.json."""

import json
import os

_CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "config.json")

with open(_CONFIG_PATH, encoding="utf-8") as _f:
    _cfg = json.load(_f)

# ── UVC ───────────────────────────────────────────────────────────────────
DEFAULT_UVC_DEVICE   = _cfg["uvc"]["device"]
DEFAULT_SCREEN_WIDTH  = _cfg["uvc"]["screen_width"]
DEFAULT_SCREEN_HEIGHT = _cfg["uvc"]["screen_height"]

# ── Cloud API ─────────────────────────────────────────────────────────────
DEFAULT_API_ENDPOINT = _cfg["api"]["endpoint"]
DEFAULT_API_KEY      = _cfg["api"]["key"]
DEFAULT_MODEL_NAME   = _cfg["api"]["model_name"]
API_TIMEOUT          = _cfg["api"]["timeout"]
API_MAX_RETRIES      = _cfg["api"]["max_retries"]

# ── Agent ─────────────────────────────────────────────────────────────────
MAX_ACTIONS      = _cfg["agent"]["max_actions"]
HISTORY_MAX_LEN  = _cfg["agent"]["history_max_len"]

# ── Paths ─────────────────────────────────────────────────────────────────
PLAN_DIR     = _cfg["paths"]["plan_dir"]
PROFILE_DIR  = _cfg["paths"]["profile_dir"]
PROFILE_FILE = _cfg["paths"]["profile_file"]
LOG_DIR      = _cfg["paths"].get("log_dir", "./log")

# ── HTTP API ──────────────────────────────────────────────────────────────
API_HOST = _cfg["http"]["host"]
API_PORT = _cfg["http"]["port"]
