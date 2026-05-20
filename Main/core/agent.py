"""VictrlAgent main class — decision loop, state management, and action execution."""

import json
import logging
import os
import time
from datetime import datetime

from core.cloud_client import CloudClient, MockCloudClient, SYSTEM_PROMPT
from core.hid_controller import HidController
from core.serial_hid import SerialHidBridge
from core.uvc_capture import UvcCapture
from memory.plan_manager import PlanManager
from memory.profile_manager import ProfileManager
from memory.short_term import ShortTermMemory
from utils.coordinates import normalized_to_pixel
from utils.exceptions import UvcError

logger = logging.getLogger("victrl.agent")

# Actions that change screen state and MUST be verified
_VERIFY_ACTIONS = {"click", "press", "type", "drag"}

# Actions that indicate a possible stuck loop (clicking/dragging same target repeatedly)
# press is excluded — consecutive different key presses (Tab, Enter, arrows) are normal
_REPETITION_WARN_ACTIONS = {"click", "type", "drag"}

# Minimum post-action delay (seconds) for state-changing actions,
# so the target computer has time to render the result before next capture
_MIN_POST_ACTION_DELAY = 0.5


class VictrlAgent:
    """Main agent orchestrating UVC capture, model query, and HID execution."""

    def __init__(
        self,
        uvc_device: str = "/dev/video0",
        screen_width: int = 1280,
        screen_height: int = 720,
        api_endpoint: str = "",
        api_key: str = "",
        model_name: str = "doubao-seed-2-0-mini-260428",
        max_actions: int = 200,
        plan_dir: str = "./plans",
        profile_dir: str = "./profiles",
        log_dir: str = "./log",
        history_max_len: int = 10,
        dry_run: bool = False,
        hid_backend: str = "uinput",
        serial_port: str = "/dev/ttyUSB0",
    ):
        self.max_actions = max_actions
        self.dry_run = dry_run
        self.hid_backend = hid_backend
        self.log_dir = log_dir

        # Subsystems
        self.uvc = UvcCapture(device=uvc_device, width=screen_width, height=screen_height)

        if hid_backend == "serial":
            self.hid = SerialHidBridge(port=serial_port, dry_run=dry_run)
        else:
            self.hid = HidController(dry_run=dry_run)
        self.hid.set_screen_size(screen_width, screen_height)

        if dry_run:
            self.cloud = MockCloudClient()
        else:
            self.cloud = CloudClient(
                api_endpoint=api_endpoint,
                api_key=api_key,
                model_name=model_name,
            )
        self.short_term = ShortTermMemory(max_len=history_max_len)
        self.plan_mgr = PlanManager(plan_dir=plan_dir)
        self.profile_mgr = ProfileManager(
            profile_dir=profile_dir,
            screen_width=screen_width,
            screen_height=screen_height,
        )

        # State
        self.profile_text = self.profile_mgr.load_full_text()
        self.running = False
        self.need_screen = True
        self.action_count = 0
        self.fail_count = 0
        self.task_goal: str | None = None
        self._last_summary = ""
        self._recent_action_types: list[str] = []  # for repetition detection
        self._task_log_path: str = ""
        self._task_log_fh = None
        self._img_dir: str = ""

        logger.info("VictrlAgent initialized")

    # ── Task log ──────────────────────────────────────────────────────────
    def _open_task_log(self) -> None:
        os.makedirs(self.log_dir, exist_ok=True)
        task_id = self.plan_mgr.task_id or time.strftime("%Y%m%d_%H%M%S")
        self._task_log_path = os.path.join(self.log_dir, f"{task_id}.log")
        self._task_log_fh = open(self._task_log_path, "w", encoding="utf-8")
        self._task_log_fh.write(f"# Victrl Task Log\n")
        self._task_log_fh.write(f"# Task: {self.task_goal}\n")
        self._task_log_fh.write(f"# Started: {datetime.now().isoformat()}\n")
        self._task_log_fh.write(f"# {'=' * 60}\n\n")
        self._task_log_fh.flush()

        self._img_dir = os.path.join(self.log_dir, f"{task_id}_img")
        os.makedirs(self._img_dir, exist_ok=True)

    @staticmethod
    def _action_details(resp: dict, screen_w: int, screen_h: int) -> str:
        """Build a human-readable summary of the action to be executed."""
        atype = resp.get("action_type", "?")
        lines = [f"action_type: {atype}"]

        box = resp.get("box_2d")
        if box:
            cx, cy = normalized_to_pixel(box, screen_w, screen_h)
            lines.append(f"box_2d: {box} → pixel_center=({cx}, {cy})")

        from_box = resp.get("from_box")
        to_box = resp.get("to_box")
        if from_box:
            fx, fy = normalized_to_pixel(from_box, screen_w, screen_h)
            lines.append(f"from_box: {from_box} → pixel=({fx}, {fy})")
        if to_box:
            tx, ty = normalized_to_pixel(to_box, screen_w, screen_h)
            lines.append(f"to_box: {to_box} → pixel=({tx}, {ty})")

        button = resp.get("button")
        if button and button != "left":  # left is default, only log non-default
            lines.append(f"button: {button}")

        key = resp.get("key")
        if key:
            lines.append(f"key: {key}")

        text = resp.get("text")
        if text:
            lines.append(f"text: {repr(text[:80])}")

        delta_x = resp.get("delta_x", 0)
        delta_y = resp.get("delta_y", 0)
        if delta_x or delta_y:
            lines.append(f"scroll: dx={delta_x}, dy={delta_y}")

        wait = resp.get("wait_seconds")
        if wait:
            lines.append(f"wait: {wait}s")

        sleep_after = resp.get("sleep_before_next")
        if sleep_after:
            lines.append(f"sleep_after: {sleep_after}s")

        return "\n  ".join(lines)

    def _log_step(self, step_no: int, captured: bool, system_prompt: str,
                  user_text: str, has_image: bool, resp: dict) -> None:
        """Write full step details to the per-task log file."""
        if not self._task_log_fh:
            return
        fh = self._task_log_fh

        fh.write(f"\n{'=' * 70}\n")
        fh.write(f"STEP {step_no} | {datetime.now().isoformat()}\n")
        fh.write(f"Screen: {'captured' if captured else 'skipped'} | "
                 f"Image attached: {has_image}\n")

        # Action details with pixel coordinates
        fh.write(f"ACTION: ")
        fh.write(self._action_details(resp, self.uvc.width, self.uvc.height))
        fh.write(f"\n{'=' * 70}\n\n")

        fh.write("─── SYSTEM PROMPT ──────────────────────────────\n")
        fh.write(system_prompt)
        fh.write("\n\n─── USER PROMPT ────────────────────────────────\n")
        fh.write(user_text)
        fh.write("\n\n─── MODEL REASONING ────────────────────────────\n")
        reasoning = resp.get("_reasoning", "")
        fh.write(reasoning if reasoning else "(no reasoning content returned)")
        fh.write("\n\n─── RAW RESPONSE ───────────────────────────────\n")
        raw = resp.get("_raw_content", "")
        fh.write(raw if raw else "(no raw content)")
        fh.write("\n\n─── PARSED VALUES ──────────────────────────────\n")
        for key in sorted(resp.keys()):
            if key.startswith("_"):
                continue
            val = resp[key]
            if isinstance(val, (dict, list)):
                val = json.dumps(val, ensure_ascii=False, indent=2)
                fh.write(f"\n  {key}:\n")
                for line in val.split("\n"):
                    fh.write(f"    {line}\n")
            else:
                fh.write(f"  {key}: {val}\n")
        fh.write(f"\n{'=' * 70}\n")
        fh.flush()

    def _close_task_log(self) -> None:
        if self._task_log_fh:
            self._task_log_fh.write(f"\n# Task ended: {datetime.now().isoformat()}\n")
            self._task_log_fh.close()
            self._task_log_fh = None
            logger.info(f"Task log saved: {self._task_log_path}")

    # ── Main loop ─────────────────────────────────────────────────────────
    def run(self, task_goal: str | None = None) -> None:
        """Main agent loop."""
        if task_goal:
            self.task_goal = task_goal
            self.plan_mgr.new_task(task_goal)
        elif self.plan_mgr.task_id is None:
            logger.error("No task goal provided and no plan to resume.")
            return

        self.running = True
        self.action_count = 0
        self.fail_count = 0
        self.need_screen = True
        self._last_summary = ""
        self._recent_action_types = []

        self._open_task_log()
        logger.info(f"Starting task: {self.task_goal}")

        try:
            while self.running and self.action_count < self.max_actions:
                # ── Capture screen ─────────────────────────────────────
                img = None
                captured = False
                if self.need_screen:
                    try:
                        img = self.uvc.grab_frame()
                        captured = True
                        if self._img_dir and img is not None:
                            ts = datetime.now().strftime("%H%M%S")
                            step_img_path = os.path.join(
                                self._img_dir,
                                f"step_{self.action_count + 1:04d}_{ts}.jpg",
                            )
                            img.save(step_img_path, "JPEG", quality=90)
                            logger.info(f"Saved: {step_img_path}")
                    except UvcError as e:
                        logger.error(f"Step {self.action_count + 1}: Screen capture failed: {e}")
                        self.fail_count += 1
                        if self.fail_count >= 3:
                            logger.critical("Too many capture failures, aborting.")
                            break
                        time.sleep(1)
                        continue

                logger.info(
                    f"Step {self.action_count + 1}: "
                    f"screen={'captured' if captured else 'skipped'}, "
                    f"querying model..."
                )

                # ── Query model ────────────────────────────────────────
                plan = self.plan_mgr.get_current_plan()
                history = self.short_term.get_last(5)

                # Build what we'll log as the user prompt text
                plan_text = json.dumps(plan, ensure_ascii=False, indent=2) if plan else "None"
                history_text = "\n".join(history) if history else "(none)"
                user_text = (
                    f"Current plan:\n{plan_text}\n\n"
                    f"Recent action history (last {len(history)}):\n{history_text}\n\n"
                    f"Last summary for self-evaluation: {self._last_summary or '(first step)'}"
                )

                system_content = SYSTEM_PROMPT.format(
                    device_profile_text=self.profile_text,
                    pressed_buttons="[]",
                    last_summary=self._last_summary or "(This is the first step — no prior action to evaluate.)",
                )

                resp = self.cloud.query(
                    image=img,
                    plan=plan,
                    history=history,
                    system_prompt=SYSTEM_PROMPT,
                    profile_text=self.profile_text,
                    last_summary=self._last_summary,
                )

                if resp is None:
                    self.fail_count += 1
                    logger.warning(f"Step {self.action_count + 1}: Model query failed (#{self.fail_count})")
                    if self.fail_count >= 3:
                        logger.critical("Too many model failures, aborting.")
                        break
                    backoff = min(2 ** self.fail_count, 30)  # 2s, 4s, 8s... capped at 30s
                    logger.info(f"Backing off {backoff}s before retry")
                    time.sleep(backoff)
                    self.need_screen = True  # re-capture after waiting
                    continue

                self.fail_count = 0
                self.action_count += 1

                # ── Log step details to task log ───────────────────────
                self._log_step(
                    step_no=self.action_count,
                    captured=captured,
                    system_prompt=system_content,
                    user_text=user_text,
                    has_image=img is not None,
                    resp=resp,
                )

                # ── Log key info to console ────────────────────────────
                action_type = resp.get("action_type", "?")
                observation = resp.get("observation", "")
                self_eval = resp.get("self_evaluation", "")
                obs_short = observation[:80] + "..." if len(observation) > 80 else observation
                action_detail = self._action_details(resp, self.uvc.width, self.uvc.height)
                logger.info(
                    f"Step {self.action_count}: action={action_type}, "
                    f"obs=\"{obs_short}\""
                )
                logger.info(f"  detail: {action_detail}")
                if self_eval:
                    logger.info(f"  self_eval: \"{self_eval[:120]}\"")

                # ── Completion verification ────────────────────────────
                if resp.get("done") and not captured:
                    logger.info(f"Step {self.action_count}: Model reports done, "
                                "forcing screen capture for verification...")
                    try:
                        img = self.uvc.grab_frame()
                        captured = True
                    except UvcError:
                        logger.warning("Verification capture failed, accepting done anyway")
                    if captured:
                        resp = self.cloud.query(
                            image=img,
                            plan=self.plan_mgr.get_current_plan(),
                            history=self.short_term.get_last(5),
                            system_prompt=SYSTEM_PROMPT,
                            profile_text=self.profile_text,
                            last_summary=self._last_summary,
                        )
                        if resp is None or not resp.get("done"):
                            logger.info(f"Step {self.action_count}: Verification failed, continuing...")
                            if resp:
                                self.need_screen = resp.get("need_screen", True)
                            continue

                # ── Repetition detection ───────────────────────────────
                self._recent_action_types.append(action_type)
                if len(self._recent_action_types) > 5:
                    self._recent_action_types.pop(0)

                # Check if last 3 actions are identical
                if len(self._recent_action_types) >= 3:
                    last3 = self._recent_action_types[-3:]
                    if len(set(last3)) == 1 and last3[0] in _REPETITION_WARN_ACTIONS:
                        logger.warning(
                            f"Step {self.action_count}: Repetition detected — "
                            f"3 consecutive '{last3[0]}' actions. Model may be stuck."
                        )

                # ── Execute action ─────────────────────────────────────
                summary = self._execute_action(resp)

                # ── Update memories ────────────────────────────────────
                self.short_term.add(summary)
                self._last_summary = resp.get("plan_update", {}).get("summary", summary)

                if resp.get("plan_update"):
                    self.plan_mgr.save(resp["plan_update"])

                for upd in resp.get("profile_updates", []):
                    content = upd.get("content", "")
                    if content:
                        self.profile_mgr.append_content(content)
                        self.profile_text = self.profile_mgr.load_full_text()

                # ── Force screen after critical actions ────────────────
                if action_type in _VERIFY_ACTIONS:
                    self.need_screen = True
                else:
                    self.need_screen = resp.get("need_screen", True)

                sleep_sec = resp.get("sleep_before_next", 0)
                # Enforce minimum delay after state-changing actions so the
                # target computer has time to process HID input and render.
                if action_type in _VERIFY_ACTIONS and sleep_sec < _MIN_POST_ACTION_DELAY:
                    sleep_sec = _MIN_POST_ACTION_DELAY
                if sleep_sec == 0 and not self.need_screen:
                    sleep_sec = 0.05
                if sleep_sec > 0:
                    time.sleep(sleep_sec)

                # ── Check for completion ───────────────────────────────
                if resp.get("done"):
                    verification = resp.get("verification", "")
                    logger.info(
                        f"Task complete after {self.action_count} actions. "
                        f"Verification: \"{verification[:120]}\""
                    )
                    break

        except KeyboardInterrupt:
            logger.info("Interrupted by user")
        except Exception as e:
            logger.exception(f"Unexpected error in main loop: {e}")
        finally:
            self.running = False
            self.hid.release_all()
            self._close_task_log()
            logger.info("Agent loop ended.")

    def stop(self) -> None:
        """Signal the agent to stop and release HID resources."""
        logger.info("Stop requested.")
        self.running = False
        try:
            self.hid.release_all()
        except Exception as e:
            logger.error(f"Error during HID release: {e}")
        try:
            self.uvc.release()
        except Exception:
            pass
        self._close_task_log()

    # ── Action execution ──────────────────────────────────────────────────
    def _execute_action(self, resp: dict) -> str:
        """Execute a single action from the model response."""
        action_type = resp.get("action_type", "wait")
        screen_w = self.uvc.width
        screen_h = self.uvc.height

        try:
            if action_type == "click":
                button = resp.get("button", "left")
                box = resp.get("box_2d")
                if box:
                    x, y = normalized_to_pixel(box, screen_w, screen_h)
                    self.hid.mouse_move_abs(x, y)
                    time.sleep(0.02)
                self.hid.mouse_click(button)
                return f"Clicked {button} at {box}"

            elif action_type == "move":
                box = resp.get("box_2d")
                if box:
                    x, y = normalized_to_pixel(box, screen_w, screen_h)
                    self.hid.mouse_move_abs(x, y)
                return f"Moved mouse to {box}"

            elif action_type == "drag":
                button = resp.get("button", "left")
                from_box = resp.get("from_box")
                to_box = resp.get("to_box")
                hold = resp.get("hold", 0)
                if from_box:
                    x1, y1 = normalized_to_pixel(from_box, screen_w, screen_h)
                    self.hid.mouse_move_abs(x1, y1)
                    time.sleep(0.02)
                else:
                    x1 = y1 = 0
                self.hid.mouse_down(button)
                time.sleep(0.05)
                if to_box:
                    x2, y2 = normalized_to_pixel(to_box, screen_w, screen_h)
                    # Smooth drag in small steps so the host sees continuous
                    # movement (required for desktop selection boxes etc.)
                    steps = 8
                    for i in range(1, steps + 1):
                        tx = x1 + (x2 - x1) * i // steps
                        ty = y1 + (y2 - y1) * i // steps
                        self.hid.mouse_move_abs(tx, ty)
                        time.sleep(0.005)
                if hold >= 0:
                    if hold > 0:
                        time.sleep(hold / 1000.0)
                    self.hid.mouse_up(button)
                return f"Dragged {button} from {from_box} to {to_box}"

            elif action_type == "scroll":
                dx = resp.get("delta_x", 0)
                dy = resp.get("delta_y", 0)
                self.hid.mouse_scroll(delta_x=dx, delta_y=dy)
                return f"Scrolled dx={dx} dy={dy}"

            elif action_type == "press":
                key = resp.get("key", "")
                # Normalize: model sometimes outputs "win r" instead of "win+r"
                key = "+".join(k.strip() for k in key.replace(" ", "+").split("+") if k.strip())
                self.hid.key_press(key)
                return f"Pressed {key}"

            elif action_type == "type":
                text = resp.get("text", "")
                self.hid.type_string(text)
                return f"Typed {repr(text[:50])}..."

            elif action_type == "wait":
                secs = resp.get("wait_seconds", 1.0)
                time.sleep(max(secs, 0.05))
                return f"Waited {secs}s"

            elif action_type == "release":
                button = resp.get("button", "left")
                self.hid.mouse_up(button)
                return f"Released {button}"

            elif action_type == "complete":
                msg = resp.get("message", "Task completed.")
                logger.info(f"Task complete: {msg}")
                return f"Completed: {msg}"

            elif action_type == "error":
                msg = resp.get("message", "Unknown error.")
                logger.error(f"Task error: {msg}")
                return f"Error: {msg}"

            elif action_type == "call_skill":
                logger.warning("Skill execution not implemented in MVP")
                return "Attempted skill call (not implemented)"

            else:
                logger.warning(f"Unknown action_type: {action_type}")
                return f"Unknown action: {action_type}"

        except Exception as e:
            logger.error(f"Action execution failed ({action_type}): {e}")
            try:
                self.hid.release_all()
            except Exception:
                pass
            return f"Failed {action_type}: {e}"
