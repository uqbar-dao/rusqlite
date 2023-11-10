// 2010 April 7
//
// The author disclaims copyright to this source code.  In place of
// a legal notice, here is a blessing:
//
//    May you do good and not evil.
//    May you find forgiveness for yourself and forgive others.
//    May you share freely, never taking more than you give.
//
//***********************************************************************
//
// This file implements an example of a simple VFS implementation that
// omits complex features often not required or not possible on embedded
// platforms.  Code is included to buffer writes to the journal file,
// which can be a significant performance improvement on some embedded
// platforms.
//
// OVERVIEW
//
//   The code in this file implements a minimal SQLite VFS that can be
//   used on Linux and other posix-like operating systems. The following
//   system calls are used:
//
//    File-system: access(), unlink(), getcwd()
//    File IO:     open(), read(), write(), fsync(), close(), fstat()
//    Other:       sleep(), usleep(), time()
//
//   The following VFS features are omitted:
//
//     1. File locking. The user must ensure that there is at most one
//        connection to each database when using this VFS. Multiple
//        connections to a single shared-cache count as a single connection
//        for the purposes of the previous statement.
//
//     2. The loading of dynamic extensions (shared libraries).
//
//     3. Temporary files. The user must configure SQLite to use in-memory
//        temp files when using this VFS. The easiest way to do this is to
//        compile with:
//
//          -DSQLITE_TEMP_STORE=3
//
//     4. File truncation. As of version 3.6.24, SQLite may run without
//        a working xTruncate() call, providing the user does not configure
//        SQLite to use "journal_mode=truncate", or use both
//        "journal_mode=persist" and ATTACHed databases.
//
//   It is assumed that the system uses UNIX-like path-names. Specifically,
//   that '/' characters are used to separate path components and that
//   a path-name is a relative path unless it begins with a '/'. And that
//   no UTF-8 encoded paths are greater than 512 bytes in length.
//
// JOURNAL WRITE-BUFFERING
//
//   To commit a transaction to the database, SQLite first writes rollback
//   information into the journal file. This usually consists of 4 steps:
//
//     1. The rollback information is sequentially written into the journal
//        file, starting at the start of the file.
//     2. The journal file is synced to disk.
//     3. A modification is made to the first few bytes of the journal file.
//     4. The journal file is synced to disk again.
//
//   Most of the data is written in step 1 using a series of calls to the
//   VFS xWrite() method. The buffers passed to the xWrite() calls are of
//   various sizes. For example, as of version 3.6.24, when committing a
//   transaction that modifies 3 pages of a database file that uses 4096
//   byte pages residing on a media with 512 byte sectors, SQLite makes
//   eleven calls to the xWrite() method to create the rollback journal,
//   as follows:
//
//             Write offset | Bytes written
//             ----------------------------
//                        0            512
//                      512              4
//                      516           4096
//                     4612              4
//                     4616              4
//                     4620           4096
//                     8716              4
//                     8720              4
//                     8724           4096
//                    12820              4
//             ++++++++++++SYNC+++++++++++
//                        0             12
//             ++++++++++++SYNC+++++++++++
//
//   On many operating systems, this is an efficient way to write to a file.
//   However, on some embedded systems that do not cache writes in OS
//   buffers it is much more efficient to write data in blocks that are
//   an integer multiple of the sector-size in size and aligned at the
//   start of a sector.
//
//   To work around this, the code in this file allocates a fixed size
//   buffer of SQLITE_DEMOVFS_BUFFERSZ using sqlite3_malloc() whenever a
//   journal file is opened. It uses the buffer to coalesce sequential
//   writes into aligned SQLITE_DEMOVFS_BUFFERSZ blocks. When SQLite
//   invokes the xSync() method to sync the contents of the file to disk,
//   all accumulated data is written out, even if it does not constitute
//   a complete block. This means the actual IO to create the rollback
//   journal for the example transaction above is this:
//
//             Write offset | Bytes written
//             ----------------------------
//                        0           8192
//                     8192           4632
//             ++++++++++++SYNC+++++++++++
//                        0             12
//             ++++++++++++SYNC+++++++++++
//
//   Much more efficient if the underlying OS is not caching write
//   operations.

#if !defined(SQLITE_TEST) || SQLITE_OS_UNIX

#include "sqlite3.h"

#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/param.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

