#include "phoniebox_service.h"

#include <esp_log.h>
#include <dirent.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>

#include "application.h"
#include "board.h"
#include "display/display.h"
#include "music_player.h"
#include "phoniebox_web_server.h"
#include "rfid_reader.h"
#include "storage.h"

#define TAG "Phoniebox"

namespace {

std::string BaseName(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool IsBusyState(DeviceState state) {
    return state == kDeviceStateConnecting || state == kDeviceStateListening ||
           state == kDeviceStateSpeaking || state == kDeviceStateNotifying;
}

}  // namespace

PhonieboxService& PhonieboxService::GetInstance() {
    static PhonieboxService instance;
    return instance;
}

void PhonieboxService::Initialize() {
    if (initialized_) {
        return;
    }
    initialized_ = true;

    auto& board = Board::GetInstance();
    rfid_ = board.GetRfidReader();
    storage_ = board.GetStorage();

    if (rfid_ == nullptr) {
        ESP_LOGW(TAG, "Board does not provide an RFID reader");
    } else if (!rfid_->IsReady()) {
        ESP_LOGW(TAG, "RFID reader is not ready");
    }

    if (storage_ == nullptr) {
        ESP_LOGW(TAG, "Board does not provide storage");
    } else if (!storage_->IsMounted()) {
        ESP_LOGW(TAG, "Storage is not mounted; music playback is unavailable");
    }

    if (storage_ != nullptr) {
        player_ = std::make_unique<MusicPlayer>(Application::GetInstance().GetAudioService());
        if (!player_->Start()) {
            ESP_LOGE(TAG, "Failed to start the music player");
            player_.reset();
        }
    }

    web_server_ = std::make_unique<PhonieboxWebServer>(*this);
    if (!web_server_->Start()) {
        ESP_LOGE(TAG, "Failed to start the web interface");
        web_server_.reset();
    }

    StartRfidTask();
}

void PhonieboxService::StartRfidTask() {
    if (rfid_ == nullptr || !rfid_->IsReady()) {
        return;
    }

    xTaskCreate(
        [](void* context) {
            auto* service = static_cast<PhonieboxService*>(context);
            std::string uid;
            while (true) {
                if (service->rfid_->ReadCard(uid)) {
                    service->HandleCardScanned(uid);
                }
                vTaskDelay(pdMS_TO_TICKS(80));
            }
        },
        "phoniebox_rfid", 4096, this, 3, nullptr);
}

void PhonieboxService::HandleCardScanned(const std::string& uid) {
    std::string normalized = TagStore::NormalizeUid(uid);
    if (normalized.empty()) {
        return;
    }

    PhonieboxTag tag;
    bool assigned = tag_store_.Get(normalized, tag);
    if (assigned && tag.name.empty()) {
        tag.name = BaseName(tag.path);
    }
    if (!assigned) {
        tag.uid = normalized;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_uid_ = normalized;
        last_tag_ = tag;
        last_tag_assigned_ = assigned;
    }

    ESP_LOGI(TAG, "Tag %s (%s)", normalized.c_str(), assigned ? tag.path.c_str() : "unassigned");

    Application::GetInstance().Schedule([this, normalized, tag, assigned]() {
        auto& app = Application::GetInstance();
        if (IsBusyState(app.GetDeviceState())) {
            app.AbortSpeaking(kAbortReasonNone);
            app.SetDeviceState(kDeviceStateIdle);
        }

        auto* display = Board::GetInstance().GetDisplay();
        if (!assigned) {
            if (display != nullptr) {
                display->ShowNotification("New tag " + normalized);
            }
            return;
        }

        if (player_ == nullptr || !player_->PlayPath(tag.path)) {
            if (display != nullptr) {
                display->ShowNotification("Cannot play " + tag.path);
            }
            return;
        }

        if (display != nullptr) {
            display->ShowNotification("Playing " + (tag.name.empty() ? normalized : tag.name));
        }
    });
}

void PhonieboxService::PauseForInteraction() {
    if (player_ != nullptr) {
        player_->Pause();
    }
}

bool PhonieboxService::PlayPath(const std::string& path) {
    if (player_ == nullptr || !storage_ || !storage_->IsMounted()) {
        return false;
    }
    std::string normalized = NormalizePath(path);
    if (!IsAllowedPath(normalized)) {
        return false;
    }
    return player_->PlayPath(normalized);
}

void PhonieboxService::Pause() {
    if (player_ != nullptr) {
        player_->Pause();
    }
}

void PhonieboxService::Resume() {
    if (player_ != nullptr) {
        player_->Resume();
    }
}

void PhonieboxService::TogglePause() {
    if (player_ != nullptr) {
        player_->TogglePause();
    }
}

void PhonieboxService::Next() {
    if (player_ != nullptr) {
        player_->Next();
    }
}

void PhonieboxService::Previous() {
    if (player_ != nullptr) {
        player_->Previous();
    }
}

void PhonieboxService::Stop() {
    if (player_ != nullptr) {
        player_->Stop();
    }
}

bool PhonieboxService::SetVolume(int volume) {
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return false;
    }
    codec->SetOutputVolume(std::clamp(volume, 0, 100));
    return true;
}

