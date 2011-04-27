/*
 * $Id$
 *
 * DEBUG: section 47    Store Directory Routines
 */

#include "config.h"
#include "Parsing.h"
#include <iomanip>
#include "MemObject.h"
#include "SquidMath.h"
#include "base/RunnersRegistry.h"
#include "DiskIO/DiskIOModule.h"
#include "DiskIO/DiskIOStrategy.h"
#include "DiskIO/ReadRequest.h"
#include "DiskIO/WriteRequest.h"
#include "fs/rock/RockSwapDir.h"
#include "fs/rock/RockIoState.h"
#include "fs/rock/RockIoRequests.h"
#include "fs/rock/RockRebuild.h"

// must be divisible by 1024 due to cur_size and max_size KB madness
const int64_t Rock::SwapDir::HeaderSize = 16*1024;

Rock::SwapDir::SwapDir(): ::SwapDir("rock"), filePath(NULL), io(NULL), map(NULL)
{
}

Rock::SwapDir::~SwapDir()
{
    delete io;
    delete map;
    safe_free(filePath);
}

StoreSearch *
Rock::SwapDir::search(String const url, HttpRequest *)
{
    assert(false); return NULL; // XXX: implement
}

// called when Squid core needs a StoreEntry with a given key
StoreEntry *
Rock::SwapDir::get(const cache_key *key)
{
    if (!map)
        return NULL;

    sfileno fileno;
    const Ipc::StoreMapSlot *const slot = map->openForReading(key, fileno);
    if (!slot)
        return NULL;

    const Ipc::StoreMapSlot::Basics &basics = slot->basics;

    // create a brand new store entry and initialize it with stored basics
    StoreEntry *e = new StoreEntry();
    e->lock_count = 0;
    e->swap_dirn = index;
    e->swap_filen = fileno;
    e->swap_file_sz = basics.swap_file_sz;
    e->lastref = basics.lastref;
    e->timestamp = basics.timestamp;
    e->expires = basics.expires;
    e->lastmod = basics.lastmod;
    e->refcount = basics.refcount;
    e->flags = basics.flags;
    e->store_status = STORE_OK;
    e->setMemStatus(NOT_IN_MEMORY);
    e->swap_status = SWAPOUT_DONE;
    e->ping_status = PING_NONE;
    EBIT_SET(e->flags, ENTRY_CACHABLE);
    EBIT_CLR(e->flags, RELEASE_REQUEST);
    EBIT_CLR(e->flags, KEY_PRIVATE);
    EBIT_SET(e->flags, ENTRY_VALIDATED);
    e->hashInsert(key);
    trackReferences(*e);

    return e;
    // the disk entry remains open for reading, protected from modifications
}

void Rock::SwapDir::disconnect(StoreEntry &e)
{
    assert(e.swap_dirn == index);
    assert(e.swap_filen >= 0);
    // cannot have SWAPOUT_NONE entry with swap_filen >= 0
    assert(e.swap_status != SWAPOUT_NONE);

    // do not rely on e.swap_status here because there is an async delay
    // before it switches from SWAPOUT_WRITING to SWAPOUT_DONE.

    // since e has swap_filen, its slot is locked for either reading or writing
    map->abortIo(e.swap_filen);
    e.swap_dirn = -1;
    e.swap_filen = -1;
    e.swap_status = SWAPOUT_NONE;
}

uint64_t
Rock::SwapDir::currentSize() const
{
    return HeaderSize + max_objsize * currentCount();
}

uint64_t
Rock::SwapDir::currentCount() const
{
    return map ? map->entryCount() : 0;
}

/// In SMP mode only the disker process reports stats to avoid
/// counting the same stats by multiple processes.
bool
Rock::SwapDir::doReportStat() const
{
    return ::SwapDir::doReportStat() && (!UsingSmp() || IamDiskProcess());
}

void
Rock::SwapDir::swappedOut(const StoreEntry &)
{
    // stats are not stored but computed when needed
}

