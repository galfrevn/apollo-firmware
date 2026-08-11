#include "micro_wake_word.h"

#include <algorithm>
#include <cstring>
#include <numeric>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <tensorflow/lite/micro/micro_allocator.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/micro_resource_variable.h>
#include <tensorflow/lite/schema/schema_generated.h>

extern "C" {
#include <frontend.h>
#include <frontend_util.h>
}

#include "audio_service.h"

#define TAG "MicroWakeWord"

extern const uint8_t model_start[] asm("_binary_micro_wake_word_model_tflite_start");
extern const uint8_t model_end[] asm("_binary_micro_wake_word_model_tflite_end");

namespace {

// One feature slice per 10 ms step at 16 kHz; the model was trained on
// the micro_speech frontend settings below, so they must not change.
constexpr size_t kFeatureStepSamples = 160;
// ~1 s of slices must pass after (re)arming before a detection can fire,
// so stale streaming state never triggers on the first frames.
constexpr int kMinSlicesBeforeDetection = 100;
constexpr size_t kVariableArenaSize = 8192;

}  // namespace

MicroWakeWord::~MicroWakeWord() {
    delete interpreter_;
    delete resolver_;
    if (tensor_arena_ != nullptr) {
        heap_caps_free(tensor_arena_);
    }
    // variable_allocator_ and resource_variables_ live inside variable_arena_
    if (variable_arena_ != nullptr) {
        heap_caps_free(variable_arena_);
    }
    if (frontend_state_ != nullptr) {
        FrontendFreeStateContents(frontend_state_);
        delete frontend_state_;
    }
    if (model_data_ != nullptr) {
        heap_caps_free(model_data_);
    }
    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
    }
    if (wake_word_encode_task_buffer_ != nullptr) {
        heap_caps_free(wake_word_encode_task_buffer_);
    }
}

