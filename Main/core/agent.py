"""VictrlAgent main class — decision loop, state management, and action execution."""

import json
import logging
import time

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
        history_max_len: int = 10,
        dry_run: bool = False,
        hid_backend: str = "uinput",
        serial_port: str = "/dev/ttyUSB0",
    ):
        """Initialize the Victrl agent and all subsystems.

        Args:
            uvc_device: V4L2 device path for screen capture.
            screen_width: Target capture width.
            screen_height: Target capture height.
            api_endpoint: Model API endpoint URL.
            api_key: API authentication key.
            model_name: Model identifier string.
            max_actions: Hard limit on actions per task.
            plan_dir: Directory for plan JSON files.
            profile_dir: Directory for device profile.
            history_max_len: Max short-term memory entries.
            dry_run: If True, mock HID and model calls.
            hid_backend: "uinput" (default) or "serial" for ESP32.
            serial_port: Serial device path when hid_backend="serial".
        """
        self.max_actions = max_actions
        self.dry_run = dry_run
        self.hid_backend = hid_backend

        # Subsystems
        self.uvc = UvcCapture(device=uvc_device, width=screen_width, height=screen_height)

        # HID backend selection
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
        self.last_action: str | None = None

        logger.info("VictrlAgent initialized")

    def run(self, task_goal: str | None = None) -> None:
        """Main agent loop.

        Args:
            task_goal: Description of the task to accomplish.
        """
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

        logger.info(f"Starting task: {self.task_goal}")

        try:
            while self.running and self.action_count < self.max_actions:
                # Capture screen if needed
                img = None
                captured = False
                if self.need_screen:
                    try:
                        img = self.uvc.grab_frame()
                        captured = True
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

                # Query model
                resp = self.cloud.query(
                    image=img,
                    plan=self.plan_mgr.get_current_plan(),
                    history=self.short_term.get_last(5),
                    system_prompt=SYSTEM_PROMPT,
                    profile_text=self.profile_text,
                )

                if resp is None:
                    self.fail_count += 1
                    logger.warning(f"Step {self.action_count + 1}: Model query failed (#{self.fail_count})")
                    if self.fail_count >= 3:
                        logger.critical("Too many model failures, aborting.")
                        break
                    continue

                self.fail_count = 0
                self.action_count += 1

                action_type = resp.get("action_type", "?")
                observation = resp.get("observation", "")
                obs_short = observation[:80] + "..." if len(observation) > 80 else observation
                logger.info(
                    f"Step {self.action_count}: action={action_type}, "
                    f"observation=\"{obs_short}\""
                )

                # ── Completion verification ────────────────────────────────
                if resp.get("done") and not captured:
                    # Model claims done but didn't see the final screen.
                    # Force a verification round.
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
                        )
                        if resp is None or not resp.get("done"):
                            logger.info(f"Step {self.action_count}: Verification failed, continuing...")
                            if resp:
                                self.need_screen = resp.get("need_screen", True)
                            continue

                # Execute the action
                summary = self._execute_action(resp)
                self.last_action = summary

                # Update short-term memory
                self.short_term.add(summary)

                # Update plan
                if resp.get("plan_update"):
                    self.plan_mgr.save(resp["plan_update"])

                # Process profile updates
                for upd in resp.get("profile_updates", []):
                    content = upd.get("content", "")
                    if content:
                        self.profile_mgr.append_content(content)
                        self.profile_text = self.profile_mgr.load_full_text()

                # Set next-round flags
                self.need_screen = resp.get("need_screen", True)

                sleep_sec = resp.get("sleep_before_next", 0)
                if sleep_sec == 0 and not self.need_screen:
                    sleep_sec = 0.05
                if sleep_sec > 0:
                    time.sleep(sleep_sec)

                # Check for completion
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

    def _execute_action(self, resp: dict) -> str:
        """Execute a single action from the model response.

        Args:
            resp: Parsed model response dict.

        Returns:
            Semantic summary string of the action executed.
        """
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
                hold = resp.get("hold", -1)

                if from_box:
                    x, y = normalized_to_pixel(from_box, screen_w, screen_h)
                    self.hid.mouse_move_abs(x, y)
                    time.sleep(0.02)

                self.hid.mouse_down(button)
                time.sleep(0.05)

                if to_box:
                    x, y = normalized_to_pixel(to_box, screen_w, screen_h)
                    self.hid.mouse_move_abs(x, y)

                if hold != -1:
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