int64_t
Rock::SwapDir::entryLimitAllowed() const
{
    const int64_t eLimitLo = map ? map->entryLimit() : 0; // dynamic shrinking unsupported
    const int64_t eWanted = (maximumSize() - HeaderSize)/maxObjectSize();
    return min(max(eLimitLo, eWanted), entryLimitHigh());
}

// TODO: encapsulate as a tool; identical to CossSwapDir::create()
void
Rock::SwapDir::create()
{
    assert(path);
    assert(filePath);

    if (UsingSmp() && !IamDiskProcess()) {
        debugs (47,3, HERE << "disker will create in " << path);
        return;
    }

    debugs (47,3, HERE << "creating in " << path);

    struct stat swap_sb;
    if (::stat(path, &swap_sb) < 0) {
        debugs (47, 1, "Creating Rock db directory: " << path);
#ifdef _SQUID_MSWIN_
        const int res = mkdir(path);
#else
        const int res = mkdir(path, 0700);
#endif
        if (res != 0) {
            debugs(47,0, "Failed to create Rock db dir " << path <<
                ": " << xstrerror());
            fatal("Rock Store db creation error");
		}
	}

#if SLOWLY_FILL_WITH_ZEROS
    /* TODO just set the file size */
    char block[1024]; // max_size is in KB so this is one unit of max_size
    memset(block, '\0', sizeof(block));

    const int swap = open(filePath, O_WRONLY|O_CREAT|O_TRUNC|O_BINARY, 0600);
    for (off_t offset = 0; offset < max_size; ++offset) {
        if (write(swap, block, sizeof(block)) != sizeof(block)) {
            debugs(47,0, "Failed to create Rock Store db in " << filePath <<
                ": " << xstrerror());
            fatal("Rock Store db creation error");
		}
	}
    close(swap);
#else
    const int swap = open(filePath, O_WRONLY|O_CREAT|O_TRUNC|O_BINARY, 0600);
    if (swap < 0) {
        debugs(47,0, "Failed to initialize Rock Store db in " << filePath <<
            "; create error: " << xstrerror());
        fatal("Rock Store db creation error");
    }

    if (ftruncate(swap, maximumSize()) != 0) {
        debugs(47,0, "Failed to initialize Rock Store db in " << filePath <<
            "; truncate error: " << xstrerror());
        fatal("Rock Store db creation error");
    }

    char header[HeaderSize];
    memset(header, '\0', sizeof(header));
    if (write(swap, header, sizeof(header)) != sizeof(header)) {
        debugs(47,0, "Failed to initialize Rock Store db in " << filePath <<
                "; write error: " << xstrerror());
        fatal("Rock Store db initialization error");
    }
    close(swap);
#endif

}

void
Rock::SwapDir::init()
{
    debugs(47,2, HERE);

    // XXX: SwapDirs aren't refcounted. We make IORequestor calls, which
    // are refcounted. We up our count once to avoid implicit delete's.
    RefCountReference();

    Must(!map);
    map = new DirMap(path);

    const char *ioModule = UsingSmp() ? "IpcIo" : "Blocking";
    if (DiskIOModule *m = DiskIOModule::Find(ioModule)) {
        debugs(47,2, HERE << "Using DiskIO module: " << ioModule);
        io = m->createStrategy();
        io->init();
    } else {
        debugs(47,1, "Rock store is missing DiskIO module: " << ioModule);
        fatal("Rock Store missing a required DiskIO module");
    }

    theFile = io->newFile(filePath);
    theFile->open(O_RDWR, 0644, this);

    // Increment early. Otherwise, if one SwapDir finishes rebuild before
    // others start, storeRebuildComplete() will think the rebuild is over!
    // TODO: move store_dirs_rebuilding hack to store modules that need it.
    ++StoreController::store_dirs_rebuilding;
}

bool
Rock::SwapDir::needsDiskStrand() const
{
    return true;
}

