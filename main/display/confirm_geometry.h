#ifndef CONFIRM_GEOMETRY_H
#define CONFIRM_GEOMETRY_H

#include <cstdint>

// Confirm screen layout for the 360x360 round panel, shared by the display
// (drawing) and the touch task (hit-testing) so the two can never drift apart.
// Everything stays inside the circle: the accent ring overlay paints radius
// 172-180 over whatever the engine rendered, and the buttons' worst corner
// (300,296) sits at ~167 from center.
namespace confirm_geometry {

// Three wrapped lines of the 20 px font; anything longer clips.
constexpr int kSummaryOffsetY = 78;
constexpr int kSummaryWidth = 250;
constexpr int kSummaryHeight = 90;

// gfx_label has no vertical centering — text draws at the top of its box —
// so the visible button is only slightly taller than the text line and sits
// centered in the (much larger) touch zone.
constexpr int kButtonOffsetY = 246;
constexpr int kButtonWidth = 110;
constexpr int kButtonHeight = 40;
constexpr int kRejectButtonOffsetX = 60;
constexpr int kApproveButtonOffsetX = 190;

constexpr uint32_t kSummaryTextColor = 0xFFFFFF;
constexpr uint32_t kButtonTextColor = 0xFFFFFF;
constexpr uint32_t kApproveBackgroundColor = 0x2E7D32;
constexpr uint32_t kRejectBackgroundColor = 0xC62828;

// Touch zones are larger than the visible buttons and separated by a dead
// gutter, so a fat finger lands somewhere unambiguous or nowhere at all.
constexpr int kTouchZoneTop = 205;
constexpr int kTouchZoneBottom = 330;
constexpr int kRejectZoneLeft = 30;
constexpr int kRejectZoneRight = 175;
constexpr int kApproveZoneLeft = 185;
constexpr int kApproveZoneRight = 330;

inline bool IsInsideApproveZone(int x, int y) {
    return x >= kApproveZoneLeft && x <= kApproveZoneRight && y >= kTouchZoneTop &&
           y <= kTouchZoneBottom;
}

inline bool IsInsideRejectZone(int x, int y) {
    return x >= kRejectZoneLeft && x <= kRejectZoneRight && y >= kTouchZoneTop &&
           y <= kTouchZoneBottom;
}

}  // namespace confirm_geometry

#endif  // CONFIRM_GEOMETRY_H
