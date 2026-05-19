"""Cloud model client for calling multimodal LLM APIs via Volces Ark Responses API."""

import base64
import io
import json
import logging
import time

from PIL import Image
from volcenginesdkarkruntime import Ark

from config import API_MAX_RETRIES, API_TIMEOUT

logger = logging.getLogger("victrl.cloud")

SYSTEM_PROMPT = """You are Victrl, a hardware AI agent controlling a computer via HID. You see the screen via UVC. Your task is to fulfill the user's goal by outputting JSON actions.

**Device Profile:**
{device_profile_text}

**Pressed buttons:** {pressed_buttons}

**Your last action and expected outcome:**
{last_summary}

**Your output must be a JSON object:**
{{
  "action_type": "click|move|drag|scroll|press|type|wait|release|complete|error",
  "box_2d": [ymin, xmin, ymax, xmax],
  "from_box": [ymin, xmin, ymax, xmax],
  "to_box": [ymin, xmin, ymax, xmax],
  "button": "left"|"right"|"middle"|"double_left",
  "hold": int,
  "delta_y": int,
  "delta_x": int,
  "key": "combo separated by + (e.g. win+r, ctrl+c, ctrl+shift+esc) — NEVER use spaces",
  "text": "string",
  "wait_seconds": float,
  "message": "string",
  "need_screen": bool,
  "sleep_before_next": float,
  "observation": "string",
  "self_evaluation": "string",
  "plan_update": {{
    "summary": "What I did this step + what I expected + whether it worked",
    "current_milestone": int,
    "milestones": [{{"id": int, "description": "high-level sub-goal, NOT a specific UI action", "status": "pending|in_progress|done"}}]
  }},
  "profile_updates": [{{"content": "string"}}],
  "done": bool,
  "verification": "string"
}}

─── CRITICAL RULES (violating these causes task failure) ───

1. LOOK BEFORE YOU LEAP.
   - If you do NOT have a current screen image, set need_screen: true and output a wait action. NEVER guess.
   - After EVERY state-changing action (click, press, type, drag), you MUST set need_screen: true so the next step verifies the result. Only skip the screen for pure wait/scroll actions where the outcome is predictable.

   ⚠️ CRITICAL — LEAVE TIME FOR THE COMPUTER TO REACT:
   - The screen is captured IMMEDIATELY after your sleep_before_next expires. If the computer hasn't finished rendering, you will see a STALE screen and wrongly conclude your action failed.
   - Set sleep_before_next based on what you just did:
     * click (menu, button):          0.3 – 0.5s
     * press (keyboard shortcut):     0.3 – 0.8s
     * press (launching an app):      1.0 – 2.0s
     * type (short text <20 chars):   0.2 – 0.4s
     * type (longer text):            0.4 – 0.8s
     * drag:                          0.3 – 0.5s
     * wait / scroll:                 0.1 – 0.3s  (predictable outcome, fine to skip screen)
   - If the screen shows your action had NO effect, FIRST consider: "was the screen captured too early?" Before declaring failure, try the same action with a LONGER sleep_before_next. Only conclude the action truly failed after a second attempt with adequate delay.

2. ⚠️ WATCH FOR INPUT METHOD (IME) CORRUPTION.
   - If the target system has a non-English IME active (e.g. Chinese 输入法), typing English produces garbled text (pinyin interpretation, full-width punctuation).
   - After a "type" action, VERIFY the text on screen matches what you intended. If characters are wrong, wrong-width, or missing: suspect IME interference.
   - Common fixes: toggle input mode (Win+Space, Alt+Shift, Ctrl+Shift), press Esc to cancel IME composition, or use Ctrl+Space. Adapt based on what you see on screen.

3. SELF-EVALUATE EVERY STEP.
   - The "self_evaluation" field is REQUIRED. Compare: did my last action produce the expected result?
   - If the screen shows your last action FAILED (wrong window, no response, unexpected dialog): DO NOT repeat the same action. Diagnose the problem and try a different approach.
   - The "summary" in plan_update must describe: (a) what you just did, (b) what you expected, (c) whether the screen confirmed it worked.

4. DETECT AND BREAK LOOPS.
   - If you have performed 3+ similar actions with NO visible progress toward the goal, you are STUCK. Do NOT try the same thing again. Step back, re-examine the screen, and formulate a completely different strategy.
   - Examples of being stuck: clicking the same area repeatedly, typing the same text multiple times, pressing the same key combo that isn't working.
   - When stuck: mark the current milestone as "blocked", add a new milestone describing the recovery approach, and EXPLICITLY state in self_evaluation what went wrong.

5. SCREEN IS THE ONLY TRUTH.
   - The plan is a compass, not a script. If the screen contradicts the plan, BELIEVE THE SCREEN.
   - If a milestone was marked "done" but the screen shows otherwise, revert it to "in_progress" or "blocked".
   - Never skip need_screen just because the plan says something should have happened.

6. VERIFY COMPLETION RIGOROUSLY.
   - Before outputting done: true, you MUST have a fresh screen capture showing the final state.
   - The "verification" field must list specific UI elements visible on screen that prove success.
   - If ANY part of the goal is unconfirmed, continue with corrective actions instead of declaring done.

─── STANDARD RULES ───
- Coordinates normalized [0,1], 3 decimal places.
- Always fill "observation" (what you see on screen right now) and "self_evaluation" (did last action work?).
- Always set sleep_before_next — never leave it at 0 after state-changing actions. The host enforces a 0.5s minimum for click/press/type/drag, but you should set LONGER delays for slow operations (app launch, dialog open, page load).
- Milestones describe sub-goals (e.g. "Open the target application"), NOT specific clicks.
- For unrecoverable error, action_type: "error", done: true.
- Use profile_updates to record UI locations and shortcuts you discover.
"""