void
Rock::SwapDir::parse(int anIndex, char *aPath)
{
    index = anIndex;

    path = xstrdup(aPath);

    // cache store is located at path/db
    String fname(path);
    fname.append("/rock");
    filePath = xstrdup(fname.termedBuf());

    parseSize();
    parseOptions(0);

    // Current openForWriting() code overwrites the old slot if needed
    // and possible, so proactively removing old slots is probably useless.
    assert(!repl); // repl = createRemovalPolicy(Config.replPolicy);

    validateOptions();
}

void
Rock::SwapDir::reconfigure(int, char *)
{
    parseSize();
    parseOptions(1);
    // TODO: can we reconfigure the replacement policy (repl)?
    validateOptions();
}

/// parse maximum db disk size
void
Rock::SwapDir::parseSize()
{
    max_size = GetInteger() << 10; // MBytes to KBytes
    if (max_size < 0)
        fatal("negative Rock cache_dir size value");
}

/// check the results of the configuration; only level-0 debugging works here
void
Rock::SwapDir::validateOptions()
{
    if (max_objsize <= 0)
        fatal("Rock store requires a positive max-size");

    /* XXX: should we support resize?
    map->resize(entryLimitAllowed()); // the map may decide to use an even lower limit
    */

    /* XXX: misplaced, map is not yet created
    // Note: We could try to shrink max_size now. It is stored in KB so we
    // may not be able to make it match the end of the last entry exactly.
    const int64_t mapRoundWasteMx = max_objsize*sizeof(long)*8;
    const int64_t sizeRoundWasteMx = 1024; // max_size stored in KB
    const int64_t roundingWasteMx = max(mapRoundWasteMx, sizeRoundWasteMx);
    const int64_t totalWaste = maximumSize() - diskOffsetLimit();
    assert(diskOffsetLimit() <= maximumSize());

    // warn if maximum db size is not reachable due to sfileno limit
    if (map->entryLimit() == entryLimitHigh() && totalWaste > roundingWasteMx) {
        debugs(47, 0, "Rock store cache_dir[" << index << "]:");
        debugs(47, 0, "\tmaximum number of entries: " << map->entryLimit());
        debugs(47, 0, "\tmaximum entry size: " << max_objsize << " bytes");
        debugs(47, 0, "\tmaximum db size: " << maximumSize() << " bytes");
        debugs(47, 0, "\tusable db size:  " << diskOffsetLimit() << " bytes");
        debugs(47, 0, "\tdisk space waste: " << totalWaste << " bytes");
        debugs(47, 0, "WARNING: Rock store config wastes space.");
	}
    */
}

void
Rock::SwapDir::rebuild() {
    //++StoreController::store_dirs_rebuilding; // see Rock::SwapDir::init()
    AsyncJob::Start(new Rebuild(this));
}

/* Add a new object to the cache with empty memory copy and pointer to disk
 * use to rebuild store from disk. Based on UFSSwapDir::addDiskRestore */
bool
Rock::SwapDir::addEntry(const int fileno, const DbCellHeader &header, const StoreEntry &from)
{
    debugs(47, 8, HERE << &from << ' ' << from.getMD5Text() <<
       ", fileno="<< std::setfill('0') << std::hex << std::uppercase <<
       std::setw(8) << fileno);

    sfileno newLocation = 0;
    if (Ipc::StoreMapSlot *slot = map->openForWriting(reinterpret_cast<const cache_key *>(from.key), newLocation)) {
        if (fileno == newLocation) {
            slot->set(from);
            map->extras(fileno) = header;
        } // else some other, newer entry got into our cell
        map->closeForWriting(newLocation, false);
        return fileno == newLocation;
    }

    return false;
}


bool
Rock::SwapDir::canStore(const StoreEntry &e, int64_t diskSpaceNeeded, int &load) const
{
    if (!::SwapDir::canStore(e, sizeof(DbCellHeader)+diskSpaceNeeded, load))
        return false;

    if (!theFile || !theFile->canWrite())
        return false;

    if (!map)
        return false;

    if (io->shedLoad())
        return false;

    load = io->load();
    return true;
}

