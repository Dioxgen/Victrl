"""Custom exception classes for Victrl."""


class VictrlError(Exception):
    """Base exception for all Victrl errors."""
    pass


class UvcError(VictrlError):
    """Raised when UVC capture fails."""
    pass


class HidError(VictrlError):
    """Raised when HID operation fails."""
    pass


class CloudAPIError(VictrlError):
    """Raised when cloud API call fails."""
    pass


class PlanError(VictrlError):
    """Raised when plan file operations fail."""
    pass
