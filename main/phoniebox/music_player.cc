#include "music_player.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>

#include "audio_service.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"

#define TAG "MusicPlayer"

namespace {

constexpr size_t kInitialInputBuffer = 16 * 1024;
constexpr size_t kMaxInputBuffer = 128 * 1024;
constexpr size_t kInitialOutputBuffer = 16 * 1024;
constexpr size_t kMaxOutputBuffer = 512 * 1024;

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string BaseName(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string JoinPath(const std::string& directory, const std::string& name) {
    if (directory.empty() || directory.back() == '/') {
        return directory + name;
    }
    return directory + "/" + name;
}

bool EndsWith(const std::string& value, const char* suffix) {
    size_t length = std::strlen(suffix);
    return value.size() >= length && value.compare(value.size() - length, length, suffix) == 0;
}

esp_audio_simple_dec_type_t DecoderTypeForPath(const std::string& path) {
    std::string lower = ToLower(path);
    if (EndsWith(lower, ".mp3")) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    if (EndsWith(lower, ".wav")) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    }
    if (EndsWith(lower, ".flac")) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    }
    if (EndsWith(lower, ".m4a") || EndsWith(lower, ".mp4")) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    }
    if (EndsWith(lower, ".aac")) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    }
    if (EndsWith(lower, ".ogg") || EndsWith(lower, ".opus")) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

}  // namespace

MusicPlayer::MusicPlayer(AudioService& audio_service) : audio_service_(audio_service) {}

MusicPlayer::~MusicPlayer() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        task_stop_ = true;
        ++generation_;
    }
    cv_.notify_all();
    // The task deletes itself; give it a moment to leave the decode loop.
    vTaskDelay(pdMS_TO_TICKS(100));
}

bool MusicPlayer::IsSupportedFile(const std::string& path) {
    return DecoderTypeForPath(path) != ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

bool MusicPlayer::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_started_) {
        return true;
    }

    esp_audio_err_t ret = esp_audio_simple_dec_register_default();
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to register audio decoders: %d", ret);
        return false;
    }

    BaseType_t created =
        xTaskCreate([](void* context) { static_cast<MusicPlayer*>(context)->TaskLoop(); },
                    "music_player", 8192, this, 4, &task_handle_);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create music player task");
        return false;
    }
    task_started_ = true;
    return true;
}

bool MusicPlayer::PlayPath(const std::string& path) { return LoadPlaylist(path, false); }

bool MusicPlayer::LoadPlaylist(const std::string& path, bool single_file) {
    struct stat info = {};
    if (path.empty() || stat(path.c_str(), &info) != 0) {
        ESP_LOGW(TAG, "Path does not exist: %s", path.c_str());
        return false;
    }

    std::vector<std::string> files;
    if (single_file || S_ISREG(info.st_mode)) {
        if (!IsSupportedFile(path)) {
            ESP_LOGW(TAG, "Unsupported audio file: %s", path.c_str());
            return false;
        }
        files.push_back(path);
    } else if (S_ISDIR(info.st_mode)) {
        DIR* directory = opendir(path.c_str());
        if (directory == nullptr) {
            ESP_LOGW(TAG, "Cannot open folder: %s", path.c_str());
            return false;
        }
        struct dirent* entry = nullptr;
        while ((entry = readdir(directory)) != nullptr) {
            if (entry->d_name[0] == '.') {
                continue;
            }
            std::string candidate = JoinPath(path, entry->d_name);
            struct stat candidate_info = {};
            if (stat(candidate.c_str(), &candidate_info) == 0 && S_ISREG(candidate_info.st_mode) &&
                IsSupportedFile(candidate)) {
                files.push_back(std::move(candidate));
            }
        }
        closedir(directory);
        std::sort(files.begin(), files.end());
    }

    if (files.empty()) {
        ESP_LOGW(TAG, "No playable audio in: %s", path.c_str());
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        playlist_ = std::move(files);
        index_ = 0;
        position_ms_ = 0;
        current_path_ = playlist_.front();
        current_title_ = BaseName(current_path_);
        state_ = State::kPlaying;
        ++generation_;
    }
    cv_.notify_all();
    return true;
}

void MusicPlayer::Pause() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::kPlaying) {
            return;
        }
        state_ = State::kPaused;
    }
    cv_.notify_all();
}

