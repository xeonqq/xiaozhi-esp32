#include "rc522_rfid_reader.h"

#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

#define TAG "Rc522"

namespace {

// MFRC522 register map (only the registers used here).
constexpr uint8_t kCommandReg = 0x01;
constexpr uint8_t kComIEnReg = 0x02;
constexpr uint8_t kComIrqReg = 0x04;
constexpr uint8_t kErrorReg = 0x06;
constexpr uint8_t kFifoDataReg = 0x09;
constexpr uint8_t kFifoLevelReg = 0x0A;
constexpr uint8_t kBitFramingReg = 0x0D;
constexpr uint8_t kModeReg = 0x11;
constexpr uint8_t kTxControlReg = 0x14;
constexpr uint8_t kTxASKReg = 0x15;
constexpr uint8_t kCollReg = 0x0E;
constexpr uint8_t kRFCfgReg = 0x26;
constexpr uint8_t kTModeReg = 0x2A;
constexpr uint8_t kTPrescalerReg = 0x2B;
constexpr uint8_t kTReloadRegH = 0x2C;
constexpr uint8_t kTReloadRegL = 0x2D;
constexpr uint8_t kVersionReg = 0x37;

// MFRC522 commands.
constexpr uint8_t kPcdIdle = 0x00;
constexpr uint8_t kPcdTransceive = 0x0C;
constexpr uint8_t kPcdSoftReset = 0x0F;

// PICC commands / values.
constexpr uint8_t kPiccReqA = 0x26;
constexpr uint8_t kPiccWupA = 0x52;
constexpr uint8_t kPiccAnticoll = 0x93;
constexpr uint8_t kPiccCascadeLevel2 = 0x95;
constexpr uint8_t kPiccSelect = 0x93;
constexpr uint8_t kPiccHalt = 0x50;
constexpr uint8_t kPiccNvbAnticoll = 0x20;
constexpr uint8_t kPiccNvbSelect = 0x70;
constexpr uint8_t kCascadeTag = 0x88;

// ComIrqReg bits.
constexpr uint8_t kIrqTimer = 0x01;
constexpr uint8_t kIrqErr = 0x02;
constexpr uint8_t kIrqRx = 0x20;
constexpr uint8_t kIrqIdle = 0x10;

// ErrorReg bits: protocol, parity, CRC, collision, buffer overflow, write.
constexpr uint8_t kErrorMask = 0x1B;

constexpr uint8_t kCommandWaitMask = kIrqRx | kIrqIdle | kIrqTimer | kIrqErr;
constexpr int64_t kCommandTimeoutUs = 50 * 1000;

bool IsValidVersion(uint8_t version) {
    switch (version) {
        case 0x12:  // Some clones
        case 0x82:  // FM17522 clone
        case 0x88:  // FM17522 clone
        case 0x90:  // v0.0
        case 0x91:  // v1.0
        case 0x92:  // v2.0
        case 0xB2:  // Some clones
            return true;
        default:
            return false;
    }
}

std::string ToHex(const uint8_t* data, size_t length) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        result.push_back(kHex[data[i] >> 4]);
        result.push_back(kHex[data[i] & 0x0F]);
    }
    return result;
}

}  // namespace

Rc522RfidReader::Rc522RfidReader(spi_host_device_t host, gpio_num_t cs_pin, gpio_num_t rst_pin,
                                 uint32_t clock_hz)
    : host_(host), cs_pin_(cs_pin), rst_pin_(rst_pin), clock_hz_(clock_hz) {}

Rc522RfidReader::~Rc522RfidReader() {
    if (device_ != nullptr) {
        spi_bus_remove_device(device_);
        device_ = nullptr;
    }
}

