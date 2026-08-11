#include "apollo_protocol.h"
#include "application.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <cstring>
#include <ctime>
#include "assets/lang_config.h"
#include "assets/sound_variants.h"

#define TAG "Apollo"

// Apollo advertises the sample rate on every tts_start; this is only the value
// used before the first one arrives.
#define APOLLO_DEFAULT_TTS_SAMPLE_RATE 24000
#define APOLLO_UPLINK_SAMPLE_RATE 16000
#define APOLLO_FRAME_DURATION_MS 60

namespace {

// Apollo's six states are mapped onto the emote engine's vocabulary, avoiding
// "neutral" and "idle": both are configured loop=false upstream, so they play
// once and freeze. An idle desk agent would sit on a still frame, which reads
// as a crash rather than as calm.
const char* MapApolloEmotion(const char* apollo_emotion) {
    if (strcmp(apollo_emotion, "curious") == 0) {
        // Not "shocked": that asset is the wide-eyed panic face and reads as an
        // error. "winking" is neutral.eaf with loop=true, so the eyes keep
        // blinking attentively while the listen icon does the signaling.
        return "winking";
    }
    if (strcmp(apollo_emotion, "focused") == 0) {
        return "thinking";
    }
    if (strcmp(apollo_emotion, "questioning") == 0) {
        return "confused";
    }
    if (strcmp(apollo_emotion, "talking") == 0) {
        return "happy";
    }
    if (strcmp(apollo_emotion, "calm") == 0) {
        return "sleepy";
    }
    // "neutral" is the resting face: a single blink that the application
    // replays on a cadence, rather than a loop that never stops blinking.
    return "neutral";
}

uint32_t NowMilliseconds() { return (uint32_t)(esp_timer_get_time() / 1000); }

constexpr uint32_t kDefaultConfirmTimeoutMs = 30000;
constexpr uint32_t kMinConfirmTimeoutMs = 3000;
constexpr uint32_t kMaxConfirmTimeoutMs = 45000;
constexpr int64_t kEarliestPlausibleEpochMs = 1609459200000;  // 2021-01-01

// expiresAt is epoch milliseconds, so it only means something once SNTP has
// set the clock. Before that (or against a skewed server) the remainder is
// garbage, and the server's default window is the honest fallback.
uint32_t ConfirmTimeoutFromExpiry(int64_t expires_at_ms) {
    const int64_t now_epoch_ms = (int64_t)time(nullptr) * 1000;
    if (expires_at_ms <= 0 || now_epoch_ms < kEarliestPlausibleEpochMs) {
        return kDefaultConfirmTimeoutMs;
    }
    const int64_t remaining_ms = expires_at_ms - now_epoch_ms;
    if (remaining_ms < (int64_t)kMinConfirmTimeoutMs ||
        remaining_ms > (int64_t)kMaxConfirmTimeoutMs) {
        return kDefaultConfirmTimeoutMs;
    }
    return (uint32_t)remaining_ms;
}

// The wire carries logical effect names so the server can re-purpose sounds
// without a flash; only this table knows which flash asset each name means.
std::string_view MapEffectNameToSound(const char* effect_name) {
    if (strcmp(effect_name, "ding") == 0) {
        return Lang::Sounds::OGG_SUCCESS;
    }
    if (strcmp(effect_name, "chime") == 0) {
        return Lang::Sounds::OGG_POPUP;
    }
    if (strcmp(effect_name, "error") == 0) {
        return Lang::Sounds::OGG_EXCLAMATION;
    }
    if (strcmp(effect_name, "low_battery") == 0) {
        return Lang::Sounds::OGG_LOW_BATTERY;
    }
    return {};
}

}  // namespace

ApolloProtocol::ApolloProtocol() {}

ApolloProtocol::~ApolloProtocol() {}

bool ApolloProtocol::Start() {
    // Like the websocket protocol, only connect when a session actually starts.
    return true;
}

std::string ApolloProtocol::BuildConnectionUrl(const std::string& base_url,
                                               const std::string& device_id,
                                               const std::string& token) const {
    std::string url = base_url;
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    // The `agents` SDK routes on /agents/<kebab-cased class>/<instance name>,
    // and authenticates from a token query parameter rather than a header.
    url += "/agents/apollo/";
    url += device_id;
    url += "?token=";
    url += token;
    return url;
}

