#ifndef PHONIEBOX_TAG_STORE_H
#define PHONIEBOX_TAG_STORE_H

#include <string>
#include <vector>

struct PhonieboxTag {
    std::string uid;
    std::string path;
    std::string name;
};

/**
 * Persistent RFID tag assignments.
 *
 * Stored in the "phoniebox" NVS namespace under short keys ("t" + UID), so the
 * mapping survives reboots without touching the SD card. Values are
 * "path\nname".
 */
class TagStore {
public:
    /** Normalize a UID to uppercase hex, or return an empty string if invalid. */
    static std::string NormalizeUid(const std::string& uid);

    bool Get(const std::string& uid, PhonieboxTag& tag);
    bool Set(const std::string& uid, const std::string& path, const std::string& name);
    bool Remove(const std::string& uid);
    std::vector<PhonieboxTag> List();

private:
    static bool MakeKey(const std::string& uid, char* key, size_t key_size);
};

#endif  // PHONIEBOX_TAG_STORE_H