// Size of the write buffer used by journal files in bytes.
#ifndef SQLITE_DEMOVFS_BUFFERSZ
# define SQLITE_DEMOVFS_BUFFERSZ 8192
#endif

// The maximum pathname length supported by this VFS.
#define MAXPATHNAME 512

// When using this VFS, the sqlite3_file* handles that SQLite uses are
// actually pointers to instances of type UqbarFile.
typedef struct UqbarFile UqbarFile;
struct UqbarFile {
  sqlite3_file base;              // Base class. Must be first.
  char *zOurNode;
  char *zIdentifier;
  char *zName;

  char *aBuffer;                  // Pointer to malloc'd buffer
  int nBuffer;                    // Valid bytes of data in zBuffer
  sqlite3_int64 iBufferOfst;      // Offset in file of zBuffer[0]
};

typedef struct OptionStr OptionStr;
struct OptionStr {
    int is_empty;
    const char *string;
};

typedef struct Bytes Bytes;
struct Bytes {
    void *data;
    size_t len;
};

typedef struct Payload Payload;
struct Payload {
  int is_empty;       // 0 -> payload is empty
  const OptionStr *mime;
  Bytes *bytes;
};

typedef struct ProcessId ProcessId;
struct ProcessId {
    const char *process_name;
    const char *package_name;
    const char *publisher_node;
};

typedef struct IpcMetadata IpcMetadata;
struct IpcMetadata {
    OptionStr* ipc;
    OptionStr* metadata;
};

// extern Payload* get_payload_wrapped();
extern void get_payload_wrapped(Payload *payload);
extern void send_and_await_response_wrapped(
  const char *target_node,
  const ProcessId *target_process,
  const OptionStr *request_ipc,
  const OptionStr *request_metadata,
  const Payload *payload,
  const unsigned long long timeout,
  const IpcMetadata *return_val
);

// https://chat.openai.com/share/d3261d81-52f2-4fd7-b6c0-8238679e63f2
// This function looks for the key in the JSON and copies its value into the output buffer.
int getJsonValue(const char *json, const char *key, char *output, int outputSize) {
  char searchKey[256];  // This size should be adjusted based on your needs
  snprintf(searchKey, sizeof(searchKey), "\"%s\":", key);

  const char *keyStart = strstr(json, searchKey);
  if (keyStart == NULL) {
    snprintf(searchKey, sizeof(searchKey), "\\\"%s\\\":", key);
    keyStart = strstr(json, searchKey);
  }
  if (keyStart == NULL) return -1;

  const char *valueStart = keyStart + strlen(searchKey);
  while (isspace(*valueStart)) valueStart++;  // Skip any whitespace

  const char *valueEnd;
  if (*valueStart == '\"') {
      // Value is a string
      valueStart++;  // Move past the opening quote
      valueEnd = strchr(valueStart, '\"');
  } else if (*valueStart == '\\' && *(valueStart + 1) == '\"') {
      // Value is a string
      valueStart++;  // Move past the opening fas
      valueStart++;  // Move past the opening quote
      valueEnd = strchr(valueStart, '\\');
  } else {
      // Assume value is a number (integer or float)
      valueEnd = valueStart;
      while (isdigit(*valueEnd) || *valueEnd == '.' || *valueEnd == '-' || *valueEnd == '+') valueEnd++;
  }

  if (valueEnd == NULL) return -2;

  int valueLength = valueEnd - valueStart;
  if (valueLength >= outputSize) return -3;  // Ensure there's room in the output buffer

  strncpy(output, valueStart, valueLength);
  output[valueLength] = '\0';  // Null-terminate the string

  return 0;
}

