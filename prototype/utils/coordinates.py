"""Coordinate conversion utilities."""

from typing import Tuple, Union


def normalized_to_pixel(
    box_2d: list, screen_width: int, screen_height: int
) -> Tuple[int, int]:
    """Convert normalized [ymin, xmin, ymax, xmax] to pixel center (x, y).

    Args:
        box_2d: Normalized bounding box [ymin, xmin, ymax, xmax] with values in [0, 1].
        screen_width: Screen width in pixels.
        screen_height: Screen height in pixels.

    Returns:
        Tuple of (pixel_x, pixel_y) representing the center of the box.
    """
    ymin, xmin, ymax, xmax = box_2d
    center_x = int((xmin + xmax) / 2 * screen_width)
    center_y = int((ymin + ymax) / 2 * screen_height)
    return center_x, center_y


def normalized_rect_to_pixel(
    box_2d: list, screen_width: int, screen_height: int
) -> Tuple[int, int, int, int]:
    """Convert normalized [ymin, xmin, ymax, xmax] to pixel rect (x1, y1, x2, y2).

    Args:
        box_2d: Normalized bounding box.
        screen_width: Screen width in pixels.
        screen_height: Screen height in pixels.

    Returns:
        Tuple of (x1, y1, x2, y2) in pixel coordinates.
    """
    ymin, xmin, ymax, xmax = box_2d
    x1 = int(xmin * screen_width)
    y1 = int(ymin * screen_height)
    x2 = int(xmax * screen_width)
    y2 = int(ymax * screen_height)
    return x1, y1, x2, y2