StoreIOState::Pointer
Rock::SwapDir::createStoreIO(StoreEntry &e, StoreIOState::STFNCB *cbFile, StoreIOState::STIOCB *cbIo, void *data)
{
    if (!theFile || theFile->error()) {
        debugs(47,4, HERE << theFile);
        return NULL;
    }

    // compute payload size for our cell header, using StoreEntry info
    // careful: e.objectLen() may still be negative here
    const int64_t expectedReplySize = e.mem_obj->expectedReplySize();
    assert(expectedReplySize >= 0); // must know to prevent cell overflows
    assert(e.mem_obj->swap_hdr_sz > 0);
    DbCellHeader header;
    header.payloadSize = e.mem_obj->swap_hdr_sz + expectedReplySize;
    const int64_t payloadEnd = sizeof(DbCellHeader) + header.payloadSize;
    assert(payloadEnd <= max_objsize);

    sfileno fileno;
    Ipc::StoreMapSlot *const slot =
        map->openForWriting(reinterpret_cast<const cache_key *>(e.key), fileno);
    if (!slot) {
        debugs(47, 5, HERE << "Rock::SwapDir::createStoreIO: map->add failed");
        return NULL;
    }
    e.swap_file_sz = header.payloadSize; // and will be copied to the map
    slot->set(e);
    map->extras(fileno) = header;

    // XXX: We rely on our caller, storeSwapOutStart(), to set e.fileno.
    // If that does not happen, the entry will not decrement the read level!

    IoState *sio = new IoState(this, &e, cbFile, cbIo, data);

    sio->swap_dirn = index;
    sio->swap_filen = fileno;
    sio->payloadEnd = payloadEnd;
    sio->diskOffset = diskOffset(sio->swap_filen);

    debugs(47,5, HERE << "dir " << index << " created new fileno " <<
        std::setfill('0') << std::hex << std::uppercase << std::setw(8) <<
        sio->swap_filen << std::dec << " at " << sio->diskOffset);

    assert(sio->diskOffset + payloadEnd <= diskOffsetLimit());

    sio->file(theFile);

    trackReferences(e);
    return sio;
}

int64_t
Rock::SwapDir::diskOffset(int filen) const
{
    assert(filen >= 0);
    return HeaderSize + max_objsize*filen;
}

int64_t
Rock::SwapDir::diskOffsetLimit() const
{
    assert(map);
    return diskOffset(map->entryLimit());
}

// tries to open an old or being-written-to entry with swap_filen for reading
StoreIOState::Pointer
Rock::SwapDir::openStoreIO(StoreEntry &e, StoreIOState::STFNCB *cbFile, StoreIOState::STIOCB *cbIo, void *data)
{
    if (!theFile || theFile->error()) {
        debugs(47,4, HERE << theFile);
        return NULL;
    }

    if (e.swap_filen < 0) { 
        debugs(47,4, HERE << e);
        return NULL;
    }

    // The are two ways an entry can get swap_filen: our get() locked it for
    // reading or our storeSwapOutStart() locked it for writing. Peeking at our
    // locked entry is safe, but no support for reading a filling entry.
    const Ipc::StoreMapSlot *slot = map->peekAtReader(e.swap_filen);
    if (!slot)
        return NULL; // we were writing afterall

    IoState *sio = new IoState(this, &e, cbFile, cbIo, data);

    sio->swap_dirn = index;
    sio->swap_filen = e.swap_filen;
    sio->payloadEnd = sizeof(DbCellHeader) + map->extras(e.swap_filen).payloadSize;
    assert(sio->payloadEnd <= max_objsize); // the payload fits the slot

    debugs(47,5, HERE << "dir " << index << " has old fileno: " <<
        std::setfill('0') << std::hex << std::uppercase << std::setw(8) <<
        sio->swap_filen);

    assert(slot->basics.swap_file_sz > 0);
    assert(slot->basics.swap_file_sz == e.swap_file_sz);

    sio->diskOffset = diskOffset(sio->swap_filen);
    assert(sio->diskOffset + sio->payloadEnd <= diskOffsetLimit());

    sio->file(theFile);
    return sio;
}