// Write directly to the file passed as the first argument. Even if the
// file has a write-buffer (UqbarFile.aBuffer), ignore it.
static int uqbarDirectWrite(
  UqbarFile *p,                    // File handle
  const void *zBuf,               // Buffer containing data to write
  int iAmt,                       // Size of data to write in bytes
  sqlite_int64 iOfst              // File offset to write to
){
  ProcessId target_process = {.process_name = "vfs", .package_name = "sys", .publisher_node = "uqbar"};
  char temp[256];
  snprintf(
    temp,
    sizeof(temp),
    "{"
      "\"drive\": \"%s\","
      "\"action\": {"
        "\"WriteOffset\": {"
          "\"full_path\": \"%s\","
          "\"offset\": %llu"
        "}"
      "}"
    "}",
    p->zIdentifier,
    p->zName,
    (unsigned long long)iOfst
  );
  OptionStr request_ipc = {
    .is_empty = 1,
    .string = temp,
  };
  OptionStr empty_option_str = {
    .is_empty = 0,
    .string = "",
  };
  Bytes bytes = {
    .data = zBuf,
    .len = iAmt,
  };
  Payload payload = {
    .is_empty = 1,
    .mime = &empty_option_str,
    .bytes = &bytes,
  };

  char ipc_string[512];
  char metadata_string[512];
  OptionStr ipc = {
      .is_empty = 0,
      .string = ipc_string,
  };
  OptionStr metadata = {
      .is_empty = 0,
      .string = metadata_string,
  };
  IpcMetadata return_val = {
      .ipc = &ipc,
      .metadata = &metadata,
  };

  send_and_await_response_wrapped(
    p->zOurNode,
    &target_process,
    &request_ipc,
    &empty_option_str,
    &payload,
    5,
    &return_val
  );

  // TODO: check response is not an error

  return SQLITE_OK;
}

// Flush the contents of the UqbarFile.aBuffer buffer to disk. This is a
// no-op if this particular file does not have a buffer (i.e. it is not
// a journal file) or if the buffer is currently empty.
static int uqbarFlushBuffer(UqbarFile *p){
  int rc = SQLITE_OK;
  if( p->nBuffer ){
    rc = uqbarDirectWrite(p, p->aBuffer, p->nBuffer, p->iBufferOfst);
    p->nBuffer = 0;
  }
  return rc;
}

// Close a file.
static int uqbarClose(sqlite3_file *pFile){
  int rc;
  UqbarFile *p = (UqbarFile*)pFile;
  rc = uqbarFlushBuffer(p);
  sqlite3_free(p->zOurNode);
  sqlite3_free(p->zIdentifier);
  sqlite3_free(p->zName);
  sqlite3_free(p->aBuffer);
  return rc;
}

// Read data from a file.
static int uqbarRead(
  sqlite3_file *pFile,
  void *zBuf,
  int iAmt,
  sqlite_int64 iOfst
){
  UqbarFile *p = (UqbarFile*)pFile;
  off_t ofst;                     // Return value from lseek()
  int nRead;                      // Return value from read()
  int rc;                         // Return code from uqbarFlushBuffer()

  // Flush any data in the write buffer to disk in case this operation
  // is trying to read data the file-region currently cached in the buffer.
  // It would be possible to detect this case and possibly save an
  // unnecessary write here, but in practice SQLite will rarely read from
  // a journal file when there is data cached in the write-buffer.
  rc = uqbarFlushBuffer(p);
  if( rc!=SQLITE_OK ){
    return rc;
  }

  ProcessId target_process = {.process_name = "vfs", .package_name = "sys", .publisher_node = "uqbar"};
  char temp[256];
  snprintf(
    temp,
    sizeof(temp),
    "{"
      "\"drive\": \"%s\","
      "\"action\": {"
        "\"GetFileChunk\": {"
          "\"full_path\": \"%s\","
          "\"offset\": %llu,"
          "\"length\": %llu"
        "}"
      "}"
    "}",
    p->zIdentifier,
    p->zName,
    (unsigned long long)iOfst,
    (unsigned long long)iAmt
  );
  OptionStr request_ipc = {
    .is_empty = 1,
    .string = temp,
  };
  OptionStr empty_option_str = {
    .is_empty = 0,
    .string = "",
  };
  Bytes empty_bytes = {
    .data = NULL,
    .len = 0,
  };
  Payload request_payload = {
    .is_empty = 0,
    .mime = &empty_option_str,
    .bytes = &empty_bytes,
  };

  char ipc_string[512];
  char metadata_string[512];
  OptionStr ipc = {
      .is_empty = 0,
      .string = ipc_string,
  };
  OptionStr metadata = {
      .is_empty = 0,
      .string = metadata_string,
  };
  IpcMetadata response = {
      .ipc = &ipc,
      .metadata = &metadata,
  };

  send_and_await_response_wrapped(
    p->zOurNode,
    &target_process,
    &request_ipc,
    &empty_option_str,
    &request_payload,
    5,
    &response
  );

  if( response.ipc->is_empty == 0 ){
    // ipc must be populated
    return SQLITE_IOERR_READ;
  }

  char value[256];
  if( getJsonValue(response.ipc->string, "Err", value, sizeof(value)) == 0 ){
    // got error
    return SQLITE_IOERR_READ;
  }

  char mime_string[256];
  OptionStr mime = {
      .is_empty = 0,
      .string = mime_string,
  };
  Bytes bytes = {
      .data = zBuf,
      .len = iAmt,
  };
  Payload response_payload = {
      .is_empty = 0,
      .mime = &mime,
      .bytes = &bytes,
  };

  get_payload_wrapped(&response_payload);
  if( response_payload.is_empty == 0 ){
    // payload must be non-empty
    memset(zBuf, 0, iAmt);
    return SQLITE_IOERR_SHORT_READ;
  }

  if ( response_payload.bytes->len > iAmt ){
    return SQLITE_IOERR_READ;
  }

  if ( response_payload.bytes->len < iAmt ){
    memset(zBuf + response_payload.bytes->len, 0, iAmt - response_payload.bytes->len);
    return SQLITE_IOERR_SHORT_READ;
  }

  return SQLITE_OK;
}

