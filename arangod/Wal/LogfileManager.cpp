////////////////////////////////////////////////////////////////////////////////
/// @brief Write-ahead log logfile manager
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2004-2013 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Jan Steemann
/// @author Copyright 2011-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "LogfileManager.h"
#include "BasicsC/hashes.h"
#include "BasicsC/json.h"
#include "BasicsC/logging.h"
#include "Basics/Exceptions.h"
#include "Basics/FileUtils.h"
#include "Basics/JsonHelper.h"
#include "Basics/MutexLocker.h"
#include "Basics/ReadLocker.h"
#include "Basics/StringUtils.h"
#include "Basics/WriteLocker.h"
#include "VocBase/server.h"
#include "Wal/AllocatorThread.h"
#include "Wal/CollectorThread.h"
#include "Wal/Slots.h"
#include "Wal/SynchroniserThread.h"

using namespace triagens::wal;

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief create the logfile manager
////////////////////////////////////////////////////////////////////////////////

LogfileManager::LogfileManager ()
  : ApplicationFeature("logfile-manager"),
    _directory(),
    _filesize(32 * 1024 * 1024),
    _reserveLogfiles(3),
    _historicLogfiles(10),
    _slots(nullptr),
    _synchroniserThread(nullptr),
    _allocatorThread(nullptr),
    _collectorThread(nullptr),
    _logfilesLock(),
    _lastCollectedId(0),
    _logfiles(),
    _regex(),
    _shutdown(0) {
  
  LOG_INFO("creating wal logfile manager");

  int res = regcomp(&_regex, "^logfile-([0-9][0-9]*)\\.db$", REG_EXTENDED);

  if (res != 0) {
    THROW_INTERNAL_ERROR("could not compile regex"); 
  }

  _slots = new Slots(this, 1048576, 0);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy the logfile manager
////////////////////////////////////////////////////////////////////////////////

LogfileManager::~LogfileManager () {
  LOG_INFO("shutting down wal logfile manager");

  regfree(&_regex);

  if (_slots != nullptr) {
    delete _slots;
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                        ApplicationFeature methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::setupOptions (std::map<std::string, triagens::basics::ProgramOptionsDescription>& options) {
  options["Write-ahead log options:help-wal"]
    ("wal.logfile-size", &_filesize, "size of each logfile")
    ("wal.historic-logfiles", &_historicLogfiles, "number of historic logfiles to keep after collection")
    ("wal.reserve-logfiles", &_reserveLogfiles, "number of reserve logfiles to maintain")
    ("wal.directory", &_directory, "logfile directory")
  ;
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

bool LogfileManager::prepare () {
  if (_directory.empty()) {
    LOG_FATAL_AND_EXIT("no directory specified for write-ahead logs. Please use the --wal.directory option");
  }

  if (_directory[_directory.size() - 1] != TRI_DIR_SEPARATOR_CHAR) {
    // append a trailing slash to directory name
    _directory.push_back(TRI_DIR_SEPARATOR_CHAR);
  }

  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

bool LogfileManager::start () {
  int res = inventory();

  if (res != TRI_ERROR_NO_ERROR) {
    LOG_ERROR("could not create wal logfile inventory: %s", TRI_errno_string(res));
    return false;
  }
 
  std::string const shutdownFile = shutdownFilename();
  bool const shutdownFileExists = basics::FileUtils::exists(shutdownFile); 

  if (shutdownFileExists) {
    res = readShutdownInfo();
  
    if (res != TRI_ERROR_NO_ERROR) {
      LOG_ERROR("could not open shutdown file '%s': %s", 
                shutdownFile.c_str(), 
                TRI_errno_string(res));
      return false;
    }

    LOG_INFO("last tick: %llu, last collected: %llu", 
             (unsigned long long) _slots->lastAssignedTick(),
             (unsigned long long) _lastCollectedId);
  }

  res = openLogfiles();
  
  if (res != TRI_ERROR_NO_ERROR) {
    LOG_ERROR("could not open wal logfiles: %s", 
              TRI_errno_string(res));
    return false;
  }

  res = startSynchroniserThread();

  if (res != TRI_ERROR_NO_ERROR) {
    LOG_ERROR("could not start wal synchroniser thread: %s", TRI_errno_string(res));
    return false;
  }

  res = startAllocatorThread();

  if (res != TRI_ERROR_NO_ERROR) {
    LOG_ERROR("could not start wal allocator thread: %s", TRI_errno_string(res));
    return false;
  }
  
  res = startCollectorThread();

  if (res != TRI_ERROR_NO_ERROR) {
    LOG_ERROR("could not start wal collector thread: %s", TRI_errno_string(res));
    return false;
  }

  if (shutdownFileExists) {
    // delete the shutdown file if it existed
    if (! basics::FileUtils::remove(shutdownFile, &res)) {
      LOG_ERROR("could not remove shutdown file '%s': %s", shutdownFile.c_str(), TRI_errno_string(res));
      return false;
    }
  }

  LOG_INFO("wal logfile manager configuration: historic logfiles: %lu, reserve logfiles: %lu, filesize: %lu",
           (unsigned long) _historicLogfiles,
           (unsigned long) _reserveLogfiles,
           (unsigned long) _filesize);

  return true;
}
  
////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

bool LogfileManager::open () {
  for (size_t i = 0; i < 50 * 1024 * 1024; ++i) {
    void* p = TRI_Allocate(TRI_UNKNOWN_MEM_ZONE, 64, true);
    TRI_df_marker_t* marker = static_cast<TRI_df_marker_t*>(p);

    marker->_type = TRI_DF_MARKER_HEADER;
    marker->_size = 64;
    marker->_crc  = 0;
    marker->_tick = 0;

if (i % 500000 == 0) {
  LOG_INFO("now at: %d", (int) i);
}
    memcpy(static_cast<char*>(p) + sizeof(TRI_df_marker_t), "the fox is brown\0", strlen("the fox is brown") + 1);
    this->allocateAndWrite(p, static_cast<uint32_t>(64), false);

    TRI_Free(TRI_UNKNOWN_MEM_ZONE, p);
  }

  LOG_INFO("done");

  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::close () {
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::stop () {
  if (_shutdown > 0) {
    return;
  }

  _shutdown = 1;

  LOG_INFO("stopping collector thread");
  // stop threads
  stopCollectorThread();
  
  LOG_INFO("stopping allocator thread");
  stopAllocatorThread();
  
  LOG_INFO("stopping synchroniser thread");
  stopSynchroniserThread();
  
  LOG_INFO("closing logfiles");
  sleep(1);

  // close all open logfiles
  closeLogfiles();

  int res = writeShutdownInfo();

  if (res != TRI_ERROR_NO_ERROR) {
    LOG_ERROR("could not write wal shutdown info: %s", TRI_errno_string(res));
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not there are reserve logfiles
////////////////////////////////////////////////////////////////////////////////

bool LogfileManager::hasReserveLogfiles () {
  uint32_t numberOfLogfiles = 0;

  // note: this information could also be cached instead of being recalculated
  // everytime
  READ_LOCKER(_logfilesLock);
     
  for (auto it = _logfiles.begin(); it != _logfiles.end(); ++it) {
    Logfile* logfile = (*it).second;

    if (logfile != nullptr && logfile->freeSize() > 0 && ! logfile->isSealed()) {
      if (++numberOfLogfiles >= reserveLogfiles()) {
        return true;
      }
    }
  }

  return false;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief signal that a sync operation is required
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::signalSync () {
  _synchroniserThread->signalSync();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief seal logfiles that require sealing
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::sealLogfiles () {
  std::vector<Logfile*> logfiles;

  // create a copy of all logfiles that can be sealed
  {
    READ_LOCKER(_logfilesLock);
    for (auto it = _logfiles.begin(); it != _logfiles.end(); ++it) {
      Logfile* logfile = (*it).second;

      if (logfile != nullptr && logfile->canBeSealed()) {
        logfiles.push_back(logfile);
      }
    }
  }

  // now seal them
  for (auto it = logfiles.begin(); it != logfiles.end(); ++it) {
    // remove the logfile from the list of logfiles temporarily
    // this is required so any concurrent operations on the logfile are not
    // affect
    Logfile* logfile = (*it);
    unlinkLogfile(logfile);

    // TODO: handle return value
    logfile->seal();

    relinkLogfile(logfile);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief allocate space in a logfile for later writing
////////////////////////////////////////////////////////////////////////////////

SlotInfo LogfileManager::allocate (uint32_t size) {
  if (size > maxEntrySize()) {
    // entry is too big
    return SlotInfo(TRI_ERROR_ARANGO_DOCUMENT_TOO_LARGE);
  }

  return _slots->nextUnused(size);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief finalise a log entry
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::finalise (SlotInfo& slotInfo,
                               bool waitForSync) {
  _slots->returnUsed(slotInfo, waitForSync);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief write data into the logfile
/// this is a convenience function that combines allocate, memcpy and finalise
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::allocateAndWrite (void* mem,
                                      uint32_t size,
                                      bool waitForSync) {

  SlotInfo slotInfo = allocate(size);
 
  if (slotInfo.errorCode != TRI_ERROR_NO_ERROR) {
    return slotInfo.errorCode;
  }

  assert(slotInfo.slot != nullptr);
  
  TRI_df_marker_t* marker = static_cast<TRI_df_marker_t*>(slotInfo.slot->mem());

  // write tick into marker
  marker->_tick = slotInfo.slot->tick();

  // set initial crc to 0
  marker->_crc = 0;
  
  // now calculate crc  
  TRI_voc_crc_t crc = TRI_InitialCrc32();
  crc = TRI_BlockCrc32(crc, static_cast<char const*>(mem), static_cast<TRI_voc_size_t>(size));
  marker->_crc = TRI_FinalCrc32(crc);

  memcpy(slotInfo.slot->mem(), mem, static_cast<TRI_voc_size_t>(size));

  finalise(slotInfo, waitForSync);
  return slotInfo.errorCode;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief re-inserts a logfile back into the inventory only
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::relinkLogfile (Logfile* logfile) {
  Logfile::IdType const id = logfile->id();

  WRITE_LOCKER(_logfilesLock);
  _logfiles.insert(make_pair(id, logfile));
}

////////////////////////////////////////////////////////////////////////////////
/// @brief remove a logfile from the inventory only
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::unlinkLogfile (Logfile* logfile) {
  Logfile::IdType const id = logfile->id();

  {
    WRITE_LOCKER(_logfilesLock);
    auto it = _logfiles.find(id);

    if (it == _logfiles.end()) {
      return;
    }

    _logfiles.erase(it);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief remove a logfile from the inventory and in the file system
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::removeLogfile (Logfile* logfile) {
  unlinkLogfile(logfile);
    
  // old filename
  Logfile::IdType const id = logfile->id();
  std::string const filename = logfileName(id);
  
  LOG_INFO("removing logfile '%s'", filename.c_str());
  
  // now close the logfile
  delete logfile;
  
  int res = TRI_ERROR_NO_ERROR;
  // now physically remove the file
  basics::FileUtils::remove(filename, &res);

  if (res != TRI_ERROR_NO_ERROR) {
    LOG_ERROR("unable to remove logfile '%s': %s", filename.c_str(), TRI_errno_string(res));
    return;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief request sealing of a logfile
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::requestSealing (Logfile* logfile) {
  assert(logfile != nullptr);

  {
    WRITE_LOCKER(_logfilesLock);
    logfile->setStatus(Logfile::StatusType::SEAL_REQUESTED);
    signalSync();
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the file descriptor of a logfile
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::getLogfileDescriptor (Logfile::IdType id) {
  READ_LOCKER(_logfilesLock);
  auto it = _logfiles.find(id);

  if (it == _logfiles.end()) {
    // error
    return -1;
  }

  Logfile* logfile = (*it).second;
  assert(logfile != nullptr);

  return logfile->fd();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get a logfile for writing. this may return nullptr
////////////////////////////////////////////////////////////////////////////////

Logfile* LogfileManager::getWriteableLogfile (uint32_t size) {
  size_t iterations = 0;

  while (++iterations < 1000) {
    {
      WRITE_LOCKER(_logfilesLock);

      for (auto it = _logfiles.begin(); it != _logfiles.end(); ++it) {
        Logfile* logfile = (*it).second;

        if (logfile != nullptr && logfile->isWriteable(size)) {
          return logfile;
        }
      }
    }

    // signal & sleep outside the lock
    _allocatorThread->signalLogfileCreation();
    usleep(10000);
  }
  
  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get a logfile to collect. this may return nullptr
////////////////////////////////////////////////////////////////////////////////

Logfile* LogfileManager::getCollectableLogfile () {
  READ_LOCKER(_logfilesLock);

  for (auto it = _logfiles.begin(); it != _logfiles.end(); ++it) {
    Logfile* logfile = (*it).second;

    if (logfile != nullptr && logfile->canBeCollected()) {
      return logfile;
    }
  }

  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get a logfile to remove. this may return nullptr
////////////////////////////////////////////////////////////////////////////////

Logfile* LogfileManager::getRemovableLogfile () {
  uint32_t numberOfLogfiles = 0;
  Logfile* first = nullptr;

  READ_LOCKER(_logfilesLock);

  for (auto it = _logfiles.begin(); it != _logfiles.end(); ++it) {
    Logfile* logfile = (*it).second;

    if (logfile != nullptr && logfile->canBeRemoved()) {
      if (first == nullptr) {
        first = logfile;
      }

      if (++numberOfLogfiles > historicLogfiles()) { 
        assert(first != nullptr);
        return first;
      }
    }
  }

  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief mark a file as being requested for collection
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::setCollectionRequested (Logfile* logfile) {
  assert(logfile != nullptr);

  WRITE_LOCKER(_logfilesLock);
  logfile->setStatus(Logfile::StatusType::COLLECTION_REQUESTED);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief mark a file as being done with collection
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::setCollectionDone (Logfile* logfile) {
  assert(logfile != nullptr);

  WRITE_LOCKER(_logfilesLock);
  logfile->setStatus(Logfile::StatusType::COLLECTED);
}

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief closes all logfiles
////////////////////////////////////////////////////////////////////////////////
  
void LogfileManager::closeLogfiles () {
  WRITE_LOCKER(_logfilesLock);

  for (auto it = _logfiles.begin(); it != _logfiles.end(); ++it) {
    Logfile* logfile = (*it).second;

    if (logfile != nullptr) {
      delete logfile;
    }
  }

  _logfiles.clear();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the id of the last fully collected logfile
/// returns 0 if no logfile was yet collected or no information about the
/// collection is present
////////////////////////////////////////////////////////////////////////////////

Logfile::IdType LogfileManager::lastCollected () {
  READ_LOCKER(_logfilesLock);
  return _lastCollectedId;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief reads the shutdown information
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::readShutdownInfo () {
  std::string const filename = shutdownFilename();

  TRI_json_t* json = TRI_JsonFile(TRI_UNKNOWN_MEM_ZONE, filename.c_str(), nullptr); 

  if (json == nullptr) {
    return TRI_ERROR_INTERNAL;
  }

  // read last assigned tick (may be 0)
  uint64_t const lastTick = basics::JsonHelper::stringUInt64(json, "lastTick");
  _slots->setLastAssignedTick(static_cast<Slot::TickType>(lastTick));
  
  // read if of last collected logfile (maybe 0)
  uint64_t const lastCollected = basics::JsonHelper::stringUInt64(json, "lastCollected");
  
  {
    WRITE_LOCKER(_logfilesLock);
    _lastCollectedId = static_cast<Logfile::IdType>(lastCollected);
  }
  
  TRI_FreeJson(TRI_UNKNOWN_MEM_ZONE, json);
  
  return TRI_ERROR_NO_ERROR; 
}

////////////////////////////////////////////////////////////////////////////////
/// @brief writes the shutdown information
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::writeShutdownInfo () {
  std::string const filename = shutdownFilename();

  std::string content;
  content.append("{\"lastTick\":\"");
  content.append(basics::StringUtils::itoa(_slots->lastAssignedTick()));
  content.append("\",\"lastCollected\":\"");
  content.append(basics::StringUtils::itoa(lastCollected()));
  content.append("\"}");

  // TODO: spit() doesn't return success/failure. FIXME!
  basics::FileUtils::spit(filename, content);
  
  return TRI_ERROR_NO_ERROR; 
}

////////////////////////////////////////////////////////////////////////////////
/// @brief start the synchroniser thread
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::startSynchroniserThread () {
  _synchroniserThread = new SynchroniserThread(this);

  if (_synchroniserThread == nullptr) {
    return TRI_ERROR_INTERNAL;
  }

  if (! _synchroniserThread->start()) {
    delete _synchroniserThread;
    return TRI_ERROR_INTERNAL;
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief stop the synchroniser thread
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::stopSynchroniserThread () {
  if (_synchroniserThread != nullptr) {
    LOG_TRACE("stopping wal synchroniser thread");

    _synchroniserThread->stop();
    _synchroniserThread->shutdown();

    delete _synchroniserThread;
    _synchroniserThread = 0;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief start the allocator thread
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::startAllocatorThread () {
  _allocatorThread = new AllocatorThread(this);

  if (_allocatorThread == nullptr) {
    return TRI_ERROR_INTERNAL;
  }

  if (! _allocatorThread->start()) {
    delete _allocatorThread;
    return TRI_ERROR_INTERNAL;
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief stop the allocator thread
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::stopAllocatorThread () {
  if (_allocatorThread != nullptr) {
    LOG_TRACE("stopping wal allocator thread");

    _allocatorThread->stop();
    _allocatorThread->shutdown();

    delete _allocatorThread;
    _allocatorThread = 0;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief start the collector thread
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::startCollectorThread () {
  _collectorThread = new CollectorThread(this);

  if (_collectorThread == nullptr) {
    return TRI_ERROR_INTERNAL;
  }

  if (! _collectorThread->start()) {
    delete _collectorThread;
    return TRI_ERROR_INTERNAL;
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief stop the collector thread
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::stopCollectorThread () {
  if (_collectorThread != nullptr) {
    LOG_TRACE("stopping wal collector thread");

    _collectorThread->stop();
    _collectorThread->shutdown();

    delete _collectorThread;
    _collectorThread = 0;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief check which logfiles are present in the log directory
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::inventory () {
  int res = ensureDirectory();

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  LOG_TRACE("scanning wal directory: '%s'", _directory.c_str());

  std::vector<std::string> files = basics::FileUtils::listFiles(_directory);

  for (auto it = files.begin(); it != files.end(); ++it) {
    regmatch_t matches[2];
    std::string const file = (*it);
    char const* s = file.c_str();

    if (regexec(&_regex, s, sizeof(matches) / sizeof(matches[1]), matches, 0) == 0) {
      Logfile::IdType const id = basics::StringUtils::uint64(s + matches[1].rm_so, matches[1].rm_eo - matches[1].rm_so);

      if (id == 0) {
        LOG_WARNING("encountered invalid id for logfile '%s'. ids must be > 0", file.c_str());
      }
      else {
        // update global tick
        TRI_UpdateTickServer(static_cast<TRI_voc_tick_t>(id));

        WRITE_LOCKER(_logfilesLock);
        _logfiles.insert(make_pair(id, nullptr));
      }
    }
  }
     
  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief scan the logfiles in the log directory
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::openLogfiles () {
  WRITE_LOCKER(_logfilesLock);

  for (auto it = _logfiles.begin(); it != _logfiles.end(); ) {
    Logfile::IdType const id = (*it).first;
    std::string const filename = logfileName(id);

    assert((*it).second == nullptr);

    Logfile* logfile = Logfile::open(filename, id);

    if (logfile == nullptr) {
      _logfiles.erase(it++);
    }
    else {
      (*it).second = logfile;
      ++it;
    }
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief allocates a new reserve logfile
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::createReserveLogfile () {
  Logfile::IdType const id = nextId();
  std::string const filename = logfileName(id);

  LOG_INFO("creating empty logfile '%s'", filename.c_str());
  Logfile* logfile = Logfile::create(filename.c_str(), id, filesize());

  if (logfile == nullptr) {
    int res = TRI_errno();

    LOG_ERROR("unable to create logfile: %s", TRI_errno_string(res));
    return res;
  }
               
  WRITE_LOCKER(_logfilesLock);
  _logfiles.insert(make_pair(id, logfile));

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get an id for the next logfile
////////////////////////////////////////////////////////////////////////////////
        
Logfile::IdType LogfileManager::nextId () {
  return static_cast<Logfile::IdType>(TRI_NewTickServer());
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ensure the wal logfiles directory is actually there
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::ensureDirectory () {
  if (! basics::FileUtils::isDirectory(_directory)) {
    int res;
    
    LOG_INFO("wal directory '%s' does not exist. creating it...", _directory.c_str());

    if (! basics::FileUtils::createDirectory(_directory, &res)) {
      LOG_ERROR("could not create wal directory: '%s': %s", _directory.c_str(), TRI_errno_string(res));
      return res;
    }
  }

  if (! basics::FileUtils::isDirectory(_directory)) {
    LOG_ERROR("wal directory '%s' does not exist", _directory.c_str());
    return TRI_ERROR_FILE_NOT_FOUND;
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the absolute name of the shutdown file
////////////////////////////////////////////////////////////////////////////////

std::string LogfileManager::shutdownFilename () const {
  return _directory + std::string("SHUTDOWN");
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return an absolute filename for a logfile id
////////////////////////////////////////////////////////////////////////////////

std::string LogfileManager::logfileName (Logfile::IdType id) const {
  return _directory + std::string("logfile-") + basics::StringUtils::itoa(id) + std::string(".db");
}

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