void
Rock::SwapDir::ioCompletedNotification()
{
    if (!theFile) {
        debugs(47, 1, HERE << filePath << ": initialization failure or " <<
            "premature close of rock db file");
        fatalf("Rock cache_dir failed to initialize db file: %s", filePath);
    }

    if (theFile->error()) {
        debugs(47, 1, HERE << filePath << ": " << xstrerror());
        fatalf("Rock cache_dir failed to open db file: %s", filePath);
	}

    // TODO: lower debugging level
    debugs(47,1, "Rock cache_dir[" << index << "] limits: " << 
        std::setw(12) << maximumSize() << " disk bytes and " <<
        std::setw(7) << map->entryLimit() << " entries");

    rebuild();
}

void
Rock::SwapDir::closeCompleted()
{
    theFile = NULL;
}

void
Rock::SwapDir::readCompleted(const char *buf, int rlen, int errflag, RefCount< ::ReadRequest> r)
{
    ReadRequest *request = dynamic_cast<Rock::ReadRequest*>(r.getRaw());
    assert(request);
    IoState::Pointer sio = request->sio;

    if (errflag == DISK_OK && rlen > 0)
        sio->offset_ += rlen;
    assert(sio->diskOffset + sio->offset_ <= diskOffsetLimit()); // post-factum

    StoreIOState::STRCB *callback = sio->read.callback;
    assert(callback);
    sio->read.callback = NULL;
    void *cbdata;
    if (cbdataReferenceValidDone(sio->read.callback_data, &cbdata))
        callback(cbdata, r->buf, rlen, sio.getRaw());
}

void
Rock::SwapDir::writeCompleted(int errflag, size_t rlen, RefCount< ::WriteRequest> r)
{
    Rock::WriteRequest *request = dynamic_cast<Rock::WriteRequest*>(r.getRaw());
    assert(request);
    assert(request->sio !=  NULL);
    IoState &sio = *request->sio;

    if (errflag == DISK_OK) {
        // close, assuming we only write once; the entry gets the read lock
        map->closeForWriting(sio.swap_filen, true);
        // do not increment sio.offset_ because we do it in sio->write()
    } else {
        // Do not abortWriting here. The entry should keep the write lock
        // instead of losing association with the store and confusing core.
        map->free(sio.swap_filen); // will mark as unusable, just in case
    }

    assert(sio.diskOffset + sio.offset_ <= diskOffsetLimit()); // post-factum

    sio.finishedWriting(errflag);
}

bool
Rock::SwapDir::full() const
{
    return map && map->full();
}

// storeSwapOutFileClosed calls this nethod on DISK_NO_SPACE_LEFT,
// but it should not happen for us
void
Rock::SwapDir::diskFull() {
    debugs(20,1, "Internal ERROR: No space left error with rock cache_dir: " <<
        filePath);
}

/// purge while full(); it should be sufficient to purge just one
void
Rock::SwapDir::maintain()
{
    debugs(47,3, HERE << "cache_dir[" << index << "] guards: " << 
        !repl << !map << !full() << StoreController::store_dirs_rebuilding);

    if (!repl)
        return; // no means (cannot find a victim)

    if (!map)
        return; // no victims (yet)

    if (!full())
        return; // no need (to find a victim)

    // XXX: UFSSwapDir::maintain says we must quit during rebuild
    if (StoreController::store_dirs_rebuilding)
        return;

    debugs(47,3, HERE << "cache_dir[" << index << "] state: " << map->full() <<
           ' ' << currentSize() << " < " << diskOffsetLimit());

    // Hopefully, we find a removable entry much sooner (TODO: use time?)
    const int maxProbed = 10000;
    RemovalPurgeWalker *walker = repl->PurgeInit(repl, maxProbed);

    // It really should not take that long, but this will stop "infinite" loops
    const int maxFreed = 1000;
    int freed = 0;
    // TODO: should we purge more than needed to minimize overheads?
    for (; freed < maxFreed && full(); ++freed) {
        if (StoreEntry *e = walker->Next(walker))
            e->release(); // will call our unlink() method
		else
            break; // no more objects
	}

    debugs(47,2, HERE << "Rock cache_dir[" << index << "] freed " << freed <<
        " scanned " << walker->scanned << '/' << walker->locked);

    walker->Done(walker);

    if (full()) {
        debugs(47,0, "ERROR: Rock cache_dir[" << index << "] " <<
            "is still full after freeing " << freed << " entries. A bug?");
	}
}

