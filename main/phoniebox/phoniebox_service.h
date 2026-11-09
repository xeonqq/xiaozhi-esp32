#ifndef PHONIEBOX_SERVICE_H
#define PHONIEBOX_SERVICE_H

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "tag_store.h"

class MusicPlayer;
class PhonieboxWebServer;
class RfidReader;
class Storage;

struct PhonieboxFsEntry {
    std::string name;
    std::string path;
    bool is_dir = false;
    bool playable = false;
};

struct PhonieboxStatus {
    bool sd_mounted = false;
    bool rfid_ready = false;
    std::string mount_point = "/sdcard";

    bool player_active = false;
    bool player_paused = false;
    std::string current_path;
    std::string current_title;
    unsigned int position_ms = 0;
    unsigned int playlist_index = 0;
    unsigned int playlist_count = 0;

    int volume = 0;

    std::string last_uid;
    std::string last_tag_path;
    std::string last_tag_name;
    bool last_tag_assigned = false;
};

/**
 * Glue between the board peripherals, the persistent tag store, the local
 * music player and the web interface.
 *
 * The service is a singleton because the board's buttons, the RFID task and
 * the web server all need the same player and tag store.
 */
class PhonieboxService {
public:
    static PhonieboxService& GetInstance();

    PhonieboxService(const PhonieboxService&) = delete;
    PhonieboxService& operator=(const PhonieboxService&) = delete;

    void Initialize();

    /** Called when the voice assistant starts interacting, to duck music. */
    void PauseForInteraction();

    bool PlayPath(const std::string& path);
    void Pause();
    void Resume();
    void TogglePause();
    void Next();
    void Previous();
    void Stop();
    bool SetVolume(int volume);

    PhonieboxStatus GetStatus() const;

    std::vector<PhonieboxTag> GetTags();
    bool AssignTag(const std::string& uid, const std::string& path, const std::string& name);
    bool RemoveTag(const std::string& uid);

    bool Browse(const std::string& path, std::vector<PhonieboxFsEntry>& entries);

private:
    PhonieboxService() = default;
    ~PhonieboxService() = default;

    void StartRfidTask();
    void HandleCardScanned(const std::string& uid);
    bool IsAllowedPath(const std::string& path) const;
    std::string NormalizePath(const std::string& path) const;

    bool initialized_ = false;
    RfidReader* rfid_ = nullptr;
    Storage* storage_ = nullptr;
    std::unique_ptr<MusicPlayer> player_;
    std::unique_ptr<PhonieboxWebServer> web_server_;
    TagStore tag_store_;

    mutable std::mutex state_mutex_;
    std::string last_uid_;
    PhonieboxTag last_tag_;
    bool last_tag_assigned_ = false;
};

#endif  // PHONIEBOX_SERVICE_H