void MusicPlayer::Resume() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::kPaused) {
            return;
        }
        state_ = State::kPlaying;
    }
    cv_.notify_all();
}

void MusicPlayer::TogglePause() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::kPlaying) {
            state_ = State::kPaused;
        } else if (state_ == State::kPaused) {
            state_ = State::kPlaying;
        } else {
            return;
        }
    }
    cv_.notify_all();
}

void MusicPlayer::Next() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (playlist_.empty()) {
            return;
        }
        index_ = (index_ + 1) % playlist_.size();
        current_path_ = playlist_[index_];
        current_title_ = BaseName(current_path_);
        position_ms_ = 0;
        state_ = State::kPlaying;
        ++generation_;
    }
    cv_.notify_all();
    audio_service_.ResetMusicResampler();
}

void MusicPlayer::Previous() {
    bool restart = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (playlist_.empty()) {
            return;
        }
        restart = position_ms_ > 3000;
        if (!restart) {
            index_ = (index_ + playlist_.size() - 1) % playlist_.size();
        }
        current_path_ = playlist_[index_];
        current_title_ = BaseName(current_path_);
        position_ms_ = 0;
        state_ = State::kPlaying;
        ++generation_;
    }
    cv_.notify_all();
    audio_service_.ResetMusicResampler();
}

void MusicPlayer::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        state_ = State::kStopped;
        playlist_.clear();
        index_ = 0;
        position_ms_ = 0;
        current_path_.clear();
        current_title_.clear();
    }
    cv_.notify_all();
    audio_service_.ResetMusicResampler();
}

MusicPlayer::Status MusicPlayer::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Status status;
    status.state = state_;
    status.path = current_path_;
    status.title = current_title_;
    status.index = index_;
    status.count = playlist_.size();
    status.position_ms = position_ms_;
    return status;
}

bool MusicPlayer::CheckCancelled(uint32_t generation) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this, generation]() {
        return task_stop_ || generation != generation_ || state_ != State::kPaused;
    });
    if (task_stop_ || generation != generation_) {
        return true;
    }
    // kStopped cancels, kPaused falls through only after Resume (the wait above
    // blocks while paused), so anything other than playing is cancelled.
    return state_ != State::kPlaying;
}

