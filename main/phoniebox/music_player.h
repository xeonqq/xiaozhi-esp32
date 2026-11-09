#ifndef PHONIEBOX_MUSIC_PLAYER_H
#define PHONIEBOX_MUSIC_PLAYER_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class AudioService;

/**
 * Local music player for files on the SD card.
 *
 * A dedicated task decodes MP3/WAV/FLAC/M4A/AAC/OGG files with the Espressif
 * simple decoder and pushes PCM into AudioService's playback queue, sharing
 * the speaker with XiaoZhi's voice responses.
 */
class MusicPlayer {
public:
    enum class State {
        kStopped,
        kPlaying,
        kPaused,
    };

    struct Status {
        State state = State::kStopped;
        std::string path;
        std::string title;
        size_t index = 0;
        size_t count = 0;
        uint32_t position_ms = 0;
    };

    explicit MusicPlayer(AudioService& audio_service);
    ~MusicPlayer();

    bool Start();

    bool PlayPath(const std::string& path);
    void Pause();
    void Resume();
    void TogglePause();
    void Next();
    void Previous();
    void Stop();

    Status GetStatus() const;

    static bool IsSupportedFile(const std::string& path);

private:
    enum class TrackResult {
        kFinished,
        kCancelled,
        kFailed,
    };

    void TaskLoop();
    TrackResult DecodeTrack(const std::string& path, uint32_t generation);
    bool LoadPlaylist(const std::string& path, bool single_file);
    bool CheckCancelled(uint32_t generation);

    AudioService& audio_service_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::string> playlist_;
    size_t index_ = 0;
    State state_ = State::kStopped;
    uint32_t generation_ = 0;
    bool task_stop_ = false;
    bool task_started_ = false;
    TaskHandle_t task_handle_ = nullptr;
    std::string current_path_;
    std::string current_title_;
    uint32_t position_ms_ = 0;
};

#endif  // PHONIEBOX_MUSIC_PLAYER_H
