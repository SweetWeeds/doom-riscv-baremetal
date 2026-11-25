/*
 * newlib system call layer for a machine with no operating system.
 *
 * newlib's stdio, malloc and exit() funnel into the underscore functions
 * below. Implementing them here (instead of linking libgloss, which forwards
 * them to a host through ecall/HTIF) lets DOOM's unmodified C code run:
 *   - stdout/stderr go to the UART,
 *   - the embedded WAD looks like an ordinary read-only file, so DOOM's
 *     fopen/fread/fseek code path (w_file_stdc.c) works as-is,
 *   - every attempt to create or write a file (config, savegame) fails
 *     cleanly, which DOOM already tolerates.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>

#include "platform_hardware.h"

#undef errno
extern int errno;

#define STANDARD_INPUT_FILE_DESCRIPTOR 0
#define STANDARD_OUTPUT_FILE_DESCRIPTOR 1
#define STANDARD_ERROR_FILE_DESCRIPTOR 2
#define FIRST_EMBEDDED_FILE_DESCRIPTOR 3
#define MAXIMUM_OPEN_EMBEDDED_FILES 8

#define MICROSECONDS_PER_MILLISECOND 1000
#define MILLISECONDS_PER_SECOND 1000

/* Symbols from wad_image.S and the linker script. */
extern const unsigned char embeddedWadStart[];
extern const unsigned char embeddedWadEnd[];
extern char __heap_start[];
extern char __heap_end[];

typedef struct
{
    const char *fileName;
    const unsigned char *contents;
    size_t sizeInBytes;
} EmbeddedFile;

typedef struct
{
    int isInUse;
    const EmbeddedFile *file;
    size_t readPosition;
} OpenEmbeddedFile;

/* The WAD name DOOM is told to load (-iwad) must match this name. */
static const char embeddedWadFileName[] = "doom1.wad";

static EmbeddedFile embeddedFiles[1];
static int isEmbeddedFileTableReady = 0;
static OpenEmbeddedFile openEmbeddedFiles[MAXIMUM_OPEN_EMBEDDED_FILES];

static void prepareEmbeddedFileTable(void)
{
    if (isEmbeddedFileTableReady)
    {
        return;
    }
    embeddedFiles[0].fileName = embeddedWadFileName;
    embeddedFiles[0].contents = embeddedWadStart;
    embeddedFiles[0].sizeInBytes = (size_t)(embeddedWadEnd - embeddedWadStart);
    isEmbeddedFileTableReady = 1;
}

/*
 * DOOM may ask for "doom1.wad", "./doom1.wad" or a path from its IWAD search
 * list, so only the final path component is compared.
 */
static const char *findFinalPathComponent(const char *path)
{
    const char *lastSlash = strrchr(path, '/');
    if (lastSlash == NULL)
    {
        return path;
    }
    return lastSlash + 1;
}

static const EmbeddedFile *findEmbeddedFile(const char *path)
{
    prepareEmbeddedFileTable();
    const char *requestedName = findFinalPathComponent(path);
    for (size_t fileIndex = 0; fileIndex < sizeof(embeddedFiles) / sizeof(embeddedFiles[0]); fileIndex++)
    {
        if (strcasecmp(requestedName, embeddedFiles[fileIndex].fileName) == 0)
        {
            return &embeddedFiles[fileIndex];
        }
    }
    return NULL;
}

static OpenEmbeddedFile *lookUpOpenFile(int fileDescriptor)
{
    int slotIndex = fileDescriptor - FIRST_EMBEDDED_FILE_DESCRIPTOR;
    if (slotIndex < 0 || slotIndex >= MAXIMUM_OPEN_EMBEDDED_FILES)
    {
        return NULL;
    }
    if (!openEmbeddedFiles[slotIndex].isInUse)
    {
        return NULL;
    }
    return &openEmbeddedFiles[slotIndex];
}

static int isConsoleFileDescriptor(int fileDescriptor)
{
    return fileDescriptor == STANDARD_INPUT_FILE_DESCRIPTOR
        || fileDescriptor == STANDARD_OUTPUT_FILE_DESCRIPTOR
        || fileDescriptor == STANDARD_ERROR_FILE_DESCRIPTOR;
}

int _open(const char *path, int flags, int mode)
{
    (void)mode;
    int accessMode = flags & O_ACCMODE;
    if (accessMode != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND)) != 0)
    {
        /* Read-only "disk": config files and savegames cannot be written. */
        errno = EROFS;
        return -1;
    }

    const EmbeddedFile *file = findEmbeddedFile(path);
    if (file == NULL)
    {
        errno = ENOENT;
        return -1;
    }

    for (int slotIndex = 0; slotIndex < MAXIMUM_OPEN_EMBEDDED_FILES; slotIndex++)
    {
        if (!openEmbeddedFiles[slotIndex].isInUse)
        {
            openEmbeddedFiles[slotIndex].isInUse = 1;
            openEmbeddedFiles[slotIndex].file = file;
            openEmbeddedFiles[slotIndex].readPosition = 0;
            return FIRST_EMBEDDED_FILE_DESCRIPTOR + slotIndex;
        }
    }
    errno = EMFILE;
    return -1;
}

int _close(int fileDescriptor)
{
    if (isConsoleFileDescriptor(fileDescriptor))
    {
        return 0;
    }
    OpenEmbeddedFile *openFile = lookUpOpenFile(fileDescriptor);
    if (openFile == NULL)
    {
        errno = EBADF;
        return -1;
    }
    openFile->isInUse = 0;
    return 0;
}

