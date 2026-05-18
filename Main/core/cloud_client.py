"""Cloud model client for calling multimodal LLM APIs."""

import base64
import io
import json
import logging
import time

import requests
from PIL import Image

from config import API_MAX_RETRIES, API_TIMEOUT
from utils.exceptions import CloudAPIError

logger = logging.getLogger("victrl.cloud")

SYSTEM_PROMPT = """You are Victrl, a hardware AI agent controlling a computer via HID. You see the screen via UVC. Your task is to fulfill the user's goal by outputting JSON actions.

**Device Profile (natural language, provided below):**
{device_profile_text}

**Hardware Status (dynamic):**
- UVC capture: OK
- HID device: active
- Currently pressed buttons: {pressed_buttons}

**Your output must be a JSON object with the following schema:**
{{
  "action_type": "click|move|drag|scroll|press|type|wait|release|complete|error",
  "box_2d": [ymin, xmin, ymax, xmax],
  "from_box": [ymin, xmin, ymax, xmax],
  "to_box": [ymin, xmin, ymax, xmax],
  "button": "left"|"right"|"middle"|"double_left",
  "hold": int,
  "delta_y": int,
  "delta_x": int,
  "key": "string",
  "text": "string",
  "wait_seconds": float,
  "message": "string",
  "need_screen": bool,
  "sleep_before_next": float,
  "observation": "string",
  "plan_update": {{
    "summary": "Brief description of current state and what you're doing",
    "current_milestone": int,
    "milestones": [{{"id": int, "description": "high-level sub-goal, NOT a specific UI action", "status": "pending|in_progress|done"}}]
  }},
  "profile_updates": [{{"content": "string"}}],
  "done": bool,
  "verification": "string"
}}

**Core Principles:**

1. SCREEN IS GROUND TRUTH. The screen shows reality. If the plan says "Notepad is open" but the screen shows an error dialog or empty desktop, BELIEVE YOUR EYES not the plan. Adapt immediately.

2. PLAN IS A COMPASS, NOT A SCRIPT. The milestones describe WHAT to achieve (high-level sub-goals), not HOW to do it. You decide the specific clicks, keys, and timing based on what you see on screen RIGHT NOW. If a milestone was marked "done" but the screen shows it failed, re-open it.

3. THINK BEFORE ACTING. Use the "observation" field to describe what you see on screen: which windows are open, what's selected, any error messages, and why you chose this action. This is your situational awareness log.

4. VERIFY COMPLETION. When you believe the task is done, you MUST set need_screen: true to get a fresh screen capture, then examine it carefully. Only output done: true after you have seen visual confirmation that the goal is achieved. Include a "verification" field explaining exactly what elements on screen prove success. If verification fails, continue with corrective actions.

5. USE PROFILE UPDATES. Every time you discover a UI element location, shortcut, or behavioral pattern, record it. This knowledge persists across tasks.

**Rules:**
- Coordinates normalized [0,1], 3 decimal places.
- hold: -1 keeps button pressed; later use release.
- Provide sleep_before_next >0 when need_screen: false to avoid CPU waste.
- Always fill the "observation" field — what do you see, and why this action?
- Update plan_update every response. Milestones describe sub-goals (e.g. "Open the target application"), NOT specific clicks (e.g. "Click Start menu").
- When task complete, output action_type: "complete", done: true, with a non-empty "verification" field.
- For unrecoverable error, output action_type: "error", done: true.
"""

REQUIRED_FIELDS = {"action_type", "plan_update", "done"}