bool MicroWakeWord::Initialize(AudioCodec* codec, srmodel_list_t*) {
    codec_ = codec;

    const size_t model_size = model_end - model_start;
    // EMBED_FILES only guarantees 4-byte alignment; flatbuffers want more,
    // and a PSRAM copy avoids flash-cache stalls during inference.
    model_data_ = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(16, model_size, MALLOC_CAP_SPIRAM));
    if (model_data_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for the model", model_size);
        return false;
    }
    std::memcpy(model_data_, model_start, model_size);

    model_ = tflite::GetModel(model_data_);
    if (model_->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Model schema version %lu != supported %d",
            static_cast<unsigned long>(model_->version()), TFLITE_SCHEMA_VERSION);
        return false;
    }

    frontend_state_ = new FrontendState();
    FrontendConfig frontend_config;
    frontend_config.window.size_ms = 30;
    frontend_config.window.step_size_ms = 10;
    frontend_config.filterbank.num_channels = kFeatureSize;
    frontend_config.filterbank.lower_band_limit = 125.0f;
    frontend_config.filterbank.upper_band_limit = 7500.0f;
    frontend_config.noise_reduction.smoothing_bits = 10;
    frontend_config.noise_reduction.even_smoothing = 0.025f;
    frontend_config.noise_reduction.odd_smoothing = 0.06f;
    frontend_config.noise_reduction.min_signal_remaining = 0.05f;
    frontend_config.pcan_gain_control.enable_pcan = 1;
    frontend_config.pcan_gain_control.strength = 0.95f;
    frontend_config.pcan_gain_control.offset = 80.0f;
    frontend_config.pcan_gain_control.gain_bits = 21;
    frontend_config.log_scale.enable_log = 1;
    frontend_config.log_scale.scale_shift = 6;
    if (!FrontendPopulateState(&frontend_config, frontend_state_, 16000)) {
        ESP_LOGE(TAG, "Failed to initialize the audio frontend");
        delete frontend_state_;
        frontend_state_ = nullptr;
        return false;
    }

    resolver_ = new tflite::MicroMutableOpResolver<kMaxOps>();
    resolver_->AddCallOnce();
    resolver_->AddVarHandle();
    resolver_->AddReadVariable();
    resolver_->AddAssignVariable();
    resolver_->AddReshape();
    resolver_->AddStridedSlice();
    resolver_->AddConcatenation();
    resolver_->AddConv2D();
    resolver_->AddDepthwiseConv2D();
    resolver_->AddMul();
    resolver_->AddAdd();
    resolver_->AddMean();
    resolver_->AddFullyConnected();
    resolver_->AddLogistic();
    resolver_->AddQuantize();
    resolver_->AddAveragePool2D();
    resolver_->AddMaxPool2D();
    resolver_->AddPad();
    resolver_->AddPack();
    resolver_->AddSplitV();

    variable_arena_ = static_cast<uint8_t*>(
        heap_caps_malloc(kVariableArenaSize, MALLOC_CAP_SPIRAM));
    if (variable_arena_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate the resource-variable arena");
        return false;
    }

    // The manifest's tensor_arena_size can be optimistic; probe upward.
    size_t arena_size = 32000;
    for (int attempt = 0; attempt < 3; ++attempt) {
        tensor_arena_ = static_cast<uint8_t*>(
            heap_caps_malloc(arena_size, MALLOC_CAP_SPIRAM));
        if (tensor_arena_ == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate a %zu byte tensor arena", arena_size);
            return false;
        }
        // Streaming models keep their state in resource variables; without
        // this wiring VAR_HANDLE/CALL_ONCE fail inside AllocateTensors.
        variable_allocator_ = tflite::MicroAllocator::Create(
            variable_arena_, kVariableArenaSize);
        resource_variables_ = tflite::MicroResourceVariables::Create(
            variable_allocator_, kMaxOps);
        interpreter_ = new tflite::MicroInterpreter(
            model_, *resolver_, tensor_arena_, arena_size, resource_variables_);
        if (interpreter_->AllocateTensors() == kTfLiteOk) {
            break;
        }
        delete interpreter_;
        interpreter_ = nullptr;
        heap_caps_free(tensor_arena_);
        tensor_arena_ = nullptr;
        arena_size += arena_size / 2;
    }
    if (interpreter_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate tensors for the wake model");
        return false;
    }

    TfLiteTensor* input = interpreter_->input(0);
    TfLiteTensor* output = interpreter_->output(0);
    if (input->dims->size != 3 || input->dims->data[2] != static_cast<int>(kFeatureSize) ||
        input->type != kTfLiteInt8 || output->type != kTfLiteUInt8) {
        ESP_LOGE(TAG, "Unexpected model tensor layout");
        return false;
    }
    stride_ = input->dims->data[1];
    // The model consumes features quantized from the trainer's float domain
    // (frontend uint16 * 0.0390625); using the tensor's own quantization
    // params instead of ESPHome's hardcoded formula keeps this correct for
    // models quantized with a different scale or zero point.
    input_scale_ = input->params.scale;
    input_zero_point_ = input->params.zero_point;
    output_scale_ = output->params.scale;
    output_zero_point_ = output->params.zero_point;

    probability_cutoff_ = CONFIG_MICRO_WAKE_WORD_PROBABILITY_CUTOFF * 255 / 100;
    window_size_ = CONFIG_MICRO_WAKE_WORD_SLIDING_WINDOW_SIZE;

#if CONFIG_SEND_WAKE_WORD_DATA
    if (!wake_word_audio_cache_.Initialize(16000 * 2)) {
        ESP_LOGW(TAG, "Wake-word audio upload disabled: PSRAM cache allocation failed");
    }
#endif

    ESP_LOGI(TAG, "Model '%s' loaded: %zu bytes, stride %d, arena %zu (%zu used), cutoff %lu/255",
        CONFIG_MICRO_WAKE_WORD_PHRASE, model_size, stride_, arena_size,
        interpreter_->arena_used_bytes(), static_cast<unsigned long>(probability_cutoff_));
    ESP_LOGI(TAG, "Input quant scale %f zp %ld, output quant scale %f zp %ld",
        static_cast<double>(input_scale_), static_cast<long>(input_zero_point_),
        static_cast<double>(output_scale_), static_cast<long>(output_zero_point_));
    return true;
}

void MicroWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = std::move(callback);
}

void MicroWakeWord::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (interpreter_ == nullptr) {
        return;
    }
    probabilities_.clear();
    stride_step_ = 0;
    ignore_slices_ = -kMinSlicesBeforeDetection;
    FrontendReset(frontend_state_);
    interpreter_->Reset();
    running_ = true;
}

void MicroWakeWord::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
}

void MicroWakeWord::Feed(const std::vector<int16_t>& data) {
    FeedSamples(data.data(), data.size(), false);
}

void MicroWakeWord::FeedMono(const int16_t* data, size_t samples) {
    FeedSamples(data, samples, true);
}

void MicroWakeWord::FeedSamples(const int16_t* data, size_t samples, bool mono) {
    if (interpreter_ == nullptr || data == nullptr || samples == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return;
    }

    if (!mono && codec_ != nullptr && codec_->input_channels() > 1) {
        mono_buffer_.clear();
        for (size_t i = 0; i < samples; i += codec_->input_channels()) {
            mono_buffer_.push_back(data[i]);
        }
        data = mono_buffer_.data();
        samples = mono_buffer_.size();
    }

#if CONFIG_SEND_WAKE_WORD_DATA
    wake_word_audio_cache_.Store(data, samples);
#endif

    // The frontend buffers partial windows internally, so every sample is
    // handed over here and nothing needs to be carried across calls.
    size_t consumed = 0;
    while (consumed < samples && running_) {
        size_t num_read = 0;
        FrontendOutput output = FrontendProcessSamples(
            frontend_state_, data + consumed, samples - consumed, &num_read);
        if (num_read == 0 && output.size == 0) {
            break;
        }
        consumed += num_read;
        if (output.size > 0) {
            ProcessFeatureSlice(output.values, output.size);
        }
    }
}

void MicroWakeWord::ProcessFeatureSlice(const uint16_t* values, size_t size) {
    TfLiteTensor* input = interpreter_->input(0);
    int8_t* slice = input->data.int8 + stride_step_ * kFeatureSize;
    const size_t count = std::min(size, kFeatureSize);
    // microWakeWord training scales the frontend's uint16 log-mel output by
    // 0.0390625 into its float feature domain before quantization
    const float feature_scale = 0.0390625f / input_scale_;
    for (size_t i = 0; i < count; ++i) {
        int32_t value = static_cast<int32_t>(
            values[i] * feature_scale + input_zero_point_ + 0.5f);
        slice[i] = static_cast<int8_t>(std::clamp<int32_t>(value, INT8_MIN, INT8_MAX));
    }

    if (ignore_slices_ < 0) {
        ++ignore_slices_;
    }
    if (++stride_step_ < stride_) {
        return;
    }
    stride_step_ = 0;

    auto invoke_start = esp_timer_get_time();
    if (interpreter_->Invoke() != kTfLiteOk) {
        ESP_LOGE(TAG, "Inference failed");
        return;
    }
    if (!invoke_time_logged_) {
        invoke_time_logged_ = true;
        ESP_LOGI(TAG, "First inference took %ld us",
            static_cast<long>(esp_timer_get_time() - invoke_start));
    }

    uint8_t probability = interpreter_->output(0)->data.uint8[0];
    probabilities_.push_back(probability);
    if (probabilities_.size() > window_size_) {
        probabilities_.pop_front();
    }
    peak_probability_ = std::max(peak_probability_, probability);
    if (++invokes_since_log_ >= 100) {
        ESP_LOGI(TAG, "Peak probability over last %d invokes: %u/255",
            invokes_since_log_, peak_probability_);
        invokes_since_log_ = 0;
        peak_probability_ = 0;
    }
    if (ignore_slices_ < 0 || probabilities_.size() < window_size_) {
        return;
    }

    uint32_t mean = std::accumulate(probabilities_.begin(), probabilities_.end(), 0u) /
        probabilities_.size();
    if (mean <= probability_cutoff_) {
        return;
    }

    ESP_LOGI(TAG, "Wake word detected, mean probability %lu/255",
        static_cast<unsigned long>(mean));
    last_detected_wake_word_ = CONFIG_MICRO_WAKE_WORD_PHRASE;
    running_ = false;
    probabilities_.clear();
    if (wake_word_detected_callback_) {
        wake_word_detected_callback_(last_detected_wake_word_);
    }
}