// Write data to a crash-file.
static int uqbarWrite(
  sqlite3_file *pFile,
  const void *zBuf,
  int iAmt,
  sqlite_int64 iOfst
){
  UqbarFile *p = (UqbarFile*)pFile;

  if( p->aBuffer ){
    char *z = (char *)zBuf;       // Pointer to remaining data to write
    int n = iAmt;                 // Number of bytes at z
    sqlite3_int64 i = iOfst;      // File offset to write to

    while( n>0 ){
      int nCopy;                  // Number of bytes to copy into buffer

      // If the buffer is full, or if this data is not being written directly
      // following the data already buffered, flush the buffer. Flushing
      // the buffer is a no-op if it is empty.
      if( p->nBuffer==SQLITE_DEMOVFS_BUFFERSZ || p->iBufferOfst+p->nBuffer!=i ){
        int rc = uqbarFlushBuffer(p);
        if( rc!=SQLITE_OK ){
          return rc;
        }
      }
      assert( p->nBuffer==0 || p->iBufferOfst+p->nBuffer==i );
      p->iBufferOfst = i - p->nBuffer;

      // Copy as much data as possible into the buffer.
      nCopy = SQLITE_DEMOVFS_BUFFERSZ - p->nBuffer;
      if( nCopy>n ){
        nCopy = n;
      }
      memcpy(&p->aBuffer[p->nBuffer], z, nCopy);
      p->nBuffer += nCopy;

      n -= nCopy;
      i += nCopy;
      z += nCopy;
    }
  }else{
    return uqbarDirectWrite(p, zBuf, iAmt, iOfst);
  }

  return SQLITE_OK;
}

// Truncate a file. This is a no-op for this VFS (see header comments at
// the top of the file).
static int uqbarTruncate(sqlite3_file *pFile, sqlite_int64 size){
  return SQLITE_OK;
}

// Sync the contents of the file to the persistent media: no-op.
static int uqbarSync(sqlite3_file *pFile, int flags){
  return SQLITE_OK;
}