bool ApolloProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }
    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    return true;
}

void ApolloProtocol::SendMcpMessage(const std::string& payload) {
    // The inherited frame carries xiaozhi's empty session_id and no ts; Apollo's
    // dialect wants {"type","payload","ts"} like every other event.
    cJSON* payload_json = cJSON_Parse(payload.c_str());
    if (payload_json == nullptr) {
        ESP_LOGE(TAG, "Dropping unparseable MCP payload");
        return;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "mcp");
    cJSON_AddItemToObject(root, "payload", payload_json);
    cJSON_AddNumberToObject(root, "ts", NowMilliseconds());
    auto serialized = cJSON_PrintUnformatted(root);
    std::string message(serialized);
    cJSON_free(serialized);
    cJSON_Delete(root);
    SendText(message);
}

bool ApolloProtocol::SendEvent(const char* type) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddNumberToObject(root, "ts", NowMilliseconds());
    auto serialized = cJSON_PrintUnformatted(root);
    std::string message(serialized);
    cJSON_free(serialized);
    cJSON_Delete(root);
    return SendText(message);
}

bool ApolloProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }
    // Raw PCM, no framing: the server concatenates the binary frames of an
    // utterance and wraps them in a RIFF header before transcription.
    bool sent =
        websocket_->Send((const char*)packet->payload.data(), packet->payload.size(), true);
    if (sent) {
        turn_audio_bytes_ += packet->payload.size();
    }
    return sent;
}

void ApolloProtocol::SendStartListening(ListeningMode mode) {
    // Apollo distinguishes a held button from a wake word, and resets its audio
    // buffer on either.
    turn_audio_bytes_ = 0;
    listen_started_by_hold_ = mode == kListeningModeManualStop;
    SendEvent(listen_started_by_hold_ ? "hold_start" : "wake");
}

void ApolloProtocol::SendStopListening() {
    // The server transcribes whatever arrived before this event, so knowing how
    // much actually went out is the difference between "the mic is deaf" and
    // "the audio never left the device".
    ESP_LOGI(TAG, "Turn audio sent: %u bytes (%.2f s)", (unsigned)turn_audio_bytes_,
             turn_audio_bytes_ / 32000.0f);
    SendEvent(listen_started_by_hold_ ? "hold_end" : "audio_end");
}

void ApolloProtocol::SendListenCancel() {
    ESP_LOGI(TAG, "Listen cancelled, %u bytes discarded", (unsigned)turn_audio_bytes_);
    SendEvent("listen_cancel");
}

void ApolloProtocol::SendWakeWordDetected(const std::string& wake_word) {
    // Apollo has no voice-print step, so the wake word itself carries no extra
    // information beyond "start listening", which SendStartListening covers.
    (void)wake_word;
}

void ApolloProtocol::SendGesture(const std::string& gesture) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "gesture");
    cJSON_AddStringToObject(root, "gesture", gesture.c_str());
    if (gesture.rfind("swipe", 0) == 0) {
        // A swipe cycles the speech mode. The cue plays now, on the
        // gesture, rather than on the server's ui_state echo: feedback
        // that waits for a round trip feels broken on a slow link.
        Application::GetInstance().PlaySound(Lang::SoundVariants::ModeSwitch());
    }
    cJSON_AddNumberToObject(root, "ts", NowMilliseconds());

    auto serialized = cJSON_PrintUnformatted(root);
    std::string message(serialized);
    cJSON_free(serialized);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Gesture '%s' -> %s", gesture.c_str(), message.c_str());
    SendText(message);
}

void ApolloProtocol::SendConfirm(bool ok) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "confirm");
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddNumberToObject(root, "ts", NowMilliseconds());

    auto serialized = cJSON_PrintUnformatted(root);
    std::string message(serialized);
    cJSON_free(serialized);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Confirm -> %s", message.c_str());
    SendText(message);
}

