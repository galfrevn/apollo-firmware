# Touch

The round screen is the only input surface. The board's touch task (`main/boards/waveshare/esp32-s3-touch-lcd-1.85c/`) turns raw touches into gestures.

## Gestures

| Gesture | Effect |
|---------|--------|
| Hold (≥450 ms, finger still) | Push-to-talk: records until the finger lifts |
| Tap | Local stop: cancels an open mic (listening → `listen_cancel`, nothing transcribed) or cuts Apollo off mid-speech (speaking → `abort`); never forwarded as a gesture |
| Double tap | Sent to the server (mute used to live here; removed as a trap) |
| Swipe left/right | Cycle speech mode; the switch sound cues locally, the ring color follows from the server echo |

## Design notes

- The hold threshold sits just past the tap window (400 ms), so a press only becomes push-to-talk once it is too long to still be a tap; the finger has to stay put or it would hijack a slow swipe.
- Any sign of the user — touch, wake word, a turn starting — calls `Application::NoteUserActivity()`, which wakes the screen and restarts the inactivity countdown. After 60 s idle the backlight goes dark and the emote engine stops decoding frames (that is the part that costs CPU).
- A `confirm_request` replaces the face with a full-screen prompt: the summary plus Sí/No touch buttons (`main/display/confirm_geometry.h` holds the layout, shared with the hit-test so they cannot drift). While it is up, the touch task hit-tests releases immediately — no double-tap wait, no hold-to-talk — and taps outside both zones do nothing. It used to be "any tap accepts", which is exactly the accidental-approval trap the buttons remove; voice stays available through the boot button and the wake word.
- The screen dismisses on: a button press, local expiry (from `expiresAt`, clamped; falls back to 30 s if the clock is unsynced), a server `confirm_close`, any `ui_state` whose state is not `confirm`, or the channel closing.

## Navigation

Prev: [Face](face.md) · Next: [Sounds](sounds.md)