REQUIRED_FIELDS = {"action_type", "plan_update"}

# ── Mock responses for dry-run ──────────────────────────────────────────
_MOCK_RESPONSES = [
    {
        "action_type": "click",
        "box_2d": [0.10, 0.50, 0.15, 0.55],
        "button": "left",
        "need_screen": True,
        "sleep_before_next": 0.5,
        "observation": "Desktop visible with taskbar at bottom. Start button in lower-left corner. No windows open.",
        "self_evaluation": "First action — no prior action to evaluate. Start menu should appear after this click.",
        "plan_update": {
            "summary": "Clicked Windows Start button. Expected: Start menu opens with search field visible. Will verify next step.",
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
        "observation": "Start menu is open. Search field is focused with cursor blinking. Ready for text input.",
        "self_evaluation": "Previous click succeeded — Start menu opened as expected. Now typing the app name.",
        "plan_update": {
            "summary": "Typed 'notepad' into Start search. Expected: Notepad appears as top result. Will verify with screen capture.",
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
        "observation": "Notepad is the top search result, highlighted. Search text 'notepad' is visible.",
        "self_evaluation": "Typing worked — 'notepad' text appeared and search results are visible. Enter will launch it.",
        "plan_update": {
            "summary": "Pressed Enter to launch Notepad. Expected: Notepad window opens. Will verify after allowing launch time.",
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
        "observation": "Notepad window is open and active. Title bar shows 'Untitled - Notepad'. Empty document with cursor blinking.",
        "self_evaluation": "Enter key worked — Notepad launched successfully. Window is focused and ready for typing.",
        "plan_update": {
            "summary": "Typing greeting text into Notepad. Expected: text appears in the editor. Will verify content on screen.",
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
        "observation": "Notepad window active. Editor area shows 'Hello from Victrl!' text. Title bar: 'Untitled - Notepad'.",
        "self_evaluation": "Text was typed and is visible in Notepad. All milestones are done. Screen confirms success.",
        "verification": "Notepad title bar confirms the app is open. Editor area contains the target text 'Hello from Victrl!' clearly visible. All three milestones completed.",
        "plan_update": {
            "summary": "Task complete. Notepad launched, greeting typed, result verified on screen.",
            "current_milestone": 3,
            "milestones": [
                {"id": 1, "description": "Open the target application", "status": "done"},
                {"id": 2, "description": "Perform the requested operation", "status": "done"},
                {"id": 3, "description": "Verify the result", "status": "done"},
            ],
        },
        "profile_updates": [
            {"content": "- Start menu opens via left-click on Windows icon at [0.10, 0.50, 0.15, 0.55]"},
            {"content": "- Start search field auto-focuses and accepts text input immediately after menu opens"},
            {"content": "- Notepad launches within ~1 second after pressing Enter on search result"},
        ],
        "done": True,
    },
]


class MockCloudClient:
    """Mock client that returns canned responses for dry-run testing."""

    def __init__(self):
        self._step = 0
        logger.info("MockCloudClient initialized — no real API calls will be made")

    def query(self, image=None, plan=None, history=None, system_prompt="", profile_text="", last_summary="") -> dict | None:
        """Return the next canned response, cycling through the mock sequence."""
        idx = self._step
        self._step += 1
        if idx >= len(_MOCK_RESPONSES):
            idx = _MOCK_RESPONSES.index(
                next(r for r in _MOCK_RESPONSES if r.get("done"))
            )
        resp = dict(_MOCK_RESPONSES[idx])
        resp.setdefault("_reasoning", "")
        logger.info(f"[MOCK] query #{idx}: action_type={resp.get('action_type')}")
        return resp


class CloudClient:
    """Client for calling multimodal LLM APIs via Volces Ark Responses API."""

    def __init__(self, api_endpoint: str, api_key: str, model_name: str):
        self.api_endpoint = api_endpoint.rstrip("/")
        self.api_key = api_key
        self.model_name = model_name
        self._client = Ark(
            base_url=self.api_endpoint,
            api_key=self.api_key,
        )
        logger.info(f"CloudClient initialized: endpoint={api_endpoint}, model={model_name}")

    def query(
        self,
        image: Image.Image | None,
        plan: dict | None,
        history: list,
        system_prompt: str,
        profile_text: str,
        last_summary: str = "",
    ) -> dict | None:
        """Send a query to the model and parse the response.

        Returns:
            Parsed response dict (with extra '_reasoning' and '_raw_content' keys),
            or None on failure.
        """
        instructions = system_prompt.format(
            device_profile_text=profile_text,
            pressed_buttons="[]",
            last_summary=last_summary or "(This is the first step — no prior action to evaluate.)",
        )

        plan_summary = json.dumps(plan, ensure_ascii=False, indent=2) if plan else "No plan yet. Create one."
        history_text = "\n".join(history) if history else "No history yet."

        user_text = (
            f"Current plan:\n{plan_summary}\n\n"
            f"Recent action history (last {len(history)}):\n{history_text}\n\n"
            f"TASK: Analyze the screen, evaluate whether your last action worked, "
            f"and output the next action as JSON.\n"
            f"Remember: fill self_evaluation (did the last action succeed?), "
            f"and set need_screen: true after any click/press/type/drag."
        )

        content_blocks = [{"type": "input_text", "text": user_text}]

        if image is not None:
            buf = io.BytesIO()
            image.save(buf, format="JPEG", quality=85)
            img_b64 = base64.b64encode(buf.getvalue()).decode("ascii")
            content_blocks.append({
                "type": "input_image",
                "image_url": f"data:image/jpeg;base64,{img_b64}",
            })

        user_message = {
            "role": "user",
            "content": content_blocks,
        }

        last_error = None
        for attempt in range(API_MAX_RETRIES + 1):
            try:
                response = self._client.responses.create(
                    model=self.model_name,
                    instructions=instructions,
                    input=[user_message],
                    temperature=0.1,
                    max_output_tokens=4096,
                )

                # Extract text content from response output
                raw_content = ""
                reasoning = ""
                for item in response.output:
                    if item.type == "reasoning":
                        for s in item.summary:
                            reasoning += s.text
                    elif item.type == "message":
                        for c in item.content:
                            if c.type == "output_text":
                                raw_content += c.text

                parsed = self._parse_response(raw_content)
                if parsed is not None:
                    parsed["_reasoning"] = reasoning
                    parsed["_raw_content"] = raw_content
                    return parsed

                logger.warning(f"Response parse failed, attempt {attempt + 1}")
                if attempt < API_MAX_RETRIES:
                    user_text = (
                        "Your previous response was missing required fields "
                        f"({', '.join(sorted(REQUIRED_FIELDS))}). "
                        "Output a COMPLETE JSON object. All fields are required.\n\n"
                    ) + user_text
                    content_blocks[0]["text"] = user_text
                    time.sleep(1)

            except Exception as e:
                last_error = str(e)
                logger.warning(f"API error on attempt {attempt + 1}: {e}")
                if attempt < API_MAX_RETRIES:
                    delay = min(2 ** attempt, 60)
                    time.sleep(delay)
                continue

        logger.error(f"All {API_MAX_RETRIES + 1} attempts failed. Last error: {last_error}")
        return None

    def _parse_response(self, content: str) -> dict | None:
        """Parse JSON from model response text."""
        if not content:
            return None

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
        except json.JSONDecodeError:
            start = text.find("{")
            end = text.rfind("}")
            if start != -1 and end != -1 and end > start:
                try:
                    data = json.loads(text[start:end + 1])
                except json.JSONDecodeError:
                    logger.warning(f"JSON parse error in response")
                    return None
            else:
                logger.warning(f"No JSON object found in response")
                return None

        if not isinstance(data, dict):
            return None

        missing = REQUIRED_FIELDS - set(data.keys())
        if missing:
            logger.warning(f"Response missing required fields: {missing}")
            return None

        data.setdefault("need_screen", True)
        data.setdefault("sleep_before_next", 0.0)
        data.setdefault("button", "left")
        data.setdefault("done", False)

        return data