void ApolloProtocol::SendTelemetry(const DeviceTelemetry& telemetry) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "telemetry");
    if (telemetry.battery_valid) {
        cJSON_AddNumberToObject(root, "battery", telemetry.battery_level);
    }
    if (telemetry.charging_valid) {
        cJSON_AddBoolToObject(root, "charging", telemetry.charging);
    }
    cJSON_AddNumberToObject(root, "volume", telemetry.volume);
    cJSON_AddNumberToObject(root, "wifiRssi", telemetry.wifi_rssi);
    if (telemetry.firmware_version != nullptr) {
        cJSON_AddStringToObject(root, "firmwareVersion", telemetry.firmware_version);
    }
    cJSON_AddNumberToObject(root, "ts", NowMilliseconds());

    auto serialized = cJSON_PrintUnformatted(root);
    std::string message(serialized);
    cJSON_free(serialized);
    cJSON_Delete(root);

    SendText(message);
}

void ApolloProtocol::SendAbortSpeaking(AbortReason reason) {
    (void)reason;
    // Tell the server to stop streaming, then drop what is already queued here:
    // without the first the audio keeps arriving, without the second the device
    // keeps playing what it already has.
    SendEvent("abort");
    FinishTtsRun();
}

bool ApolloProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void ApolloProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;
    tts_run_active_ = false;
    tts_expected_bytes_ = 0;
    tts_received_bytes_ = 0;
    websocket_.reset();
}

void ApolloProtocol::EmitTranslatedJson(cJSON* root) {
    if (on_incoming_json_ != nullptr) {
        on_incoming_json_(root);
    }
    cJSON_Delete(root);
}

void ApolloProtocol::EmitTtsState(const char* state, const char* text) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "tts");
    cJSON_AddStringToObject(root, "state", state);
    if (text != nullptr) {
        cJSON_AddStringToObject(root, "text", text);
    }
    EmitTranslatedJson(root);
}

void ApolloProtocol::EmitEmotion(const char* emotion) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "llm");
    cJSON_AddStringToObject(root, "emotion", emotion);
    cJSON_AddStringToObject(root, "text", "");
    EmitTranslatedJson(root);
}

void ApolloProtocol::EmitAccentColor(const char* color) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "accent");
    cJSON_AddStringToObject(root, "color", color);
    EmitTranslatedJson(root);
}

void ApolloProtocol::EmitAlert(const char* status, const char* message, const char* emotion) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "alert");
    cJSON_AddStringToObject(root, "status", status);
    cJSON_AddStringToObject(root, "message", message);
    cJSON_AddStringToObject(root, "emotion", emotion);
    EmitTranslatedJson(root);
}

void ApolloProtocol::HandleUiState(const cJSON* root) {
    // Any state but "confirm" means the turn moved on, so a confirm screen
    // still up is stale. Not keyed off tts_start: the summary's own TTS
    // follows the confirm_request immediately and would kill the screen the
    // moment it appeared, while the turn's closing ui_state carries "confirm".
    auto state = cJSON_GetObjectItem(root, "state");
    if (cJSON_IsString(state) && strcmp(state->valuestring, "confirm") != 0) {
        Application::GetInstance().DismissConfirm();
    }

    auto emotion = cJSON_GetObjectItem(root, "emotion");
    if (cJSON_IsString(emotion)) {
        EmitEmotion(MapApolloEmotion(emotion->valuestring));
    }

    // The speech mode is signaled by the accent ring color alone, so it has to
    // ride along on every ui_state, not only on an explicit mode change.
    auto accent = cJSON_GetObjectItem(root, "accentColor");
    if (cJSON_IsString(accent)) {
        EmitAccentColor(accent->valuestring);
    }

    // Apollo's caption is whatever the agent is saying or thinking about, which
    // is what xiaozhi renders from a sentence_start.
    auto caption = cJSON_GetObjectItem(root, "caption");
    if (cJSON_IsString(caption) && strlen(caption->valuestring) > 0) {
        EmitTtsState("sentence_start", caption->valuestring);
    }
}

