#ifndef MICRO_WAKE_WORD_H
#define MICRO_WAKE_WORD_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "audio_codec.h"
#include "wake_word.h"
#include "wake_word_audio_cache.h"

struct FrontendState;

namespace tflite {
struct Model;
class MicroAllocator;
class MicroInterpreter;
class MicroResourceVariables;
template <unsigned int tOpCount>
class MicroMutableOpResolver;
}  // namespace tflite

class MicroWakeWord : public WakeWord {
public:
    MicroWakeWord() = default;
    ~MicroWakeWord();

    bool Initialize(AudioCodec* codec, srmodel_list_t* models_list) override;
    void Feed(const std::vector<int16_t>& data) override;
    void FeedMono(const int16_t* data, size_t samples);
    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) override;
    void Start() override;
    void Stop() override;
    size_t GetFeedSize() override;
    void EncodeWakeWordData() override;
    bool GetWakeWordOpus(std::vector<uint8_t>& opus) override;
    const std::string& GetLastDetectedWakeWord() const override { return last_detected_wake_word_; }

private:
    static constexpr size_t kFeatureSize = 40;
    static constexpr size_t kMaxOps = 20;

    void FeedSamples(const int16_t* data, size_t samples, bool mono);
    void ProcessFeatureSlice(const uint16_t* values, size_t size);

    AudioCodec* codec_ = nullptr;
    uint8_t* model_data_ = nullptr;
    const tflite::Model* model_ = nullptr;
    tflite::MicroMutableOpResolver<kMaxOps>* resolver_ = nullptr;
    tflite::MicroAllocator* variable_allocator_ = nullptr;
    tflite::MicroResourceVariables* resource_variables_ = nullptr;
    tflite::MicroInterpreter* interpreter_ = nullptr;
    uint8_t* tensor_arena_ = nullptr;
    uint8_t* variable_arena_ = nullptr;
    FrontendState* frontend_state_ = nullptr;

    int stride_ = 1;
    int stride_step_ = 0;
    float input_scale_ = 1.0f;
    int input_zero_point_ = 0;
    float output_scale_ = 1.0f;
    int output_zero_point_ = 0;
    int ignore_slices_ = 0;
    uint32_t probability_cutoff_ = 0;
    size_t window_size_ = 0;
    std::deque<uint8_t> probabilities_;
    std::vector<int16_t> mono_buffer_;
    bool invoke_time_logged_ = false;
    uint8_t peak_probability_ = 0;
    int invokes_since_log_ = 0;

    std::atomic<bool> running_ = false;
    std::mutex mutex_;
    std::function<void(const std::string& wake_word)> wake_word_detected_callback_;
    std::string last_detected_wake_word_;

    TaskHandle_t wake_word_encode_task_ = nullptr;
    StaticTask_t* wake_word_encode_task_buffer_ = nullptr;
    StackType_t* wake_word_encode_task_stack_ = nullptr;
    WakeWordAudioCache wake_word_audio_cache_;
    std::deque<std::vector<uint8_t>> wake_word_opus_;
    std::mutex wake_word_mutex_;
    std::condition_variable wake_word_cv_;
};

#endif