size_t MicroWakeWord::GetFeedSize() {
    return kFeatureStepSamples;
}

void MicroWakeWord::EncodeWakeWordData() {
    const size_t stack_size = 4096 * 7;
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ = static_cast<StackType_t*>(
            heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM));
        assert(wake_word_encode_task_stack_ != nullptr);
    }
    if (wake_word_encode_task_buffer_ == nullptr) {
        wake_word_encode_task_buffer_ = static_cast<StaticTask_t*>(
            heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
        assert(wake_word_encode_task_buffer_ != nullptr);
    }

    wake_word_encode_task_ = xTaskCreateStatic([](void* arg) {
        auto* this_ = static_cast<MicroWakeWord*>(arg);
        auto start_time = esp_timer_get_time();
        esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
        void* encoder_handle = nullptr;
        auto ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &encoder_handle);
        if (encoder_handle == nullptr) {
            ESP_LOGE(TAG, "Failed to create wake-word encoder: %d", ret);
            this_->wake_word_audio_cache_.Clear();
            {
                std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                this_->wake_word_opus_.emplace_back();
                this_->wake_word_cv_.notify_all();
            }
            vTaskDelete(nullptr);
            return;
        }

        int frame_size = 0;
        int outbuf_size = 0;
        esp_opus_enc_get_frame_size(encoder_handle, &frame_size, &outbuf_size);
        frame_size /= sizeof(int16_t);
        int packets = 0;
        std::vector<int16_t> input(frame_size);
        esp_audio_enc_in_frame_t in = {};
        esp_audio_enc_out_frame_t out = {};

        const size_t cached_samples = this_->wake_word_audio_cache_.Size();
        for (size_t offset = 0;
             offset + static_cast<size_t>(frame_size) <= cached_samples;
             offset += frame_size) {
            if (this_->wake_word_audio_cache_.Read(
                    offset, input.data(), frame_size) != static_cast<size_t>(frame_size)) {
                break;
            }
            std::vector<uint8_t> opus_buf(outbuf_size);
            in.buffer = reinterpret_cast<uint8_t*>(input.data());
            in.len = frame_size * sizeof(int16_t);
            out.buffer = opus_buf.data();
            out.len = outbuf_size;
            out.encoded_bytes = 0;
            ret = esp_opus_enc_process(encoder_handle, &in, &out);
            if (ret == ESP_AUDIO_ERR_OK) {
                std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                this_->wake_word_opus_.emplace_back(
                    opus_buf.data(), opus_buf.data() + out.encoded_bytes);
                this_->wake_word_cv_.notify_all();
                ++packets;
            } else {
                ESP_LOGE(TAG, "Failed to encode wake-word audio: %d", ret);
            }
        }

        this_->wake_word_audio_cache_.Clear();
        esp_opus_enc_close(encoder_handle);
        ESP_LOGI(TAG, "Encoded wake word into %d packets in %ld ms", packets,
            static_cast<long>((esp_timer_get_time() - start_time) / 1000));
        {
            std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
            this_->wake_word_opus_.emplace_back();
            this_->wake_word_cv_.notify_all();
        }
        vTaskDelete(nullptr);
    }, "encode_wake_word", stack_size, this, 2,
        wake_word_encode_task_stack_, wake_word_encode_task_buffer_);
}

bool MicroWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() { return !wake_word_opus_.empty(); });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}