bool Rc522RfidReader::Initialize() {
    if (ready_) {
        return true;
    }

    if (rst_pin_ != GPIO_NUM_NC) {
        gpio_config_t io_config = {};
        io_config.pin_bit_mask = 1ULL << rst_pin_;
        io_config.mode = GPIO_MODE_OUTPUT;
        io_config.pull_up_en = GPIO_PULLUP_DISABLE;
        io_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_config.intr_type = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&io_config));

        // MFRC522 reset is active low.
        gpio_set_level(rst_pin_, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(rst_pin_, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    spi_device_interface_config_t device_config = {};
    device_config.clock_speed_hz = static_cast<int>(clock_hz_);
    device_config.mode = 0;
    device_config.spics_io_num = cs_pin_;
    // The driver only uses polling transactions, but FreeRTOS rejects a
    // zero-length transaction queue when the SPI device is added.
    device_config.queue_size = 1;
    esp_err_t err = spi_bus_add_device(host_, &device_config, &device_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(err));
        device_ = nullptr;
        return false;
    }

    if (!SoftReset() || !DetectVersion()) {
        ESP_LOGE(TAG, "MFRC522 did not respond (check wiring and 3.3 V supply)");
        return false;
    }

    // 25 ms timer for REQA/anticollision. Values match the NXP reference driver.
    WriteRegister(kTModeReg, 0x80);
    WriteRegister(kTPrescalerReg, 0xA9);
    WriteRegister(kTReloadRegH, 0x03);
    WriteRegister(kTReloadRegL, 0xE8);

    // Force 100% ASK modulation and use the ISO14443A CRC preset.
    WriteRegister(kTxASKReg, 0x40);
    WriteRegister(kModeReg, 0x3D);

    AntennaOn();
    SetAntennaGain(7);

    // Temporary bring-up diagnostics: verify the RF configuration actually
    // stuck. Expected: TxControl 0x83, RFCfg 0x78, TxASK 0x40, Mode 0x3D,
    // TMode 0x80, TPrescaler 0xA9, reload 0x03E8.
    ESP_LOGI(TAG, "MFRC522 config: TxControl 0x%02X RFCfg 0x%02X TxASK 0x%02X Mode 0x%02X "
                  "TMode 0x%02X TPre 0x%02X Reload 0x%02X%02X",
             ReadRegister(kTxControlReg), ReadRegister(kRFCfgReg), ReadRegister(kTxASKReg),
             ReadRegister(kModeReg), ReadRegister(kTModeReg), ReadRegister(kTPrescalerReg),
             ReadRegister(kTReloadRegH), ReadRegister(kTReloadRegL));

    ready_ = true;
    ESP_LOGI(TAG, "MFRC522 ready (SPI %d MHz)", static_cast<int>(clock_hz_ / 1000000));
    return true;
}

void Rc522RfidReader::LogCardDiagnostic(uint16_t atqa) {
    int64_t now = esp_timer_get_time();
    if (now - last_diag_us_ < kDiagLogIntervalUs) {
        return;
    }
    last_diag_us_ = now;
    ESP_LOGW(TAG, "Card in the field (ATQA 0x%04X) but the UID could not be read", atqa);
}

void Rc522RfidReader::LogPollDiagnostic() {
    int64_t now = esp_timer_get_time();
    if (now - last_diag_us_ < kDiagLogIntervalUs) {
        return;
    }
    last_diag_us_ = now;
    // Temporary bring-up diagnostic for the no-PICC case.
    ESP_LOGW(TAG, "No PICC response (ComIrq 0x%02X Error 0x%02X FifoLevel 0x%02X)",
             ReadRegister(kComIrqReg), ReadRegister(kErrorReg), ReadRegister(kFifoLevelReg));
}

bool Rc522RfidReader::ReadCard(std::string& uid) {
    if (!ready_) {
        return false;
    }

    uint16_t atqa = 0;
    if (!RequestA(atqa)) {
        if (card_present_ && esp_timer_get_time() - last_seen_us_ > kCardRemoveTimeoutUs) {
            card_present_ = false;
            last_uid_.clear();
        }
        LogPollDiagnostic();
        return false;
    }

    // Cascade level 1. The answer is always five bytes: four UID bytes plus
    // the BCC. A 7-byte UID card puts the cascade tag (0x88) in place of
    // UID3 and continues with a second anticollision round.
    uint8_t buffer[5] = {};
    uint8_t length = 0;
    if (!Anticoll(kPiccAnticoll, buffer, length)) {
        LogCardDiagnostic(atqa);
        return false;
    }

    uint8_t uid_bytes[7] = {};
    uint8_t uid_length = 0;
    if (buffer[0] == kCascadeTag) {
        if ((buffer[0] ^ buffer[1] ^ buffer[2] ^ buffer[3]) != buffer[4]) {
            return false;
        }
        std::memcpy(uid_bytes, &buffer[1], 3);

        uint8_t level2[5] = {};
        if (!Anticoll(kPiccCascadeLevel2, level2, length)) {
            LogCardDiagnostic(atqa);
            return false;
        }
        if ((level2[0] ^ level2[1] ^ level2[2] ^ level2[3]) != level2[4]) {
            return false;
        }
        std::memcpy(&uid_bytes[3], level2, 4);
        uid_length = 7;
    } else {
        if ((buffer[0] ^ buffer[1] ^ buffer[2] ^ buffer[3]) != buffer[4]) {
            return false;
        }
        std::memcpy(uid_bytes, buffer, 4);
        uid_length = 4;
    }

    last_seen_us_ = esp_timer_get_time();
    std::string current_uid = ToHex(uid_bytes, uid_length);

    if (card_present_ && current_uid == last_uid_) {
        Halt();
        return false;
    }

    if (uid_length == 7) {
        // Selecting happens at the deepest cascade level with its four UID
        // bytes.
        Select(kPiccCascadeLevel2, &uid_bytes[3], 4);
    } else {
        Select(kPiccSelect, uid_bytes, 4);
    }
    Halt();

    card_present_ = true;
    last_uid_ = current_uid;
    uid = std::move(current_uid);
    return true;
}