void ApolloProtocol::HandleTtsStart(const cJSON* root) {
    auto bytes = cJSON_GetObjectItem(root, "bytes");
    auto sample_rate = cJSON_GetObjectItem(root, "sampleRate");
    auto format = cJSON_GetObjectItem(root, "format");

    if (cJSON_IsString(format) && strcmp(format->valuestring, "pcm") != 0) {
        // Nothing on the device can decode mp3, so a non-pcm run would only
        // produce noise. Refuse it loudly instead.
        ESP_LOGE(TAG, "Unsupported tts format '%s', expected pcm", format->valuestring);
        SetError(Lang::Strings::SERVER_ERROR);
        return;
    }

    server_sample_rate_ =
        cJSON_IsNumber(sample_rate) ? sample_rate->valueint : APOLLO_DEFAULT_TTS_SAMPLE_RATE;
    server_frame_duration_ = APOLLO_FRAME_DURATION_MS;
    tts_expected_bytes_ = cJSON_IsNumber(bytes) ? (uint32_t)bytes->valuedouble : 0;
    tts_received_bytes_ = 0;
    tts_run_active_ = tts_expected_bytes_ > 0;

    ESP_LOGI(TAG, "TTS run starting: %lu bytes at %d Hz", (unsigned long)tts_expected_bytes_,
             server_sample_rate_);
    EmitTtsState("start", nullptr);

    if (!tts_run_active_) {
        // An empty run still has to be closed or Application would sit in the
        // speaking state forever.
        EmitTtsState("stop", nullptr);
    }
}

void ApolloProtocol::FinishTtsRun() {
    if (!tts_run_active_) {
        return;
    }
    tts_run_active_ = false;
    tts_expected_bytes_ = 0;
    tts_received_bytes_ = 0;
    EmitTtsState("stop", nullptr);
}

void ApolloProtocol::HandleIncomingAudio(const char* data, size_t len) {
    if (on_incoming_audio_ == nullptr) {
        return;
    }

    auto payload = (const uint8_t*)data;
    on_incoming_audio_(std::make_unique<AudioStreamPacket>(
        AudioStreamPacket{.sample_rate = server_sample_rate_,
                          .frame_duration = server_frame_duration_,
                          .timestamp = 0,
                          .payload = std::vector<uint8_t>(payload, payload + len),
                          .pcm = true}));

    if (!tts_run_active_) {
        return;
    }
    tts_received_bytes_ += len;
    if (tts_received_bytes_ >= tts_expected_bytes_) {
        FinishTtsRun();
    }
}

