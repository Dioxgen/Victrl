"""L2 Medium-term memory: plan file management with checkpoint support."""

import json
import logging
import os
import time

from config import PLAN_DIR
from utils.exceptions import PlanError

logger = logging.getLogger("victrl.memory.plan")


class PlanManager:
    """Manages task plans stored as JSON files, supporting checkpoint/resume."""

    def __init__(self, plan_dir: str = PLAN_DIR):
        """Initialize plan manager.

        Args:
            plan_dir: Directory to store plan JSON files.
        """
        self.plan_dir = plan_dir
        os.makedirs(self.plan_dir, exist_ok=True)
        self._current_plan: dict | None = None
        self._task_id: str | None = None

    @property
    def task_id(self) -> str | None:
        """Current task identifier."""
        return self._task_id

    def new_task(self, task_goal: str) -> str:
        """Create a new task ID from timestamp.

        Args:
            task_goal: The user's task description.

        Returns:
            Generated task ID string.
        """
        self._task_id = time.strftime("%Y%m%d_%H%M%S")
        self._current_plan = {
            "task_id": self._task_id,
            "goal": task_goal,
            "created_at": time.strftime("%Y-%m-%d %H:%M:%S"),
            "updated_at": time.strftime("%Y-%m-%d %H:%M:%S"),
            "summary": f"Task: {task_goal}",
            "current_milestone": 0,
            "milestones": [],
        }
        logger.info(f"New task created: {self._task_id}")
        return self._task_id

    def save(self, plan_dict: dict | None = None) -> str:
        """Save the current plan to a JSON file.

        Args:
            plan_dict: Optional plan dict to save. Uses self._current_plan if None.

        Returns:
            Path to the saved file.

        Raises:
            PlanError: If no task ID is set.
        """
        if self._task_id is None:
            raise PlanError("No task ID set. Call new_task() first.")

        if plan_dict is not None:
            self._current_plan = plan_dict

        if self._current_plan:
            self._current_plan["updated_at"] = time.strftime("%Y-%m-%d %H:%M:%S")

        filepath = os.path.join(self.plan_dir, f"{self._task_id}.json")
        try:
            with open(filepath, "w", encoding="utf-8") as f:
                json.dump(self._current_plan, f, ensure_ascii=False, indent=2)
        except OSError as e:
            raise PlanError(f"Failed to save plan: {e}")

        logger.debug(f"Plan saved: {filepath}")
        return filepath

    def load(self, task_id: str) -> dict | None:
        """Load a plan from JSON file (for resuming).

        Args:
            task_id: The task identifier to load.

        Returns:
            Plan dict, or None if not found.
        """
        filepath = os.path.join(self.plan_dir, f"{task_id}.json")
        try:
            with open(filepath, "r", encoding="utf-8") as f:
                self._current_plan = json.load(f)
            self._task_id = task_id
            logger.info(f"Plan loaded: {task_id}")
            return self._current_plan
        except FileNotFoundError:
            logger.warning(f"Plan not found: {task_id}")
            return None
        except (json.JSONDecodeError, OSError) as e:
            raise PlanError(f"Failed to load plan: {e}")

    def list_plans(self) -> list[str]:
        """List all available plan task IDs.

        Returns:
            List of task ID strings.
        """
        plans = []
        for f in os.listdir(self.plan_dir):
            if f.endswith(".json"):
                plans.append(f[:-5])
        return sorted(plans)

    def get_current_plan(self) -> dict | None:
        """Return the current in-memory plan."""
        return self._current_plan
