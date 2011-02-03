#ifndef SQUID_FS_ROCK_DIR_MAP_H
#define SQUID_FS_ROCK_DIR_MAP_H

#include "ipc/AtomicWord.h"
#include "ipc/SharedMemory.h"

class StoreEntryBasics {
public:
    void set(const StoreEntry &from);

    /* START OF ON-DISK STORE_META_STD TLV field */
    time_t timestamp;
    time_t lastref;
    time_t expires;
    time_t lastmod;
    uint64_t swap_file_sz;
    u_short refcount;
    u_short flags;
    /* END OF ON-DISK STORE_META_STD */
};

namespace Rock {

/// DirMap entry
class Slot {
public:
    /// possible persistent states
    typedef enum {
        Empty, ///< ready for writing, with nothing of value
        Writeable, ///< transitions from Empty to Readable
        Readable, ///< ready for reading
    } State;

    void setKey(const cache_key *const aKey);
    bool checkKey(const cache_key *const aKey) const;

    bool sharedLock() const; ///< lock for reading or return false
    bool exclusiveLock(); ///< lock for modification or return false
    void releaseSharedLock() const; ///< undo successful sharedLock()
    void releaseExclusiveLock(); ///< undo successful exclusiveLock()

public:
    // we want two uint64_t, but older GCCs lack __sync_fetch_and_add_8
    AtomicWordT<uint32_t> key_[4]; ///< MD5 entry key
    StoreEntryBasics seBasics; ///< basic store entry data
    AtomicWordT<uint8_t> state; ///< current state
    AtomicWordT<uint8_t> waitingToBeFreed; ///< a state-independent mark

private:
    mutable AtomicWord readers; ///< number of users trying to read
    AtomicWord writers; ///< number of writers trying to modify the slot
};

/// \ingroup Rock
/// map of used db slots indexed by sfileno
class DirMap
{
public:
    DirMap(const char *const aPath, const int limit); ///< create a new shared DirMap
    DirMap(const char *const aPath); ///< open an existing shared DirMap

    /// finds space for writing a new entry or returns nil
    StoreEntryBasics *openForWriting(const cache_key *const key, sfileno &fileno);
    /// finish writing a new entry, leaves the entry opened for reading
    void closeForWriting(const sfileno fileno);

    /// stores entry info at the requested slot or returns false
    bool putAt(const StoreEntry &e, const sfileno fileno);

    /// mark the slot as waiting to be freed and, if possible, free it
    void free(const sfileno fileno);

    /// open slot for reading, increments read level
    const StoreEntryBasics *openForReading(const cache_key *const key, sfileno &fileno);
    /// open slot for reading, increments read level
    const StoreEntryBasics *openForReadingAt(const sfileno fileno);
    /// close slot after reading, decrements read level
    void closeForReading(const sfileno fileno);

    bool full() const; ///< there are no empty slots left
    bool valid(int n) const; ///< whether n is a valid slot coordinate
    int entryCount() const; ///< number of used slots
    int entryLimit() const; ///< maximum number of slots that can be used

    static int AbsoluteEntryLimit(); ///< maximum entryLimit() possible

private:
    struct Shared {
        Shared(const int aLimit);

        const AtomicWord limit; ///< maximum number of map slots
        AtomicWord count; ///< current number of map slots

        Slot slots[]; ///< slots storage
    };

    int slotIdx(const cache_key *const key) const;
    Slot &slot(const cache_key *const key);
    const StoreEntryBasics *openForReading(Slot &s);
    void freeIfNeeded(Slot &s);
    String sharedMemoryName();

    static int SharedSize(const int limit);

    const String path; ///< cache_dir path, used for logging
    SharedMemory shm; ///< shared memory segment
    Shared *shared; ///< pointer to shared memory
};

} // namespace Rock

// We do not reuse struct _fileMap because we cannot control its size,
// resulting in sfilenos that are pointing beyond the database.

#endif /* SQUID_FS_ROCK_DIR_MAP_H */