void
Rock::SwapDir::reference(StoreEntry &e)
{
    debugs(47, 5, HERE << &e << ' ' << e.swap_dirn << ' ' << e.swap_filen);
    if (repl && repl->Referenced)
        repl->Referenced(repl, &e, &e.repl);
}

void
Rock::SwapDir::dereference(StoreEntry &e)
{
    debugs(47, 5, HERE << &e << ' ' << e.swap_dirn << ' ' << e.swap_filen);
    if (repl && repl->Dereferenced)
        repl->Dereferenced(repl, &e, &e.repl);
}

void
Rock::SwapDir::unlink(StoreEntry &e)
{
    debugs(47, 5, HERE << e);
    ignoreReferences(e);
    map->free(e.swap_filen);
    disconnect(e);
}

void
Rock::SwapDir::trackReferences(StoreEntry &e)
{
    debugs(47, 5, HERE << e);
    if (repl)
        repl->Add(repl, &e, &e.repl);
}


void
Rock::SwapDir::ignoreReferences(StoreEntry &e)
{
    debugs(47, 5, HERE << e);
    if (repl)
        repl->Remove(repl, &e, &e.repl);
}

void
Rock::SwapDir::statfs(StoreEntry &e) const
{
    const double currentSizeInKB = currentSize() / 1024.0;
    storeAppendPrintf(&e, "\n");
    storeAppendPrintf(&e, "Maximum Size: %"PRIu64" KB\n", max_size);
    storeAppendPrintf(&e, "Current Size: %.2f KB %.2f%%\n",
                      currentSizeInKB,
                      Math::doublePercent(currentSizeInKB, max_size));

    if (map) {
        const int limit = map->entryLimit();
        storeAppendPrintf(&e, "Maximum entries: %9d\n", limit);
        if (limit > 0) {
            const int entryCount = map->entryCount();
            storeAppendPrintf(&e, "Current entries: %9d %.2f%%\n",
                entryCount, (100.0 * entryCount / limit));

            if (limit < 100) { // XXX: otherwise too expensive to count
                Ipc::ReadWriteLockStats stats;
                map->updateStats(stats);
                stats.dump(e);
            }
        }
    }    

    storeAppendPrintf(&e, "Pending operations: %d out of %d\n",
        store_open_disk_fd, Config.max_open_disk_fds);

    storeAppendPrintf(&e, "Flags:");

    if (flags.selected)
        storeAppendPrintf(&e, " SELECTED");

    if (flags.read_only)
        storeAppendPrintf(&e, " READ-ONLY");

    storeAppendPrintf(&e, "\n");

}


/// initializes shared memory segments used by Rock::SwapDir
class RockSwapDirRr: public RegisteredRunner
{
public:
    /* RegisteredRunner API */
    virtual void run(const RunnerRegistry &);
    virtual ~RockSwapDirRr();

private:
    Vector<Rock::SwapDir::DirMap::Owner *> owners;
};

RunnerRegistrationEntry(rrAfterConfig, RockSwapDirRr);


void RockSwapDirRr::run(const RunnerRegistry &)
{
    if (IamMasterProcess()) {
        Must(owners.empty());
        for (int i = 0; i < Config.cacheSwap.n_configured; ++i) {
            if (const Rock::SwapDir *const sd = dynamic_cast<Rock::SwapDir *>(INDEXSD(i))) {
                Rock::SwapDir::DirMap::Owner *const owner = Rock::SwapDir::DirMap::Init(sd->path, sd->entryLimitAllowed());
                owners.push_back(owner);
            }
        }
    }
}

RockSwapDirRr::~RockSwapDirRr()
{
    for (size_t i = 0; i < owners.size(); ++i)
        delete owners[i];
}
