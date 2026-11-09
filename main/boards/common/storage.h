#ifndef STORAGE_H
#define STORAGE_H

/**
 * Abstract block storage interface (currently used for SD cards).
 *
 * A board that exposes removable media returns it from Board::GetStorage().
 * The storage is mounted as a FAT filesystem during board initialization.
 */
class Storage {
public:
    virtual ~Storage() = default;

    /** Mount the filesystem. Returns false when the medium is absent or invalid. */
    virtual bool Initialize() = 0;

    virtual bool IsMounted() const = 0;

    /** VFS mount point without a trailing slash, for example "/sdcard". */
    virtual const char* MountPoint() const = 0;
};

#endif  // STORAGE_H