bool Rc522RfidReader::DetectVersion() {
    uint8_t version = ReadRegister(kVersionReg);
    if (!IsValidVersion(version)) {
        ESP_LOGE(TAG, "Unexpected MFRC522 version 0x%02X", version);
        return false;
    }
    ESP_LOGI(TAG, "MFRC522 firmware version 0x%02X", version);
    return true;
}

bool Rc522RfidReader::SoftReset() {
    WriteRegister(kCommandReg, kPcdSoftReset);
    int64_t deadline = esp_timer_get_time() + kCommandTimeoutUs;
    while (esp_timer_get_time() < deadline) {
        if ((ReadRegister(kCommandReg) & 0x10) == 0) {
            return true;
        }
        esp_rom_delay_us(100);
    }
    return false;
}

void Rc522RfidReader::AntennaOn() {
    uint8_t value = ReadRegister(kTxControlReg);
    if ((value & 0x03) != 0x03) {
        WriteRegister(kTxControlReg, value | 0x03);
    }
}

void Rc522RfidReader::SetAntennaGain(uint8_t gain) {
    // RFCfgReg bits 6:4 select receiver gain; only the lower three bits are valid.
    uint8_t value = ReadRegister(kRFCfgReg);
    value = static_cast<uint8_t>((value & 0x8F) | ((gain & 0x07) << 4));
    WriteRegister(kRFCfgReg, value);
}

bool Rc522RfidReader::RequestA(uint16_t& atqa) {
    WriteRegister(kCommandReg, kPcdIdle);
    WriteRegister(kComIEnReg, 0x00);
    WriteRegister(kComIrqReg, 0x7F);
    WriteRegister(kFifoLevelReg, 0x80);
    // Do not clear values received after a collision.
    ClearRegisterBitMask(kCollReg, 0x20);
    // WUPA instead of REQA: a card left in the HALT state by an earlier
    // session only answers to WUPA, and this driver polls continuously.
    WriteRegister(kFifoDataReg, kPiccWupA);
    WriteRegister(kBitFramingReg, 0x07);

    WriteRegister(kCommandReg, kPcdTransceive);
    SetRegisterBitMask(kBitFramingReg, 0x80);

    bool ok = WaitForCommand(kIrqRx);
    ClearRegisterBitMask(kBitFramingReg, 0x80);
    if (!ok) {
        return false;
    }
    if ((ReadRegister(kErrorReg) & kErrorMask) != 0) {
        return false;
    }
    if (ReadRegister(kFifoLevelReg) != 0x02) {
        return false;
    }

    uint8_t buffer[2] = {};
    ReadFifo(buffer, sizeof(buffer));
    atqa = static_cast<uint16_t>(buffer[0] | (buffer[1] << 8));
    return true;
}

bool Rc522RfidReader::Anticoll(uint8_t sel_code, uint8_t* buffer, uint8_t& length) {
    WriteRegister(kCommandReg, kPcdIdle);
    WriteRegister(kComIEnReg, 0x00);
    WriteRegister(kComIrqReg, 0x7F);
    WriteRegister(kFifoLevelReg, 0x80);

    const uint8_t frame[] = {sel_code, kPiccNvbAnticoll};
    WriteFifo(frame, sizeof(frame));
    WriteRegister(kBitFramingReg, 0x00);
    WriteRegister(kCommandReg, kPcdTransceive);
    SetRegisterBitMask(kBitFramingReg, 0x80);

    bool ok = WaitForCommand(kIrqRx);
    ClearRegisterBitMask(kBitFramingReg, 0x80);
    if (!ok) {
        return false;
    }
    if ((ReadRegister(kErrorReg) & kErrorMask) != 0) {
        return false;
    }
    // The anticollision answer is always five bytes: four UID bytes (or the
    // cascade tag plus three UID bytes) and the BCC.
    if (ReadRegister(kFifoLevelReg) != 0x05) {
        return false;
    }

    length = 0x05;
    ReadFifo(buffer, length);
    return true;
}

