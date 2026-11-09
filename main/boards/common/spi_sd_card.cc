#include "spi_sd_card.h"

#include <driver/sdspi_host.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>

#define TAG "SpiSdCard"

SpiSdCard::SpiSdCard(spi_host_device_t host, gpio_num_t cs_pin, const char* mount_point)
    : host_(host), cs_pin_(cs_pin), mount_point_(mount_point) {}

SpiSdCard::~SpiSdCard() {
    if (mounted_ && card_ != nullptr) {
        esp_vfs_fat_sdcard_unmount(mount_point_, card_);
        card_ = nullptr;
        mounted_ = false;
    }
}

bool SpiSdCard::Initialize() {
    if (mounted_) {
        return true;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = host_;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = host_;
    slot_config.gpio_cs = cs_pin_;

    esp_vfs_fat_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 6;
    mount_config.allocation_unit_size = 16 * 1024;
    mount_config.disk_status_check_enable = false;
    mount_config.use_one_fat = false;

    esp_err_t err =
        esp_vfs_fat_sdspi_mount(mount_point_, &host, &slot_config, &mount_config, &card_);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD card not mounted at %s: %s", mount_point_, esp_err_to_name(err));
        card_ = nullptr;
        return false;
    }

    mounted_ = true;
    ESP_LOGI(TAG, "SD card mounted at %s (%s)", mount_point_, card_->cid.name);
    return true;
}