// Write the size of the file in bytes to *pSize.
static int uqbarFileSize(sqlite3_file *pFile, sqlite_int64 *pSize){
  UqbarFile *p = (UqbarFile*)pFile;

  // Flush the contents of the buffer to disk. As with the flush in the
  // uqbarRead() method, it would be possible to avoid this and save a write
  // here and there. But in practice this comes up so infrequently it is
  // not worth the trouble.
  int rc = uqbarFlushBuffer(p);
  if( rc!=SQLITE_OK ){
    return rc;
  }

  ProcessId target_process = {.process_name = "vfs", .package_name = "sys", .publisher_node = "uqbar"};
  char temp[256];
  snprintf(
    temp,
    sizeof(temp),
    "{"
      "\"drive\": \"%s\","
      "\"action\": {"
        "\"GetEntryLength\": \"%s\""
      "}"
    "}",
    p->zIdentifier,
    p->zName
  );
  OptionStr request_ipc = {
    .is_empty = 1,
    .string = temp,
  };
  OptionStr empty_option_str = {
    .is_empty = 0,
    .string = "",
  };
  Bytes empty_bytes = {
    .data = NULL,
    .len = 0,
  };
  Payload request_payload = {
    .is_empty = 0,
    .mime = &empty_option_str,
    .bytes = &empty_bytes,
  };

  char ipc_string[512];
  char metadata_string[512];
  OptionStr ipc = {
      .is_empty = 0,
      .string = ipc_string,
  };
  OptionStr metadata = {
      .is_empty = 0,
      .string = metadata_string,
  };
  IpcMetadata response = {
      .ipc = &ipc,
      .metadata = &metadata,
  };

  send_and_await_response_wrapped(
    p->zOurNode,
    &target_process,
    &request_ipc,
    &empty_option_str,
    &request_payload,
    5,
    &response
  );

  if( response.ipc->is_empty == 0 ) {
    // ipc must be populated
    return SQLITE_IOERR_READ;
  }

  char value[256];
  if( getJsonValue(response.ipc->string, "GetEntryLength", value, sizeof(value)) != 0 ){
    // could not find expected value
    return SQLITE_IOERR_READ;
  }

  if( strcmp(value, "null") == 0 ) {
    // file DNE
    return SQLITE_IOERR_READ;
  }

  char *endptr;
  unsigned long long length = strtoull(value, &endptr, 10);
  *pSize = (sqlite3_int64)length;

  return SQLITE_OK;
}

// Locking functions. The xLock() and xUnlock() methods are both no-ops.
// The xCheckReservedLock() always indicates that no other process holds
// a reserved lock on the database file. This ensures that if a hot-journal
// file is found in the file-system it is rolled back.
static int uqbarLock(sqlite3_file *pFile, int eLock){
  return SQLITE_OK;
}
static int uqbarUnlock(sqlite3_file *pFile, int eLock){
  return SQLITE_OK;
}
static int uqbarCheckReservedLock(sqlite3_file *pFile, int *pResOut){
  *pResOut = 0;
  return SQLITE_OK;
}

// No xFileControl() verbs are implemented by this VFS.
static int uqbarFileControl(sqlite3_file *pFile, int op, void *pArg){
  return SQLITE_OK;
}

// The xSectorSize() and xDeviceCharacteristics() methods. These two
// may return special values allowing SQLite to optimize file-system
// access to some extent. But it is also safe to simply return 0.
static int uqbarSectorSize(sqlite3_file *pFile){
  return 0;
}
static int uqbarDeviceCharacteristics(sqlite3_file *pFile){
  return 0;
}