bool Rc522RfidReader::Select(uint8_t sel_code, const uint8_t* uid, uint8_t length) {
    WriteRegister(kCommandReg, kPcdIdle);
    WriteRegister(kComIEnReg, 0x00);
    WriteRegister(kComIrqReg, 0x7F);
    WriteRegister(kFifoLevelReg, 0x80);

    uint8_t frame[11] = {sel_code, kPiccNvbSelect};
    std::memcpy(&frame[2], uid, length);
    uint8_t bcc = 0;
    for (uint8_t i = 0; i < length; ++i) {
        bcc = static_cast<uint8_t>(bcc ^ uid[i]);
    }
    frame[2 + length] = bcc;
    WriteFifo(frame, static_cast<size_t>(length) + 3);
    WriteRegister(kBitFramingReg, 0x00);
    WriteRegister(kCommandReg, kPcdTransceive);
    SetRegisterBitMask(kBitFramingReg, 0x80);

    bool ok = WaitForCommand(kIrqRx);
    ClearRegisterBitMask(kBitFramingReg, 0x80);
    return ok && (ReadRegister(kErrorReg) & kErrorMask) == 0;
}

void Rc522RfidReader::Halt() {
    WriteRegister(kCommandReg, kPcdIdle);
    WriteRegister(kComIrqReg, 0x7F);
    WriteRegister(kFifoLevelReg, 0x80);

    const uint8_t frame[] = {kPiccHalt, 0x00};
    WriteFifo(frame, sizeof(frame));
    WriteRegister(kBitFramingReg, 0x00);
    WriteRegister(kCommandReg, kPcdTransceive);
    SetRegisterBitMask(kBitFramingReg, 0x80);
    WaitForCommand(kIrqRx);
    ClearRegisterBitMask(kBitFramingReg, 0x80);
}

bool Rc522RfidReader::WaitForCommand(uint8_t irq_mask) {
    int64_t deadline = esp_timer_get_time() + kCommandTimeoutUs;
    while (esp_timer_get_time() < deadline) {
        uint8_t irq = ReadRegister(kComIrqReg);
        if ((irq & irq_mask) != 0) {
            return true;
        }
        if ((irq & kIrqTimer) != 0) {
            return false;
        }
        esp_rom_delay_us(100);
    }
    return false;
}

uint8_t Rc522RfidReader::ReadRegister(uint8_t reg) {
    // MFRC522 SPI address byte: bit 7 set for reads, bits 6:1 hold the address.
    // Without the read flag the transaction is a write and MISO stays idle.
    uint8_t tx_buffer[2] = {static_cast<uint8_t>(((reg << 1) & 0x7E) | 0x80), 0x00};
    uint8_t rx_buffer[2] = {};
    spi_transaction_t transaction = {};
    transaction.length = sizeof(tx_buffer) * 8;
    transaction.tx_buffer = tx_buffer;
    transaction.rx_buffer = rx_buffer;
    if (spi_device_polling_transmit(device_, &transaction) != ESP_OK) {
        return 0;
    }
    return rx_buffer[1];
}

void Rc522RfidReader::WriteRegister(uint8_t reg, uint8_t value) {
    uint8_t tx_buffer[2] = {static_cast<uint8_t>((reg << 1) & 0x7E), value};
    spi_transaction_t transaction = {};
    transaction.length = sizeof(tx_buffer) * 8;
    transaction.tx_buffer = tx_buffer;
    spi_device_polling_transmit(device_, &transaction);
}

void Rc522RfidReader::ReadFifo(uint8_t* data, size_t length) {
    if (length > 64) {
        length = 64;
    }
    uint8_t tx_buffer[65] = {};
    uint8_t rx_buffer[65] = {};
    tx_buffer[0] = static_cast<uint8_t>(((kFifoDataReg << 1) & 0x7E) | 0x80);
    spi_transaction_t transaction = {};
    transaction.length = (length + 1) * 8;
    transaction.tx_buffer = tx_buffer;
    transaction.rx_buffer = rx_buffer;
    if (spi_device_polling_transmit(device_, &transaction) == ESP_OK) {
        std::memcpy(data, &rx_buffer[1], length);
    }
}

void Rc522RfidReader::WriteFifo(const uint8_t* data, size_t length) {
    if (length > 64) {
        length = 64;
    }
    uint8_t tx_buffer[65] = {};
    tx_buffer[0] = static_cast<uint8_t>((kFifoDataReg << 1) & 0x7E);
    std::memcpy(&tx_buffer[1], data, length);
    spi_transaction_t transaction = {};
    transaction.length = (length + 1) * 8;
    transaction.tx_buffer = tx_buffer;
    spi_device_polling_transmit(device_, &transaction);
}

void Rc522RfidReader::SetRegisterBitMask(uint8_t reg, uint8_t mask) {
    WriteRegister(reg, static_cast<uint8_t>(ReadRegister(reg) | mask));
}

void Rc522RfidReader::ClearRegisterBitMask(uint8_t reg, uint8_t mask) {
    WriteRegister(reg, static_cast<uint8_t>(ReadRegister(reg) & ~mask));
}
