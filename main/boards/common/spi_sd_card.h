#ifndef SPI_SD_CARD_H
#define SPI_SD_CARD_H

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <sdmmc_cmd.h>

#include "storage.h"

/**
 * SD card mounted over an existing SPI bus (SDSPI).
 *
 * The board owns the SPI bus and is expected to initialize it before calling
 * Initialize(); the bus may be shared with other SPI devices such as an
 * MFRC522 RFID reader. The card is mounted as a FAT filesystem.
 */
class SpiSdCard : public Storage {
public:
    SpiSdCard(spi_host_device_t host, gpio_num_t cs_pin, const char* mount_point);
    ~SpiSdCard() override;

    bool Initialize() override;
    bool IsMounted() const override { return mounted_; }
    const char* MountPoint() const override { return mount_point_; }

private:
    spi_host_device_t host_;
    gpio_num_t cs_pin_;
    const char* mount_point_;
    sdmmc_card_t* card_ = nullptr;
    bool mounted_ = false;
};

#endif  // SPI_SD_CARD_H