// Open a file handle.
static int uqbarOpen(
  sqlite3_vfs *pVfs,              // VFS
  const char *zName,              // File to open, or 0 for a temp file
  sqlite3_file *pFile,            // Pointer to UqbarFile struct to populate
  int flags,                      // Input SQLITE_OPEN_XXX flags
  int *pOutFlags                  // Output SQLITE_OPEN_XXX flags (or NULL)
){
  static const sqlite3_io_methods uqbario = {
    1,                             // iVersion
    uqbarClose,                    // xClose
    uqbarRead,                     // xRead
    uqbarWrite,                    // xWrite
    uqbarTruncate,                 // xTruncate
    uqbarSync,                     // xSync
    uqbarFileSize,                 // xFileSize
    uqbarLock,                     // xLock
    uqbarUnlock,                   // xUnlock
    uqbarCheckReservedLock,        // xCheckReservedLock
    uqbarFileControl,              // xFileControl
    uqbarSectorSize,               // xSectorSize
    uqbarDeviceCharacteristics     // xDeviceCharacteristics
  };

  UqbarFile *p = (UqbarFile*)pFile;  // Populate this structure
  char *aBuf = 0;

  if( zName==0 ){
    return SQLITE_IOERR;
  }

  if( flags&SQLITE_OPEN_MAIN_JOURNAL ){
    aBuf = (char *)sqlite3_malloc(SQLITE_DEMOVFS_BUFFERSZ);
    if( !aBuf ){
      return SQLITE_NOMEM;
    }
  }
  p->aBuffer = aBuf;

  memset(p, 0, sizeof(UqbarFile));

  p->base.pMethods = &uqbario;

  char *zNameBackup;
  zNameBackup = sqlite3_malloc(strlen(zName) + 1);
  strcpy(zNameBackup, zName);

  char *token = strtok(zNameBackup, ":");
  if (token == NULL) {
    return SQLITE_IOERR_READ;
  }
  p->zOurNode = sqlite3_malloc(strlen(token) + 1);
  strcpy(p->zOurNode, token);
  token = strtok(NULL, ":");
  if (token == NULL) {
    return SQLITE_IOERR_READ;
  }
  p->zIdentifier = sqlite3_malloc(strlen(token) + 1);
  strcpy(p->zIdentifier, token);
  token = strtok(NULL, ":");
  if (token == NULL) {
    return SQLITE_IOERR_READ;
  }
  p->zName = sqlite3_malloc(strlen(token) + 1);
  strcpy(p->zName, token);

  sqlite3_free(zNameBackup);

  if( pOutFlags ){
    *pOutFlags = flags;
  }

  // check if file exists
  ProcessId target_process = {.process_name = "vfs", .package_name = "sys", .publisher_node = "uqbar"};
  char file_exists_request_ipc_string[256];
  snprintf(
    file_exists_request_ipc_string,
    sizeof(file_exists_request_ipc_string),
    "{"
      "\"drive\": \"%s\","
      "\"action\": {"
        "\"GetHash\": \"%s\""
      "}"
    "}",
    p->zIdentifier,
    p->zName
  );
  OptionStr file_exists_request_ipc = {
    .is_empty = 1,
    .string = file_exists_request_ipc_string,
  };
  OptionStr empty_option_str = {
    .is_empty = 0,
    .string = "",
  };
  Bytes bytes = {
    .data = "",
    .len = 0,
  };
  Payload payload = {
    .is_empty = 1,
    .mime = &empty_option_str,
    .bytes = &bytes,
  };

  char file_exists_response_ipc_string[512];
  char file_exists_response_metadata_string[512];
  OptionStr file_exists_response_ipc = {
      .is_empty = 0,
      .string = file_exists_response_ipc_string,
  };
  OptionStr file_exists_response_metadata = {
      .is_empty = 0,
      .string = file_exists_response_metadata_string,
  };
  IpcMetadata file_exists_response = {
      .ipc = &file_exists_response_ipc,
      .metadata = &file_exists_response_metadata,
  };

  send_and_await_response_wrapped(
    p->zOurNode,
    &target_process,
    &file_exists_request_ipc,
    &empty_option_str,
    &payload,
    5,
    &file_exists_response
  );

  if( file_exists_response.ipc->is_empty != 0 ){
    char value[256];
    if( getJsonValue(file_exists_response.ipc->string, "GetHash", value, sizeof(value)) == 0 ){
      // already exists
      return SQLITE_OK;
    }
  }

  // if file does not exist, create it
  char temp[256];
  snprintf(
    temp,
    sizeof(temp),
    "{"
      "\"drive\": \"%s\","
      "\"action\": {"
        "\"Add\": {"
          "\"full_path\": \"%s\","
          "\"entry_type\": \"NewFile\""
        "}"
      "}"
    "}",
    p->zIdentifier,
    p->zName
  );
  OptionStr request_ipc = {
    .is_empty = 1,
    .string = temp,
  };

  char ipc_string[512];
  char metadata_string[512];
  OptionStr ipc = {
      .is_empty = 0,
      .string = ipc_string,
  };
  OptionStr metadata = {
      .is_empty = 0,
      .string = metadata_string,
  };
  IpcMetadata response = {
      .ipc = &ipc,
      .metadata = &metadata,
  };

  send_and_await_response_wrapped(
    p->zOurNode,
    &target_process,
    &request_ipc,
    &empty_option_str,
    &payload,
    5,
    &response
  );

  return SQLITE_OK;
}

