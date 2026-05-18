"""L3 Long-term memory: device profile (natural language Markdown) manager."""

import logging
import os
import time

from config import PROFILE_DIR, PROFILE_FILE

logger = logging.getLogger("victrl.memory.profile")


class ProfileManager:
    """Manages the device profile as a natural language Markdown file.

    Supports reading, appending, and future section-based updates.
    """

    def __init__(
        self,
        profile_dir: str = PROFILE_DIR,
        profile_file: str = PROFILE_FILE,
        screen_width: int = 1280,
        screen_height: int = 720,
    ):
        """Initialize profile manager.

        Args:
            profile_dir: Directory containing profile files.
            profile_file: Filename for the main profile.
            screen_width: Default screen width (used in default template).
            screen_height: Default screen height (used in default template).
        """
        self.profile_dir = profile_dir
        self.profile_file = profile_file
        self.profile_path = os.path.join(profile_dir, profile_file)
        self._default_content = (
            "# Device Profile\n"
            "- Operating System: Unknown (to be learned)\n"
            f"- Screen resolution: {screen_width}x{screen_height}\n"
            "- Known UI elements: none yet\n"
            "- Shortcuts: none yet\n"
            "- Lessons: none yet\n"
        )
        self._ensure_exists()

    def _ensure_exists(self) -> None:
        """Create profile directory and default file if they don't exist."""
        os.makedirs(self.profile_dir, exist_ok=True)
        if not os.path.exists(self.profile_path):
            try:
                with open(self.profile_path, "w", encoding="utf-8") as f:
                    f.write(self._default_content)
                logger.info(f"Created default profile: {self.profile_path}")
            except OSError as e:
                logger.error(f"Failed to create profile: {e}")

    def load_full_text(self) -> str:
        """Read the entire profile Markdown file.

        Returns:
            Full content of the profile file as a string.
        """
        try:
            with open(self.profile_path, "r", encoding="utf-8") as f:
                return f.read()
        except FileNotFoundError:
            logger.warning("Profile file not found, returning default.")
            return self._default_content
        except OSError as e:
            logger.error(f"Failed to read profile: {e}")
            return self._default_content

    def append_content(self, content: str) -> None:
        """Append new content to the profile file.

        Args:
            content: Text to append (preceded by a blank line and timestamp).
        """
        if not content.strip():
            return

        timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
        entry = f"\n\n[{timestamp}] {content.strip()}"
        try:
            with open(self.profile_path, "a", encoding="utf-8") as f:
                f.write(entry)
            logger.info(f"Profile updated: {content[:80]}...")
        except OSError as e:
            logger.error(f"Failed to append to profile: {e}")

    def update_section(self, section_name: str, content: str) -> None:
        """Reserved: update a specific markdown section by heading.

        Args:
            section_name: Name of the section (without '#').
            content: New content for the section.
        """
        logger.info(f"Section update not implemented in MVP: {section_name}")