void MusicPlayer::TaskLoop() {
    for (;;) {
        std::string path;
        uint32_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return task_stop_ || state_ == State::kPlaying; });
            if (task_stop_) {
                break;
            }
            if (state_ != State::kPlaying || playlist_.empty()) {
                state_ = State::kStopped;
                continue;
            }
            if (index_ >= playlist_.size()) {
                index_ = 0;
            }
            path = playlist_[index_];
            generation = generation_;
            current_path_ = path;
            current_title_ = BaseName(path);
            position_ms_ = 0;
        }

        TrackResult result = DecodeTrack(path, generation);
        if (result == TrackResult::kCancelled) {
            continue;
        }

        // Let the previously queued tail play out before moving on.
        if (result == TrackResult::kFinished) {
            while (!audio_service_.IsPlaybackIdle()) {
                if (CheckCancelled(generation)) {
                    result = TrackResult::kCancelled;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        if (result == TrackResult::kCancelled) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (task_stop_ || generation != generation_) {
                continue;
            }
            if (result == TrackResult::kFailed) {
                ESP_LOGW(TAG, "Skipping unplayable file: %s", path.c_str());
            }
            if (index_ + 1 < playlist_.size()) {
                ++index_;
                current_path_ = playlist_[index_];
                current_title_ = BaseName(current_path_);
                position_ms_ = 0;
            } else {
                state_ = State::kStopped;
                position_ms_ = 0;
                current_path_.clear();
                current_title_.clear();
            }
        }
        cv_.notify_all();
    }

    task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

MusicPlayer::TrackResult MusicPlayer::DecodeTrack(const std::string& path, uint32_t generation) {
    esp_audio_simple_dec_type_t type = DecoderTypeForPath(path);
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        return TrackResult::kFailed;
    }

    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        ESP_LOGW(TAG, "Failed to open %s", path.c_str());
        return TrackResult::kFailed;
    }

    esp_audio_simple_dec_cfg_t config = {};
    config.dec_type = type;
    config.dec_cfg = nullptr;
    config.cfg_size = 0;
    config.use_frame_dec = false;

    esp_audio_simple_dec_handle_t decoder = nullptr;
    esp_audio_err_t ret = esp_audio_simple_dec_open(&config, &decoder);
    if (ret != ESP_AUDIO_ERR_OK || decoder == nullptr) {
        ESP_LOGW(TAG, "Decoder not available for %s (%d)", path.c_str(), ret);
        fclose(file);
        return TrackResult::kFailed;
    }

    audio_service_.ResetMusicResampler();

    std::vector<uint8_t> input(kInitialInputBuffer);
    std::vector<uint8_t> output(kInitialOutputBuffer);
    size_t input_length = 0;
    bool eos = false;
    bool info_ready = false;
    int sample_rate = 0;
    int channels = 0;
    TrackResult result = TrackResult::kFinished;

    while (true) {
        if (CheckCancelled(generation)) {
            result = TrackResult::kCancelled;
            break;
        }

        if (!eos && input_length < input.size()) {
            size_t read = fread(input.data() + input_length, 1, input.size() - input_length, file);
            input_length += read;
            if (read == 0) {
                if (feof(file)) {
                    eos = true;
                } else if (ferror(file)) {
                    result = TrackResult::kFailed;
                    break;
                }
            }
        }

        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = input.data();
        raw.len = input_length;
        raw.eos = eos;
        raw.consumed = 0;
        raw.frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE;

        esp_audio_simple_dec_out_t frame = {};
        frame.buffer = output.data();
        frame.len = output.size();

        ret = esp_audio_simple_dec_process(decoder, &raw, &frame);
        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (frame.needed_size == 0 || frame.needed_size > kMaxOutputBuffer) {
                result = TrackResult::kFailed;
                break;
            }
            output.resize(frame.needed_size);
            continue;
        }
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "Decode error %d in %s", ret, path.c_str());
            result = TrackResult::kFailed;
            break;
        }

        if (raw.consumed > 0) {
            if (raw.consumed > input_length) {
                result = TrackResult::kFailed;
                break;
            }
            std::memmove(input.data(), input.data() + raw.consumed, input_length - raw.consumed);
            input_length -= raw.consumed;
        }

        if (frame.decoded_size > 0) {
            if (!info_ready) {
                esp_audio_simple_dec_info_t info = {};
                if (esp_audio_simple_dec_get_info(decoder, &info) == ESP_AUDIO_ERR_OK) {
                    sample_rate = static_cast<int>(info.sample_rate);
                    channels = info.channel;
                    info_ready = true;
                    ESP_LOGI(TAG, "%s: %d Hz, %d channel(s), %d bit", BaseName(path).c_str(),
                             sample_rate, channels, info.bits_per_sample);
                    if (info.bits_per_sample != 16) {
                        ESP_LOGW(TAG, "Only 16-bit PCM is supported, skipping audio");
                        sample_rate = 0;
                    }
                }
            }

            if (info_ready && sample_rate > 0 && channels > 0) {
                size_t samples = frame.decoded_size / sizeof(int16_t);
                std::vector<int16_t> pcm(samples);
                std::memcpy(pcm.data(), frame.buffer, samples * sizeof(int16_t));

                if (!audio_service_.PushPcmToPlaybackQueue(std::move(pcm), sample_rate, channels)) {
                    result = TrackResult::kCancelled;
                    break;
                }

                uint32_t frames = static_cast<uint32_t>(samples / channels);
                uint32_t increment =
                    static_cast<uint32_t>((static_cast<uint64_t>(frames) * 1000) / sample_rate);
                std::lock_guard<std::mutex> lock(mutex_);
                position_ms_ += increment;
            }
        }

        bool no_progress = raw.consumed == 0 && frame.decoded_size == 0;
        if (eos && no_progress) {
            break;
        }
        if (no_progress && input_length == input.size()) {
            if (input.size() >= kMaxInputBuffer) {
                ESP_LOGW(TAG, "Decoder made no progress in %s", path.c_str());
                result = TrackResult::kFailed;
                break;
            }
            input.resize(std::min(input.size() * 2, kMaxInputBuffer));
        }
    }

    esp_audio_simple_dec_close(decoder);
    fclose(file);
    return result;
}