# ── Mock responses for dry-run ──────────────────────────────────────────
_MOCK_STEP = 0
_MOCK_RESPONSES = [
    {
        "action_type": "click",
        "box_2d": [0.10, 0.50, 0.15, 0.55],
        "button": "left",
        "need_screen": True,
        "sleep_before_next": 0.3,
        "observation": "Desktop visible with taskbar at bottom. Start button in lower-left corner.",
        "plan_update": {
            "summary": "Clicking Start button to begin opening the application",
            "current_milestone": 1,
            "milestones": [
                {"id": 1, "description": "Open the target application", "status": "in_progress"},
                {"id": 2, "description": "Perform the requested operation", "status": "pending"},
                {"id": 3, "description": "Verify the result", "status": "pending"},
            ],
        },
        "done": False,
    },
    {
        "action_type": "type",
        "text": "notepad",
        "need_screen": True,
        "sleep_before_next": 0.2,
        "observation": "Start menu opened, search field is focused and ready for input.",
        "plan_update": {
            "summary": "Typing application name into Start search",
            "current_milestone": 1,
            "milestones": [
                {"id": 1, "description": "Open the target application", "status": "in_progress"},
                {"id": 2, "description": "Perform the requested operation", "status": "pending"},
                {"id": 3, "description": "Verify the result", "status": "pending"},
            ],
        },
        "done": False,
    },
    {
        "action_type": "press",
        "key": "enter",
        "need_screen": True,
        "sleep_before_next": 0.8,
        "observation": "Notepad is the top search result, highlighted. Pressing Enter to launch.",
        "plan_update": {
            "summary": "Launching Notepad via Enter key",
            "current_milestone": 1,
            "milestones": [
                {"id": 1, "description": "Open the target application", "status": "in_progress"},
                {"id": 2, "description": "Perform the requested operation", "status": "pending"},
                {"id": 3, "description": "Verify the result", "status": "pending"},
            ],
        },
        "done": False,
    },
    {
        "action_type": "type",
        "text": "Hello from Victrl!",
        "need_screen": True,
        "sleep_before_next": 0.1,
        "observation": "Notepad window is open with a blank document. Cursor is flashing in the text area.",
        "plan_update": {
            "summary": "Typing content into Notepad",
            "current_milestone": 2,
            "milestones": [
                {"id": 1, "description": "Open the target application", "status": "done"},
                {"id": 2, "description": "Perform the requested operation", "status": "in_progress"},
                {"id": 3, "description": "Verify the result", "status": "pending"},
            ],
        },
        "done": False,
    },
    {
        "action_type": "complete",
        "message": "Task completed successfully.",
        "need_screen": True,
        "observation": "Notepad window shows the text 'Hello from Victrl!' in the editor area. Title bar confirms 'Untitled - Notepad'.",
        "verification": "Notepad is open and contains the target text 'Hello from Victrl!'. The title bar and text content confirm success.",
        "plan_update": {
            "summary": "Verified: Notepad is open with correct content. Task complete.",
            "current_milestone": 3,
            "milestones": [
                {"id": 1, "description": "Open the target application", "status": "done"},
                {"id": 2, "description": "Perform the requested operation", "status": "done"},
                {"id": 3, "description": "Verify the result", "status": "done"},
            ],
        },
        "profile_updates": [
            {"content": "- Start menu opens via left-click on Windows icon at [0.10, 0.50, 0.15, 0.55]"},
            {"content": "- Start search field auto-focuses when Start menu opens, accepts text input immediately"},
        ],
        "done": True,
    },
]


class MockCloudClient:
    """Mock client that returns canned responses for dry-run testing."""

    def __init__(self):
        self._step = 0
        logger.info("MockCloudClient initialized — no real API calls will be made")

    def query(self, image=None, plan=None, history=None, system_prompt="", profile_text="") -> dict | None:
        """Return the next canned response, cycling through the mock sequence."""
        idx = self._step
        self._step += 1
        if idx >= len(_MOCK_RESPONSES):
            idx = _MOCK_RESPONSES.index(
                next(r for r in _MOCK_RESPONSES if r.get("done"))
            )
        resp = _MOCK_RESPONSES[idx]
        logger.info(f"[MOCK] query #{idx}: action_type={resp.get('action_type')}")
        return resp