PhonieboxStatus PhonieboxService::GetStatus() const {
    PhonieboxStatus status;
    status.sd_mounted = storage_ != nullptr && storage_->IsMounted();
    status.rfid_ready = rfid_ != nullptr && rfid_->IsReady();
    status.mount_point = storage_ != nullptr ? storage_->MountPoint() : "/sdcard";

    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        status.volume = codec->output_volume();
    }

    if (player_ != nullptr) {
        MusicPlayer::Status player_status = player_->GetStatus();
        status.player_active = player_status.state != MusicPlayer::State::kStopped;
        status.player_paused = player_status.state == MusicPlayer::State::kPaused;
        status.current_path = player_status.path;
        status.current_title = player_status.title;
        status.position_ms = player_status.position_ms;
        status.playlist_index = static_cast<unsigned int>(player_status.index);
        status.playlist_count = static_cast<unsigned int>(player_status.count);
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    status.last_uid = last_uid_;
    status.last_tag_path = last_tag_.path;
    status.last_tag_name = last_tag_.name;
    status.last_tag_assigned = last_tag_assigned_;
    return status;
}

std::vector<PhonieboxTag> PhonieboxService::GetTags() { return tag_store_.List(); }

bool PhonieboxService::AssignTag(const std::string& uid, const std::string& path,
                                 const std::string& name) {
    std::string normalized = TagStore::NormalizeUid(uid);
    std::string normalized_path = NormalizePath(path);
    if (normalized.empty() || !IsAllowedPath(normalized_path)) {
        return false;
    }

    struct stat info = {};
    if (stat(normalized_path.c_str(), &info) != 0) {
        return false;
    }
    if (S_ISREG(info.st_mode) && !MusicPlayer::IsSupportedFile(normalized_path)) {
        return false;
    }
    if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) {
        return false;
    }

    std::string tag_name = name.empty() ? BaseName(normalized_path) : name;
    if (!tag_store_.Set(normalized, normalized_path, tag_name)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    if (last_uid_ == normalized) {
        last_tag_.uid = normalized;
        last_tag_.path = normalized_path;
        last_tag_.name = tag_name;
        last_tag_assigned_ = true;
    }
    return true;
}

bool PhonieboxService::RemoveTag(const std::string& uid) {
    std::string normalized = TagStore::NormalizeUid(uid);
    if (normalized.empty() || !tag_store_.Remove(normalized)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    if (last_uid_ == normalized) {
        last_tag_ = PhonieboxTag{normalized, "", ""};
        last_tag_assigned_ = false;
    }
    return true;
}

bool PhonieboxService::Browse(const std::string& path, std::vector<PhonieboxFsEntry>& entries) {
    entries.clear();
    if (!storage_ || !storage_->IsMounted()) {
        return false;
    }

    std::string normalized = NormalizePath(path);
    if (!IsAllowedPath(normalized)) {
        return false;
    }

    DIR* directory = opendir(normalized.c_str());
    if (directory == nullptr) {
        return false;
    }

    struct dirent* entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        std::string child = normalized;
        if (child.back() != '/') {
            child.push_back('/');
        }
        child += entry->d_name;

        struct stat info = {};
        if (stat(child.c_str(), &info) != 0) {
            continue;
        }
        bool is_dir = S_ISDIR(info.st_mode);
        bool playable = S_ISREG(info.st_mode) && MusicPlayer::IsSupportedFile(child);
        if (!is_dir && !playable) {
            continue;
        }
        entries.push_back({entry->d_name, child, is_dir, playable});
        if (entries.size() >= 300) {
            break;
        }
    }
    closedir(directory);

    std::sort(entries.begin(), entries.end(),
              [](const PhonieboxFsEntry& a, const PhonieboxFsEntry& b) {
                  if (a.is_dir != b.is_dir) {
                      return a.is_dir;
                  }
                  return Lowercase(a.name) < Lowercase(b.name);
              });
    return true;
}

bool PhonieboxService::IsAllowedPath(const std::string& path) const {
    if (storage_ == nullptr) {
        return false;
    }
    std::string mount = storage_->MountPoint();
    if (mount.empty()) {
        return false;
    }
    if (path == mount) {
        return true;
    }
    std::string prefix = mount.back() == '/' ? mount : mount + "/";
    return path.rfind(prefix, 0) == 0 && path.find("..") == std::string::npos;
}

std::string PhonieboxService::NormalizePath(const std::string& path) const {
    std::string mount = storage_ != nullptr ? storage_->MountPoint() : "/sdcard";
    std::string result = path.empty() ? mount : path;
    while (result.size() > 1 && result.back() == '/') {
        result.pop_back();
    }
    return result;
}
