# Protocol

The device speaks Apollo's JSON-over-websocket dialect, implemented in `main/protocols/apollo_protocol.*`. The server-side contract (Zod schemas) lives in the main repo; this chapter covers the device's half. The upstream MQTT and websocket protocols were removed from this fork.

## Connection

The URL is built as `<base>/agents/apollo/<device_id>?token=<token>` — the agents SDK routes on path and authenticates from the query parameter. The channel opens lazily when a session starts, not at boot.

## Device → server

| Type | Sent when |
|------|-----------|
| `hold_start` / `hold_end` | Push-to-talk press and release |
| `wake` | Wake word detected |
| `audio_end` | VAD endpoint of a wake-word turn: ≥300 ms of speech followed by 1.2 s of silence commits the utterance |
| `listen_cancel` | The listen session ended without a turn: a tap on the open mic, or 8 s with no speech |
| `gesture` | `double_tap`, `swipe_left`, `swipe_right` (a tap acts locally and is never forwarded) |
| `confirm` | A button press on the confirm screen: Sí sends `ok: true`, No sends `ok: false` |
| `abort` | User interrupts playback |
| `playback_ack` | Once a second while speaking: `playedMilliseconds` of TTS audio actually output, plus the `sequence` echoed from `tts_start`, so the server paces against the real queue instead of a model |

Utterance audio rides as raw binary frames (PCM, no framing): the server concatenates them and wraps a RIFF header before transcription.

## Server → device

| Type | Handled by |
|------|-----------|
| `ui_state` | Face emotion (mapped to the emote vocabulary), accent ring color, caption; `focusStartedAt`/`focusEndsAt` (epoch seconds) drive the draining arc on the ring, counted down locally |
| `tts_start` | Announces sample rate (and `sequence`) of the PCM run. A `bytes` total makes the run self-closing by byte count; without one the run stays open until `tts_end`, which is what allows streaming synthesis |
| `tts_end` | Closes an open-ended run |
| `tts_aborted` | Closes a run whose bytes will never arrive |
| `timer` | `endsAt` + `durationSeconds` (epoch seconds) start a countdown arc that outranks the focus arc; without them the arc clears |
| `confirm_request` | Full-screen confirm prompt (summary + Sí/No buttons) with a local expiry from `expiresAt` |
| `confirm_close` | The window ended elsewhere (resolved, expired, lost); dismisses the confirm screen |
| `error` / `reminder` / `background_result` | Alerts with matching face |
| `turn_end` | `expectsReply` decides what follows the reply: reopen the mic (the model asked something) or return to idle. Missing `turn_end` falls back to reopening |

Reply audio arrives as headerless PCM binary frames and bypasses the Opus decoder (`AudioStreamPacket.pcm`).

## Design notes

- Emotions are translated in `MapApolloEmotion`: Apollo's vocabulary → emote asset names, avoiding non-looping assets that freeze the face.
- `dashboard` and the agents SDK's own `cf_agent_*` traffic are ignored on purpose.
- Turn audio byte counts are logged on `hold_end` — the difference between "the mic is deaf" and "the audio never left the device".

## Navigation

Prev: [Architecture](../introduction/architecture.md) · Next: [Audio](audio.md)