class CloudClient:
    """Client for calling multimodal LLM APIs."""

    def __init__(self, api_endpoint: str, api_key: str, model_name: str):
        """Initialize cloud client.

        Args:
            api_endpoint: API base URL.
            api_key: API key for authentication.
            model_name: Model identifier.
        """
        self.api_endpoint = api_endpoint.rstrip("/")
        self.api_key = api_key
        self.model_name = model_name
        logger.info(f"CloudClient initialized: endpoint={api_endpoint}, model={model_name}")

    def query(
        self,
        image: Image.Image | None,
        plan: dict | None,
        history: list,
        system_prompt: str,
        profile_text: str,
    ) -> dict | None:
        """Send a query to the model and parse the response.

        Args:
            image: Current screen capture (or None if not needed).
            plan: Current plan dict.
            history: List of recent action summaries.
            system_prompt: Base system prompt template.
            profile_text: Device profile content.

        Returns:
            Parsed response dict, or None on failure.
        """
        system_content = system_prompt.format(
            device_profile_text=profile_text,
            pressed_buttons="[]",
        )

        # Build user content
        user_parts = []
        plan_summary = json.dumps(plan, ensure_ascii=False, indent=2) if plan else "No plan yet. Create one."
        history_text = "\n".join(history) if history else "No history yet."

        user_parts.append({
            "type": "text",
            "text": (
                f"Current plan:\n{plan_summary}\n\n"
                f"Recent action history (last {len(history)}):\n{history_text}\n\n"
                "Analyze the screen and output the next action as JSON."
            ),
        })

        if image is not None:
            buf = io.BytesIO()
            image.save(buf, format="JPEG", quality=85)
            img_b64 = base64.b64encode(buf.getvalue()).decode("ascii")
            user_parts.append({
                "type": "image_url",
                "image_url": {"url": f"data:image/jpeg;base64,{img_b64}"},
            })

        payload = {
            "model": self.model_name,
            "messages": [
                {"role": "system", "content": system_content},
                {"role": "user", "content": user_parts},
            ],
            "temperature": 0.1,
            "max_tokens": 4096,
        }

        last_error = None
        for attempt in range(API_MAX_RETRIES + 1):
            try:
                resp = requests.post(
                    self.api_endpoint,
                    headers={
                        "Authorization": f"Bearer {self.api_key}",
                        "Content-Type": "application/json",
                    },
                    json=payload,
                    timeout=API_TIMEOUT,
                )
                resp.raise_for_status()
                data = resp.json()

                # Extract assistant message content
                content = data["choices"][0]["message"]["content"]
                parsed = self._parse_response(content)

                if parsed is not None:
                    return parsed

                # If parsing failed, retry with stricter prompt
                logger.warning(f"Response parse failed, attempt {attempt + 1}")
                if attempt < API_MAX_RETRIES:
                    user_parts.insert(0, {
                        "type": "text",
                        "text": "IMPORTANT: Your previous response was not valid JSON. "
                                "Output ONLY a valid JSON object with the required schema. No other text.",
                    })
                    time.sleep(1)

            except requests.exceptions.Timeout:
                last_error = "Request timed out"
                logger.warning(f"Timeout on attempt {attempt + 1}")
            except requests.exceptions.RequestException as e:
                last_error = str(e)
                logger.warning(f"API error on attempt {attempt + 1}: {e}")
            except (KeyError, IndexError) as e:
                last_error = f"Invalid response structure: {e}"
                logger.error(last_error)
            except Exception as e:
                last_error = str(e)
                logger.error(f"Unexpected error: {e}")
                break

        logger.error(f"All {API_MAX_RETRIES + 1} attempts failed. Last error: {last_error}")
        return None

    def _parse_response(self, content: str) -> dict | None:
        """Parse JSON from model response.

        Args:
            content: Raw response text from the model.

        Returns:
            Parsed dict with validated fields, or None if invalid.
        """
        if not content:
            return None

        # Strip markdown code fences if present
        text = content.strip()
        if text.startswith("```"):
            lines = text.split("\n")
            if lines[0].startswith("```"):
                lines = lines[1:]
            if lines and lines[-1].strip() == "```":
                lines = lines[:-1]
            text = "\n".join(lines)

        try:
            data = json.loads(text)
        except json.JSONDecodeError as e:
            # Try to extract JSON from the text
            start = text.find("{")
            end = text.rfind("}")
            if start != -1 and end != -1 and end > start:
                try:
                    data = json.loads(text[start:end + 1])
                except json.JSONDecodeError:
                    logger.warning(f"JSON parse error: {e}")
                    return None
            else:
                logger.warning(f"JSON parse error: {e}")
                return None

        if not isinstance(data, dict):
            return None

        # Validate required fields
        missing = REQUIRED_FIELDS - set(data.keys())
        if missing:
            logger.warning(f"Response missing required fields: {missing}")
            return None

        # Default values for common optional fields
        data.setdefault("need_screen", True)
        data.setdefault("sleep_before_next", 0.0)
        data.setdefault("button", "left")

        return data