// Delete the file identified by argument zPath. If the dirSync parameter
// is non-zero, then ensure the file-system modification to delete the
// file has been synced to disk before returning.
// TODO: does this work as noop?
static int uqbarDelete(sqlite3_vfs *pVfs, const char *zPath, int dirSync){
    ProcessId target_process = {.process_name = "vfs", .package_name = "sys", .publisher_node = "uqbar"};

    char *zOurNode;
    char *zIdentifier;
    char *zName;

    // Extract the file information from zPath
    char *zPathBackup;
    zPathBackup = sqlite3_malloc(strlen(zPath) + 1);
    strcpy(zPathBackup, zPath);

    char *token = strtok(zPathBackup, ":");
    if (token == NULL) {
      sqlite3_free(zPathBackup);
      return SQLITE_IOERR_READ;
    }
    zOurNode = sqlite3_malloc(strlen(token) + 1);
    strcpy(zOurNode, token);
    token = strtok(NULL, ":");
    if (token == NULL) {
      sqlite3_free(zPathBackup);
      return SQLITE_IOERR_READ;
    }
    zIdentifier = sqlite3_malloc(strlen(token) + 1);
    strcpy(zIdentifier, token);
    token = strtok(NULL, ":");
    if (token == NULL) {
      sqlite3_free(zPathBackup);
      return SQLITE_IOERR_READ;
    }
    zName = sqlite3_malloc(strlen(token) + 1);
    strcpy(zName, token);

    sqlite3_free(zPathBackup);

    // Initialize empty_option_str before use
    OptionStr empty_option_str = {
        .is_empty = 1,
        .string = ""
    };

    char temp[256];
    snprintf(
      temp,
      sizeof(temp),
      "{"
        "\"drive\": \"%s\","
        "\"action\": {"
          "\"Delete\": \"%s\""
        "}"
      "}",
      zIdentifier,
      zName
    );
    OptionStr request_ipc = {
      .is_empty = 1,
      .string = temp,
    };

    char ipc_string[512];
    char metadata_string[512];
    OptionStr ipc = {
        .is_empty = 0,
        .string = ipc_string,
    };
    OptionStr metadata = {
        .is_empty = 0,
        .string = metadata_string,
    };
    Bytes empty_bytes = {
        .data = NULL, // Cast the const void* to void* if this is what's causing the warning
        .len = 0,
    };
    Payload request_payload = {
        .is_empty = 0,
        .mime = &empty_option_str,
        .bytes = &empty_bytes,
    };
    IpcMetadata response = {
        .ipc = &ipc,
        .metadata = &metadata,
    };

    send_and_await_response_wrapped(
      zOurNode,
      &target_process,
      &request_ipc,
      &empty_option_str,
      &request_payload,
      5,
      &response
    );

    // Free the allocated memory
    sqlite3_free(zOurNode);
    sqlite3_free(zIdentifier);
    sqlite3_free(zName);

    return SQLITE_OK;
}

#ifndef F_OK
# define F_OK 0
#endif
#ifndef R_OK
# define R_OK 4
#endif
#ifndef W_OK
# define W_OK 2
#endif

// Query the file-system to see if the named file exists, is readable or
// is both readable and writable.
// We run this as "always accessible" because we don't have access to the
// sqlite3_file node & identifier.
static int uqbarAccess(
  sqlite3_vfs *pVfs,
  const char *zPath,
  int flags,
  int *pResOut
){
  *pResOut = 0;
  return SQLITE_OK;
}

// Argument zPath points to a nul-terminated string containing a file path.
// If zPath is an absolute path, then it is copied as is into the output
// buffer. Otherwise, if it is a relative path, then the equivalent full
// path is written to the output buffer.
//
// This function assumes that paths are UNIX style. Specifically, that:
//
//   1. Path components are separated by a '/'. and
//   2. Full paths begin with a '/' character.
static int uqbarFullPathname(
  sqlite3_vfs *pVfs,              // VFS
  const char *zPath,              // Input path (possibly a relative path)
  int nPathOut,                   // Size of output buffer in bytes
  char *zPathOut                  // Pointer to output buffer
){
  sqlite3_snprintf(nPathOut, zPathOut, "%s", zPath);
  zPathOut[nPathOut-1] = '\0';

  return SQLITE_OK;
}

// The following four VFS methods:
//
//   xDlOpen
//   xDlError
//   xDlSym
//   xDlClose
//
// are supposed to implement the functionality needed by SQLite to load
// extensions compiled as shared objects. This simple VFS does not support
// this functionality, so the following functions are no-ops.
static void *uqbarDlOpen(sqlite3_vfs *pVfs, const char *zPath){
  return 0;
}
static void uqbarDlError(sqlite3_vfs *pVfs, int nByte, char *zErrMsg){
  sqlite3_snprintf(nByte, zErrMsg, "Loadable extensions are not supported");
  zErrMsg[nByte-1] = '\0';
}
static void (*uqbarDlSym(sqlite3_vfs *pVfs, void *pH, const char *z))(void){
  return 0;
}
static void uqbarDlClose(sqlite3_vfs *pVfs, void *pHandle){
  return;
}

