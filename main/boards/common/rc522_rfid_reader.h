#ifndef RC522_RFID_READER_H
#define RC522_RFID_READER_H

#include <driver/gpio.h>
#include <driver/spi_master.h>

#include <cstdint>
#include <string>

#include "rfid_reader.h"

/**
 * MFRC522 / RC522 RFID reader on an existing SPI bus.
 *
 * The board owns the SPI bus (it is usually shared with an SD card); this
 * driver only adds its own chip-select device. Poll ReadCard() from a single
 * task at a modest rate (about 50-100 ms).
 */
class Rc522RfidReader : public RfidReader {
public:
    Rc522RfidReader(spi_host_device_t host, gpio_num_t cs_pin, gpio_num_t rst_pin,
                    gpio_num_t irq_pin = GPIO_NUM_NC, uint32_t clock_hz = 4 * 1000 * 1000);
    ~Rc522RfidReader() override;

    bool Initialize() override;
    bool ReadCard(std::string& uid) override;
    bool IsReady() const override { return ready_; }
    const char* Name() const override { return "MFRC522/RC522"; }

private:
    static constexpr int64_t kCardRemoveTimeoutUs = 500 * 1000;
    static constexpr int64_t kDiagLogIntervalUs = 30 * 1000 * 1000;

    bool DetectVersion();
    bool SoftReset();
    void AntennaOn();
    void SetupIrq();
    static void IrqIsrHandler(void* arg);
    void SetAntennaGain(uint8_t gain);
    bool RequestA(uint16_t& atqa);
    void LogCardDiagnostic(uint16_t atqa);
    void LogPollDiagnostic();
    bool Anticoll(uint8_t sel_code, uint8_t* buffer, uint8_t& length);
    bool Select(uint8_t sel_code, const uint8_t* uid, uint8_t length);
    void Halt();
    bool WaitForCommand(uint8_t irq_mask);

    uint8_t ReadRegister(uint8_t reg);
    void WriteRegister(uint8_t reg, uint8_t value);
    void ReadFifo(uint8_t* data, size_t length);
    void WriteFifo(const uint8_t* data, size_t length);
    void SetRegisterBitMask(uint8_t reg, uint8_t mask);
    void ClearRegisterBitMask(uint8_t reg, uint8_t mask);

    spi_host_device_t host_;
    gpio_num_t cs_pin_;
    gpio_num_t rst_pin_;
    gpio_num_t irq_pin_;
    uint32_t clock_hz_;
    spi_device_handle_t device_ = nullptr;

    uint8_t com_ien_value_ = 0x00;
    volatile uint32_t irq_edges_ = 0;

    bool ready_ = false;
    bool card_present_ = false;
    int64_t last_seen_us_ = 0;
    int64_t last_diag_us_ = 0;
    std::string last_uid_;
};

#endif  // RC522_RFID_READER_H
