#ifndef RFID_READER_H
#define RFID_READER_H

#include <string>

/**
 * Abstract RFID reader interface.
 *
 * A board that exposes an RFID reader returns it from Board::GetRfidReader().
 * Implementations own their bus/device handle and must be safe to poll from a
 * dedicated task; they do not need to be thread-safe against concurrent calls.
 */
class RfidReader {
public:
    virtual ~RfidReader() = default;

    /** Configure the reader. Returns false when the hardware is absent. */
    virtual bool Initialize() = 0;

    /**
     * Poll the reader once.
     *
     * Returns true exactly once for each newly presented card and writes the
     * card UID as uppercase hexadecimal without separators (for example
     * "A1B2C3D4"). Repeated polls while the same card rests on the antenna
     * return false until it is removed.
     */
    virtual bool ReadCard(std::string& uid) = 0;

    virtual bool IsReady() const = 0;

    virtual const char* Name() const = 0;
};

#endif  // RFID_READER_H