void ApolloProtocol::HandleIncomingJson(const char* data, size_t len) {
    auto root = cJSON_ParseWithLength(data, len);
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse message: %s", std::string(data, len).c_str());
        return;
    }

    auto type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "ui_state") == 0) {
        HandleUiState(root);
    } else if (strcmp(type->valuestring, "tts_start") == 0) {
        HandleTtsStart(root);
    } else if (strcmp(type->valuestring, "tts_aborted") == 0) {
        // The byte total promised by tts_start will never arrive, so close the
        // run explicitly or the device waits for speech that stopped coming.
        ESP_LOGI(TAG, "Server acknowledged abort");
        FinishTtsRun();
    } else if (strcmp(type->valuestring, "turn_end") == 0) {
        auto expects_reply = cJSON_GetObjectItem(root, "expectsReply");
        Application::GetInstance().OnTurnEnd(cJSON_IsTrue(expects_reply));
    } else if (strcmp(type->valuestring, "error") == 0) {
        auto message = cJSON_GetObjectItem(root, "message");
        EmitAlert("Error", cJSON_IsString(message) ? message->valuestring : "Error", "sad");
    } else if (strcmp(type->valuestring, "reminder") == 0) {
        auto message = cJSON_GetObjectItem(root, "message");
        if (cJSON_IsString(message)) {
            EmitAlert("Recordatorio", message->valuestring, "neutral");
        }
    } else if (strcmp(type->valuestring, "background_result") == 0) {
        auto summary = cJSON_GetObjectItem(root, "summary");
        if (cJSON_IsString(summary)) {
            EmitAlert("Listo", summary->valuestring, "happy");
        }
    } else if (strcmp(type->valuestring, "confirm_request") == 0) {
        auto summary = cJSON_GetObjectItem(root, "summary");
        auto expires_at = cJSON_GetObjectItem(root, "expiresAt");
        if (cJSON_IsString(summary)) {
            const uint32_t timeout_ms = ConfirmTimeoutFromExpiry(
                cJSON_IsNumber(expires_at) ? (int64_t)expires_at->valuedouble : 0);
            // ShowConfirm schedules its own work onto the main task, which is
            // what makes it callable from this websocket task.
            Application::GetInstance().ShowConfirm(summary->valuestring, timeout_ms);
        }
    } else if (strcmp(type->valuestring, "confirm_close") == 0) {
        // The window ended somewhere else: resolved from the dashboard,
        // expired server-side, or lost. The screen has nothing left to answer.
        Application::GetInstance().DismissConfirm();
    } else if (strcmp(type->valuestring, "play_effect") == 0) {
        auto name = cJSON_GetObjectItem(root, "name");
        if (cJSON_IsString(name)) {
            auto sound = MapEffectNameToSound(name->valuestring);
            if (sound.empty()) {
                ESP_LOGW(TAG, "Unknown effect: %s", name->valuestring);
            } else {
                // This runs on the websocket task; playback goes through the
                // main task, which owns the audio service.
                Application::GetInstance().Schedule(
                    [sound]() { Application::GetInstance().PlaySound(sound); });
            }
        }
    } else if (strcmp(type->valuestring, "mcp") == 0) {
        // Handed to Application's dispatcher whole: it unwraps `payload` and
        // routes it into the embedded McpServer. Not EmitTranslatedJson — that
        // would delete root, which this function already does below.
        if (on_incoming_json_ != nullptr) {
            on_incoming_json_(root);
        }
    } else {
        // `dashboard` has no UI yet, and the agents SDK injects its own
        // cf_agent_state / cf_agent_mcp_servers traffic that is not part of
        // Apollo's protocol at all. Both are ignored on purpose.
        ESP_LOGD(TAG, "Ignoring message type: %s", type->valuestring);
    }

    cJSON_Delete(root);
}

bool ApolloProtocol::OpenAudioChannel() {
    // NVS wins so a provisioned device can be repointed without a rebuild, but
    // the build-time values keep the common case flash-and-go. Provisioning NVS
    // rewrites the whole partition, which would take the wifi credentials with
    // it, so defaults are the friendlier path here.
    Settings settings("apollo", false);
    std::string base_url = settings.GetString("url", CONFIG_APOLLO_URL);
    std::string token = settings.GetString("token", CONFIG_APOLLO_TOKEN);
    std::string device_id = settings.GetString("device_id", CONFIG_APOLLO_DEVICE_ID);

    if (base_url.empty()) {
        ESP_LOGE(TAG, "No Apollo url configured in NVS namespace 'apollo'");
        SetError(Lang::Strings::SERVER_NOT_FOUND);
        return false;
    }
    if (device_id.empty()) {
        device_id = SystemInfo::GetMacAddress();
    }

    error_occurred_ = false;
    tts_run_active_ = false;
    tts_expected_bytes_ = 0;
    tts_received_bytes_ = 0;
    server_sample_rate_ = APOLLO_DEFAULT_TTS_SAMPLE_RATE;
    server_frame_duration_ = APOLLO_FRAME_DURATION_MS;

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            HandleIncomingAudio(data, len);
        } else {
            HandleIncomingJson(data, len);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Apollo websocket disconnected");
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    auto url = BuildConnectionUrl(base_url, device_id, token);
    ESP_LOGI(TAG, "Connecting to Apollo as device '%s'", device_id.c_str());
    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect, code=%d", websocket_->GetLastError());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    // Apollo answers a hello with a ui_state push rather than a hello of its
    // own, so there is no handshake to wait for: the channel is usable as soon
    // as the socket is up.
    cJSON* hello = cJSON_CreateObject();
    cJSON_AddStringToObject(hello, "type", "hello");
    cJSON_AddStringToObject(hello, "deviceId", device_id.c_str());
    cJSON_AddNumberToObject(hello, "ts", NowMilliseconds());
    auto serialized = cJSON_PrintUnformatted(hello);
    std::string hello_message(serialized);
    cJSON_free(serialized);
    cJSON_Delete(hello);

    if (!SendText(hello_message)) {
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}