ssize_t _read(int fileDescriptor, void *destination, size_t requestedByteCount)
{
    if (fileDescriptor == STANDARD_INPUT_FILE_DESCRIPTOR)
    {
        /* No interactive input: behave like an empty stdin. */
        return 0;
    }
    OpenEmbeddedFile *openFile = lookUpOpenFile(fileDescriptor);
    if (openFile == NULL)
    {
        errno = EBADF;
        return -1;
    }
    size_t remainingByteCount = openFile->file->sizeInBytes - openFile->readPosition;
    size_t copiedByteCount = requestedByteCount;
    if (copiedByteCount > remainingByteCount)
    {
        copiedByteCount = remainingByteCount;
    }
    memcpy(destination, openFile->file->contents + openFile->readPosition, copiedByteCount);
    openFile->readPosition += copiedByteCount;
    return (ssize_t)copiedByteCount;
}

ssize_t _write(int fileDescriptor, const void *source, size_t byteCount)
{
    if (fileDescriptor != STANDARD_OUTPUT_FILE_DESCRIPTOR && fileDescriptor != STANDARD_ERROR_FILE_DESCRIPTOR)
    {
        errno = EBADF;
        return -1;
    }
    platformConsoleWriteBytes((const char *)source, byteCount);
    return (ssize_t)byteCount;
}

off_t _lseek(int fileDescriptor, off_t offset, int origin)
{
    if (isConsoleFileDescriptor(fileDescriptor))
    {
        errno = ESPIPE;
        return -1;
    }
    OpenEmbeddedFile *openFile = lookUpOpenFile(fileDescriptor);
    if (openFile == NULL)
    {
        errno = EBADF;
        return -1;
    }

    off_t basePosition;
    if (origin == SEEK_SET)
    {
        basePosition = 0;
    }
    else if (origin == SEEK_CUR)
    {
        basePosition = (off_t)openFile->readPosition;
    }
    else if (origin == SEEK_END)
    {
        basePosition = (off_t)openFile->file->sizeInBytes;
    }
    else
    {
        errno = EINVAL;
        return -1;
    }

    off_t newPosition = basePosition + offset;
    if (newPosition < 0)
    {
        errno = EINVAL;
        return -1;
    }
    openFile->readPosition = (size_t)newPosition;
    return newPosition;
}

int _fstat(int fileDescriptor, struct stat *status)
{
    memset(status, 0, sizeof(*status));
    if (isConsoleFileDescriptor(fileDescriptor))
    {
        status->st_mode = S_IFCHR;
        return 0;
    }
    OpenEmbeddedFile *openFile = lookUpOpenFile(fileDescriptor);
    if (openFile == NULL)
    {
        errno = EBADF;
        return -1;
    }
    status->st_mode = S_IFREG | S_IRUSR;
    status->st_size = (off_t)openFile->file->sizeInBytes;
    return 0;
}

int _stat(const char *path, struct stat *status)
{
    const EmbeddedFile *file = findEmbeddedFile(path);
    if (file == NULL)
    {
        errno = ENOENT;
        return -1;
    }
    memset(status, 0, sizeof(*status));
    status->st_mode = S_IFREG | S_IRUSR;
    status->st_size = (off_t)file->sizeInBytes;
    return 0;
}

int _isatty(int fileDescriptor)
{
    if (isConsoleFileDescriptor(fileDescriptor))
    {
        return 1;
    }
    errno = ENOTTY;
    return 0;
}

void *_sbrk(ptrdiff_t incrementInBytes)
{
    static char *currentHeapEnd = __heap_start;
    char *previousHeapEnd = currentHeapEnd;
    if (currentHeapEnd + incrementInBytes > __heap_end || currentHeapEnd + incrementInBytes < __heap_start)
    {
        /* DOOM's zone allocator expects malloc to return NULL, not crash. */
        errno = ENOMEM;
        return (void *)-1;
    }
    currentHeapEnd += incrementInBytes;
    return previousHeapEnd;
}

void _exit(int exitCode)
{
    platformPowerOff(exitCode);
}

int _gettimeofday(struct timeval *timeValue, void *timeZone)
{
    (void)timeZone;
    uint64_t elapsedMilliseconds = platformReadTimeMilliseconds();
    timeValue->tv_sec = (time_t)(elapsedMilliseconds / MILLISECONDS_PER_SECOND);
    timeValue->tv_usec = (suseconds_t)((elapsedMilliseconds % MILLISECONDS_PER_SECOND) * MICROSECONDS_PER_MILLISECOND);
    return 0;
}

clock_t _times(struct tms *processTimes)
{
    clock_t elapsedMilliseconds = (clock_t)platformReadTimeMilliseconds();
    processTimes->tms_utime = elapsedMilliseconds;
    processTimes->tms_stime = 0;
    processTimes->tms_cutime = 0;
    processTimes->tms_cstime = 0;
    return elapsedMilliseconds;
}

/* Single-process machine: process control calls have nothing to act on. */
#define ONLY_PROCESS_ID 1

int _getpid(void)
{
    return ONLY_PROCESS_ID;
}

int _kill(int processId, int signalNumber)
{
    (void)processId;
    (void)signalNumber;
    errno = EINVAL;
    return -1;
}

int _unlink(const char *path)
{
    (void)path;
    errno = EROFS;
    return -1;
}

int _link(const char *existingPath, const char *newPath)
{
    (void)existingPath;
    (void)newPath;
    errno = EROFS;
    return -1;
}

int mkdir(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    errno = EROFS;
    return -1;
}

/* DOOM uses system() only to launch a GUI error dialog (zenity). */
int system(const char *command)
{
    (void)command;
    return -1;
}