// Parameter zByte points to a buffer nByte bytes in size. Populate this
// buffer with pseudo-random data.
static int uqbarRandomness(sqlite3_vfs *pVfs, int nByte, char *zByte){
  return SQLITE_OK;
}

// Sleep for at least nMicro microseconds. Return the (approximate) number
// of microseconds slept for.
static int uqbarSleep(sqlite3_vfs *pVfs, int nMicro){
  sleep(nMicro / 1000000);
  usleep(nMicro % 1000000);
  return nMicro;
}

// Set *pTime to the current UTC time expressed as a Julian day. Return
// SQLITE_OK if successful, or an error code otherwise.
//
//   http://en.wikipedia.org/wiki/Julian_day
//
// This implementation is not very good. The current time is rounded to
// an integer number of seconds. Also, assuming time_t is a signed 32-bit
// value, it will stop working some time in the year 2038 AD (the so-called
// "year 2038" problem that afflicts systems that store time this way).
static int uqbarCurrentTime(sqlite3_vfs *pVfs, double *pTime){
  time_t t = time(0);
  *pTime = t/86400.0 + 2440587.5;
  return SQLITE_OK;
}

// This function returns a pointer to the VFS implemented in this file.
// To make the VFS available to SQLite:
//
//   sqlite3_vfs_register(sqlite3_uqbarvfs(), 0);
sqlite3_vfs *sqlite3_uqbarvfs(void){
  static sqlite3_vfs uqbarvfs = {
    1,                            // iVersion
    sizeof(UqbarFile),            // szOsFile
    MAXPATHNAME,                  // mxPathname
    0,                            // pNext
    "uqbar",                      // zName
    0,                            // pAppData
    uqbarOpen,                    // xOpen
    uqbarDelete,                  // xDelete
    uqbarAccess,                  // xAccess
    uqbarFullPathname,            // xFullPathname
    uqbarDlOpen,                  // xDlOpen
    uqbarDlError,                 // xDlError
    uqbarDlSym,                   // xDlSym
    uqbarDlClose,                 // xDlClose
    uqbarRandomness,              // xRandomness
    uqbarSleep,                   // xSleep
    uqbarCurrentTime,             // xCurrentTime
  };
  return &uqbarvfs;
}

#endif // !defined(SQLITE_TEST) || SQLITE_OS_UNIX


#ifdef SQLITE_TEST

#if defined(INCLUDE_SQLITE_TCL_H)
#  include "sqlite_tcl.h"
#else
#  include "tcl.h"
#  ifndef SQLITE_TCLAPI
#    define SQLITE_TCLAPI
#  endif
#endif

#if SQLITE_OS_UNIX
static int SQLITE_TCLAPI register_uqbarvfs(
  ClientData clientData, // Pointer to sqlite3_enable_XXX function
  Tcl_Interp *interp,    // The TCL interpreter that invoked this command
  int objc,              // Number of arguments
  Tcl_Obj *CONST objv[]  // Command arguments
){
  sqlite3_vfs_register(sqlite3_uqbarvfs(), 1);
  return TCL_OK;
}
static int SQLITE_TCLAPI unregister_uqbarvfs(
  ClientData clientData, // Pointer to sqlite3_enable_XXX function
  Tcl_Interp *interp,    // The TCL interpreter that invoked this command
  int objc,              // Number of arguments
  Tcl_Obj *CONST objv[]  // Command arguments
){
  sqlite3_vfs_unregister(sqlite3_uqbarvfs());
  return TCL_OK;
}

// Register commands with the TCL interpreter.
int Sqlitetest_uqbarvfs_Init(Tcl_Interp *interp){
  Tcl_CreateObjCommand(interp, "register_uqbarvfs", register_uqbarvfs, 0, 0);
  Tcl_CreateObjCommand(interp, "unregister_uqbarvfs", unregister_uqbarvfs, 0, 0);
  return TCL_OK;
}

#else
int Sqlitetest_uqbarvfs_Init(Tcl_Interp *interp){ return TCL_OK; }
#endif

#endif // SQLITE_TEST

// Register sqlite3_uqbarvfs
int sqlite3_os_init()
{
    sqlite3_vfs_register(sqlite3_uqbarvfs(), 0);
    return 0;
}

int sqlite3_os_end()
{
    return 0;
}
