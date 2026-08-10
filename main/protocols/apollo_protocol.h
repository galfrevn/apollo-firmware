#ifndef _APOLLO_PROTOCOL_H_
#define _APOLLO_PROTOCOL_H_

#include "protocol.h"

#include <web_socket.h>
#include <cstdint>

/*
 * Talks to the Apollo desk agent (a Cloudflare Worker running the `agents`
 * SDK) instead of the xiaozhi server.
 *
 * Two things differ from WebsocketProtocol and shape this class:
 *
 * 1. Apollo carries headerless little-endian PCM in both directions, not Opus.
 *    Uplink frames are whatever the audio service hands us; downlink frames are
 *    a run of chunks announced by a `tts_start` message that states the total
 *    byte count.
 *
 * 2. Apollo has its own message vocabulary. Rather than teach Application about
 *    it, incoming messages are translated into the xiaozhi shapes that
 *    Application already dispatches on (`stt`, `llm`, `tts`, `alert`), so the
 *    existing UI keeps working untouched.
 */
class ApolloProtocol : public Protocol {
public:
    ApolloProtocol();
    ~ApolloProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendAbortSpeaking(AbortReason reason) override;
    void SendWakeWordDetected(const std::string& wake_word) override;
    void SendGesture(const std::string& gesture) override;
    void SendTelemetry(const DeviceTelemetry& telemetry) override;
    void SendConfirm(bool ok) override;
    void SendMcpMessage(const std::string& payload) override;

private:
    std::unique_ptr<WebSocket> websocket_;

    // Byte accounting for the current TTS run: Apollo never sends an explicit
    // "speech finished" message, so the end of a run is inferred by counting
    // downlink bytes against the total announced in `tts_start`.
    uint32_t tts_expected_bytes_ = 0;
    uint32_t tts_received_bytes_ = 0;
    bool tts_run_active_ = false;

    size_t turn_audio_bytes_ = 0;  // PCM actually sent since the turn started
    bool listen_started_by_hold_ = true;

    bool SendText(const std::string& text) override;
    bool SendEvent(const char* type);

    void HandleIncomingJson(const char* data, size_t len);
    void HandleUiState(const cJSON* root);
    void HandleTtsStart(const cJSON* root);
    void HandleIncomingAudio(const char* data, size_t len);
    void FinishTtsRun();

    void EmitTranslatedJson(cJSON* root);
    void EmitTtsState(const char* state, const char* text);
    void EmitEmotion(const char* emotion);
    void EmitAccentColor(const char* color);
    void EmitAlert(const char* status, const char* message, const char* emotion);

    std::string BuildConnectionUrl(const std::string& base_url, const std::string& device_id,
                                   const std::string& token) const;
};

#endif  // _APOLLO_PROTOCOL_H_
