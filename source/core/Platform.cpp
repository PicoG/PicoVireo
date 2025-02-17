// Copyright (c) 2020 National Instruments
// SPDX-License-Identifier: MIT

/*! \file
 */

#include "DataTypes.h"
#include "Platform.h"

#include <cstdlib>
#include <cstdarg>
#include <cstdio>

#include "TypeAndDataManager.h"
#include "TypeDefiner.h"

#if kVireoOS_windows
  #define NOMINMAX
  #include <Windows.h>
#elif kVireoOS_macosxU
  #include <pthread.h>
  #include <time.h>
  #include <mach/mach_time.h>
  #include <unistd.h>
#elif kVireoOS_linuxU
  #include <pthread.h>
  #include <time.h>
  #include <unistd.h>
#elif kVireoOS_ZynqARM
  #include "xscutimer.h"
#elif kVireoOS_emscripten
  #include <emscripten.h>
#endif

// Enable VIREO_JOURNAL_ALLOCS to use supplementary map to track all mallocs and remember which
// actual pointers are leaked.  (map itself does not go through Platform Malloc and is not tracked.)
// Combine with VIREO_TRACK_MEMORY_ALLLOC_COUNTER in TypeAndDataManager to
// record allocation number in each Malloc, and set gWatchAlloc to find a specific allocation
// reported leaked in a previous run.

#define VIREO_JOURNAL_ALLOCS 0

#if VIREO_JOURNAL_ALLOCS
#include <set>
std::set <void*> gAllocSet;
#endif

#if defined(__rp2040__)
#include <pico/time.h>
#include <pico/unique_id.h>

static const char picog_platform[] = "rp2040";
static const char picog_board[] = "pico";
#else
static const char picog_platform[] = "unknown";
static const char picog_board[] = "none";
#endif

#if DEBUG_MEM
uint32_t usedMem = 0;
#endif

#if defined(VIREO_EMBEDDED_EXPERIMENT)

#include <malloc.h>
#include <new>

extern "C" void std_cpp_init();
extern "C" void std_io_init();
extern "C" void _exit();
extern uint32_t gTickCount;

void* operator new(std::size_t size) {
    return gPlatform.Mem.Malloc(size);
}

void* operator new[](std::size_t size) {
    return gPlatform.Mem.Malloc(size);
}

void operator delete(void* ptr) {
    return gPlatform.Mem.Free(ptr);
}

void operator delete[](void* ptr) {
    return gPlatform.Mem.Free(ptr);
}

void* operator new(std::size_t size, const std::nothrow_t&) {
    return gPlatform.Mem.Malloc(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) {
    return gPlatform.Mem.Malloc(size);
}

void operator delete(void* ptr, const std::nothrow_t&) {
    return gPlatform.Mem.Free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) {
    return gPlatform.Mem.Free(ptr);
}
#endif


namespace Vireo {

Platform gPlatform;

#define CMD_HEADER_LEN 8

const char cmdHeader[] = {
    0xF4, 0xF5, 0xF4, 0xF5, 0xF4, 0xF5, 0x00, 0x00
};

//============================================================
#if kVireoOS_windows
LONG WINAPI UnhandledExceptionFilter(_EXCEPTION_POINTERS *lpTopLevelExceptionFilter) {
    fprintf(stderr, "Uncaught exception such as Access Violation\n");
    ExitProcess(1);  // non-zero return code
}
#endif

void Platform::Setup()
{
#if defined(VIREO_EMBEDDED_EXPERIMENT)
    std_io_init();
    std_cpp_init();
#endif

#if kVireoOS_windows
    SetUnhandledExceptionFilter(UnhandledExceptionFilter);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);  // do not display different error dialogs
#endif
}

void Platform::Shutdown()
{
#if defined(VIREO_EMBEDDED_EXPERIMENT)
    _exit();
#endif
}

//============================================================
PlatformMemory gPlatformMem;

//! Static memory allocator used primarily by the TM
void* PlatformMemory::Malloc(size_t countAQ)
{
#if DEBUG_MEM
    usedMem += countAQ;
    //printf("MALLOC: %d\tTotal: %d\n", countAQ, usedMem);
#endif

#if defined(VIREO_TRACK_MALLOC)
    size_t logicalSize = countAQ;
    countAQ += sizeof(size_t);
#endif

    void* pBuffer = malloc(countAQ);
    if (pBuffer) {
        
#if VIREO_JOURNAL_ALLOCS
        gAllocSet.insert(pBuffer);
#endif
        memset(pBuffer, 0, countAQ);
#if defined(VIREO_TRACK_MALLOC)
        _totalAllocated += logicalSize;
        *(size_t*)pBuffer = logicalSize;
        pBuffer = (size_t*)pBuffer + 1;
#else
        _totalAllocated++;
#endif
    }
    return pBuffer;
}
//------------------------------------------------------------
//! Static memory deallocator used primarily by the TM.
void* PlatformMemory::Realloc(void* pBuffer, size_t countAQ)
{
#if defined(VIREO_TRACK_MALLOC)
    pBuffer = (size_t*)pBuffer - 1;
    size_t currentLogicalSize = *(size_t*)pBuffer;
    size_t newLogicalSize = countAQ;
    countAQ += sizeof(size_t);
#endif
#if VIREO_JOURNAL_ALLOCS
    if (pBuffer) {
        std::set<void*>::iterator it = gAllocSet.find(pBuffer);
        if (it != gAllocSet.end())
            gAllocSet.erase(it);
        else
            gPlatform.IO.Printf("invalid realloc\n");
    }
#endif
    pBuffer = realloc(pBuffer, countAQ);
#if VIREO_JOURNAL_ALLOCS
    if (pBuffer)
        gAllocSet.insert(pBuffer);
#endif

#if defined(VIREO_TRACK_MALLOC)
    if (pBuffer) {
        _totalAllocated = _totalAllocated - currentLogicalSize + newLogicalSize;
        *(size_t*)pBuffer = newLogicalSize;
        pBuffer = (size_t*)pBuffer + 1;
    }
#endif
    return pBuffer;
}
//------------------------------------------------------------
//! Static memory deallocator used primarily by the TM.
void PlatformMemory::Free(void* pBuffer)
{
#if defined(VIREO_TRACK_MALLOC)
    pBuffer = (size_t*)pBuffer - 1;
    _totalAllocated -= *(size_t*)pBuffer;
#else
    _totalAllocated--;
#endif
#if VIREO_JOURNAL_ALLOCS
    if (pBuffer) {
        std::set<void*>::iterator it = gAllocSet.find(pBuffer);
        if (it != gAllocSet.end())
            gAllocSet.erase(it);
        else
            gPlatform.IO.Printf("invalid free\n");
    }
#endif
    free(pBuffer);
}
#if VIREO_JOURNAL_ALLOCS
void DumpPlatformMemoryLeaks() {  // to be called from debugger
    std::set<void*>::iterator it = gAllocSet.begin(), ite = gAllocSet.end();
    while (it != ite) {
        void *pBuffer = *it;
        size_t s = ((size_t*)pBuffer)[2];  // hack, but this is just for debugging
        gPlatform.IO.Printf("Leak %lx %ld\n", pBuffer, s);
        ++it;
    }
}
#endif

PlatformIO::PlatformIO() {
    _cmdLen = 0;
    _cmdMatch = 0;
    _readCmd = false;
    _unreadI = 0;
}

void PlatformIO::Print(char c) {
    fwrite(&c, 1, 1, stdout);
}

//============================================================
//! Static memory deallocator used for all TM memory management.
void PlatformIO::Print(ConstCStr str)
{
    fwrite(str, 1, strlen(str), stdout);
//#if kVireoOS_emscripten
    fflush(stdout);
//#endif
#if VIREO_JOURNAL_ALLOCS
    if (*str == 256)  // never true, hack to prevent dead code elim, only for debugging
        DumpPlatformMemoryLeaks();
#endif
}
//------------------------------------------------------------
//! Static memory deallocator used for all TM memory management.
void PlatformIO::Print(Int32 len, ConstCStr str)
{
    fwrite(str, 1, len, stdout);
//#if kVireoOS_emscripten
    fflush(stdout);
//#endif
}
//------------------------------------------------------------
//! Static memory deallocator used for all TM memory management.
void PlatformIO::Printf(ConstCStr format, ...) const
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
//#if kVireoOS_emscripten
    fflush(stdout);
//#endif
}
//------------------------------------------------------------
//! Static memory deallocator used for all TM memory management.
void PlatformIO::ReadFile(SubString *name, StringRef buffer)
{
    buffer->Resize1DOrEmpty(0);
#if defined(VIREO_STDIO)
    TempStackCString    cString(name);

    FILE* h = fopen(cString.BeginCStr(), "r");
    if (h != nullptr) {
        fseek(h, 0L, SEEK_END);
        IntIndex bytesToRead = (IntIndex)ftell(h);
        rewind(h);

        buffer->Resize1DOrEmpty(bytesToRead);
        if (buffer->Length() == bytesToRead) {
            size_t bytesRead = fread(buffer->Begin(), 1, (size_t)bytesToRead, h);
            buffer->Resize1DOrEmpty((IntIndex)bytesRead);
        }
    }
#endif
}
//------------------------------------------------------------

#if defined(VIREO_EMBEDDED_EXPERIMENT)
char sampleProgram[] =
    "start( VI<( clump( "
    "    Println('Hello, M4. I can fly to the store.') "
    ") ) > ) ";
#endif

uint8_t PlatformIO::checkCommand() {
    bool readByte = true;
    bool timeout = false;
    int c;

    char etx = 0x03; //Ctrl+C, abort without saving

    while (readByte) {
        c = _getchar_timeout_us(0);
        if (c < 0) {
            return CMD_UNKNOWN;
        }

        if (c == etx) {
            return CMD_ABORT;
        }

        //If first byte doesn't match command header, shortcut return
        if (c != cmdHeader[0]) {
            //
            _cmd[_cmdLen] = c;
            _cmdLen++;
            return CMD_UNKNOWN;
        }

        //from this point on we've matched at least 1 byte of the header

        //read bytes until we either match the header or don't
        //storing the bytes in a buffer in case of mismatch and need to
        //give those back to the app so we affect it as little as possible
        while (c == cmdHeader[_cmdLen]) {
            _cmd[_cmdLen] = c;
            _cmdLen++;

            if (_cmdLen >= CMD_HEADER_LEN) {
                //matched the header, prepare to read command
                //once we match the header we'll reuse those bytes in the buffer to read the rest
                _readCmd = true;
                _cmdLen = 0;
                break;
            }
            
            c = _getchar_timeout_us(0);
            if (c < 0) {
                return CMD_UNKNOWN;
            }
        }

        if (_readCmd) {
            //Read command ID byte
            uint8_t cmd = fgetc(stdin);

            //Todo: Array of cmd handlers?
            switch (cmd) {
                //Device info commands
                case CMD_VERSION:
                    //TODO report picoG FW version
                    fprintf(stdout, "0.0.0\n");
                    break;

                case CMD_PLATFORM:
                    fprintf(stdout, "%s\n", picog_platform);
                    break;

                case CMD_BOARD:
                    fprintf(stdout, "%s\n", picog_board);
                    break;

                case CMD_SERIAL:
#ifdef __rp2040__
                    //use cmd buffer temporarily for serial # copy
                    _cmdLen = PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1;
                    pico_get_unique_board_id_string(_cmd, _cmdLen);
                    fprintf(stdout, "%s\n", _cmd);
#endif
                    break;

                case CMD_ALIAS:
                    //TODO Alias
                    fprintf(stdout, "MyPico\n");
                    break;

                case CMD_ISEXEC:
                    fprintf(stdout, "F\n");
                    break;

                case CMD_RESET:
                    _cmd[0] = 'r';
                    _cmd[1] = 'e';
                    _cmd[2] = 's';
                    _cmd[3] = 'e';
                    _cmd[4] = 't';
                    _cmd[5] = '(';
                    _cmd[6] = ')';
                    _cmd[7] = '\n';
                    _cmdLen = 8;

                    fprintf(stdout, "OK\n");
                    break;

                case CMD_ABORT:
                    return CMD_ABORT;
                    break;

                case CMD_RUNMAIN:
                    _cmd[0] = 'r';
                    _cmd[1] = 'u';
                    _cmd[2] = 'n';
                    _cmd[3] = '(';
                    _cmd[4] = ')';
                    _cmd[5] = '\n';
                    _cmdLen = 6;

                    fprintf(stdout, "OK\n");
                    break;
            }

            fflush(stdout);

            //Since we intercepted and acted on a command need to read another byte
            readByte = true;

        } else {
            //store the last read byte
            _cmd[_cmdLen++] = c;

            //At this point in the code we should always have at least 2 bytes in the buffer:
            //the first byte matched and the second byte didn't (or more matches)
            //we return the first byte we read and 
            c = _cmd[0];
            _unreadI = 1;

            //Don't need to read another byte, we have data to return to app
            readByte = false;
        }
    }

    return CMD_UNKNOWN;
}

char PlatformIO::_fgetc(FILE *file) {
    char c;
    //Had a header mismatch and need to give all the bytes back to the app
    if (_unreadCmd) {
        c = _cmd[_unreadI];
        _unreadI++;
        if (_unreadI == _cmdLen) {
            _unreadCmd = false;
        }
    } else {
        bool readByte = true;
        while (readByte) {

            c = fgetc(file);

            //If first byte doesn't match command header, shortcut return
            if (c != cmdHeader[0]) {
                return c;
            }

            //from this point on we've matched at least 1 byte of the header

            //Reset number of bytes filled into buffer
            _cmdLen = 0;

            //read bytes until we either match the header or don't
            //storing the bytes in a buffer in case of mismatch and need to
            //give those back to the app so we affect it as little as possible
            while (c == cmdHeader[_cmdLen]) {
                _cmd[_cmdLen] = c;
                _cmdLen++;

                if (_cmdLen >= CMD_HEADER_LEN) {
                    //matched the header, prepare to read command
                    //once we match the header we'll reuse those bytes in the buffer to read the rest
                    _readCmd = true;
                    _cmdLen = 0;
                    break;
                }
                
                c = fgetc(file);
            }

            if (_readCmd) {
                //Read command ID byte
                uint8_t cmd = fgetc(file);

                //Todo: Array of cmd handlers?
                switch (cmd) {
                    //Device info commands
                    case CMD_VERSION:
                        //TODO report picoG FW version
                        fprintf(stdout, "0.0.0\n");
                        break;

                    case CMD_PLATFORM:
                        fprintf(stdout, "%s\n", picog_platform);
                        break;

                    case CMD_BOARD:
                        fprintf(stdout, "%s\n", picog_board);
                        break;

                    case CMD_SERIAL:
    #ifdef __rp2040__
                        //use cmd buffer temporarily for serial # copy
                        _cmdLen = PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1;
                        pico_get_unique_board_id_string(_cmd, _cmdLen);
                        fprintf(stdout, "%s\n", _cmd);
    #endif
                        break;

                    case CMD_ALIAS:
                        //TODO Alias
                        fprintf(stdout, "MyPico\n");
                        break;

                    case CMD_ISEXEC:
                        fprintf(stdout, "F\n");
                        break;

                    case CMD_RESET:
                        _cmd[0] = 'r';
                        _cmd[1] = 'e';
                        _cmd[2] = 's';
                        _cmd[3] = 'e';
                        _cmd[4] = 't';
                        _cmd[5] = '(';
                        _cmd[6] = ')';
                        _cmd[7] = '\n';
                        _cmdLen = 8;

                        fprintf(stdout, "OK\n");
                        break;

                    case CMD_ABORT:
                        fprintf(stdout, "FAIL\n");
                        break;

                    case CMD_RUNMAIN:
                        _cmd[0] = 'r';
                        _cmd[1] = 'u';
                        _cmd[2] = 'n';
                        _cmd[3] = '(';
                        _cmd[4] = ')';
                        _cmd[5] = '\n';
                        _cmdLen = 6;

                        fprintf(stdout, "OK\n");
                        break;
                }

                fflush(stdout);

                //Since we intercepted and acted on a command need to read another byte
                readByte = true;

            } else {
                //store the last read byte
                _cmd[_cmdLen++] = c;

                //At this point in the code we should always have at least 2 bytes in the buffer:
                //the first byte matched and the second byte didn't (or more matches)
                //we return the first byte we read and 
                c = _cmd[0];
                _unreadI = 1;

                //Don't need to read another byte, we have data to return to app
                readByte = false;
            }
        }
    }

    return c;
}

void PlatformIO::ReadStdin(StringRef buffer)
{
    buffer->Resize1D(0);
#if defined(VIREO_EMBEDDED_EXPERIMENT)
    buffer->AppendCStr(sampleProgram);
#else
    buffer->Reserve(300);

    //following commented line supports CMD protocol handling, disable until fixed
    //char c = _fgetc(stdin);

    char c = fgetc(stdin);

    if (c == '\r') {
        fputc('\n', stdout);
    } else {
        fputc(c, stdout); //echo
    }

    fflush(stdout);

    while (true) {
        if ((c == (char)EOF) || (c == '\n' || (c == '\r'))) {
            break;
        }
        buffer->Append(c);
        c = fgetc(stdin);
        if (c == '\r') {
            fputc('\n', stdout);
        } else {
            fputc(c, stdout); //echo
        }
        fflush(stdout);
    }
#endif
}

#if 0
    //------------------------------------------------------------
    NIError PlatformIO::ReadStdin(StringRef buffer) {
        const int lenlen = 10;
        Int32 i = 0;
        char c;

        c = fgetc(stdin);
        if (c == '<') {
            //  <count>xxxxxxxx "<4>exit"
            // command starts with a size
            char packetHeader[lenlen];  // NOLINT(runtime/arrays)
            do {
                c = fgetc(stdin);
                if (i < lenlen) {
                    packetHeader[i++] = c;
                }
            } while ( c !=  '>' );
            SubString packet((Utf8Char*)packetHeader, (Utf8Char*)packetHeader+i);
            IntMax packetSize = 0;
            packet.ReadInt(&packetSize);

            PlatformIO::Printf("packet size %d\n", (int) packetSize);

            for (i = 0; i < packetSize; i++) {
                c = fgetc(stdin);
                _mallocBuffer[i] = c;
                if ((i % 2000) == 0) {
                    PlatformIO::Print(".");
                }
            }
            PlatformIO::Print("\n");

            buffer->AliasAssign((Utf8Char*)_mallocBuffer, (Utf8Char*)_mallocBuffer + packetSize);
            PlatformIO::Printf("packet read complete <%d>\n", (int)packetSize);
            return kNIError_Success;
        } else {
            const int MaxCommandLine = 20000;
            while (true) {
                if ((c == (char)EOF) || (c == '\n') || i >= MaxCommandLine) {
                    break;
                }
                _mallocBuffer[i++] = c;
                c = fgetc(stdin);
            }
            buffer->AliasAssign((Utf8Char*)_mallocBuffer, (Utf8Char*)_mallocBuffer + i);
            return ((c == (char)EOF) && (0 == i)) ? kNIError_kResourceNotFound : kNIError_Success;
        }
        return kNIError_Success;
    }

#endif

//============================================================
PlatformTickType PlatformTimer::TickCount()
{
#if defined(_WIN32) || defined(_WIN64)

    // System returns 100ns count.
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return now.dwLowDateTime;

#elif defined(kVireoOS_macosxU)

    return mach_absolute_time();

#elif defined(kVireoOS_wiring)

    return micros();

#elif defined(__rp2040__)
    absolute_time_t time;
    time = get_absolute_time();
    return time;

#elif defined(kVireoOS_linuxU)

    timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return ((Int64)time.tv_sec * 1000000000) + (Int64)time.tv_nsec;

#elif defined(kVireoOS_emscripten)

    // On modern browsers emscripten_get_now(), uses performance.now,
    // which returns sub milliseconds on the fractional part of the double that is returned.
    // Multiplying by a 1000 to get microseconds accuracy, before casting to PlatformTickType
    return (PlatformTickType) (emscripten_get_now() * 1000);

#elif defined(kVireoOS_ZynqARM)

    // Hard coded to the max Zynq7000 clock rate for now.
    // the clock register is only 32 bits so it can wrap around
    // pretty quick, depending on the prescalar.
    static Int64 TickCount;
    XScuTimer_Config*   pConfig;
    volatile UInt64     scuTickCount;
    static UInt64    lastScuTickCount = 0;
    static XScuTimer    Timer;
    static XScuTimer     *pTimer = nullptr;

    if (!pTimer) {
        pTimer = &Timer;

        pConfig = XScuTimer_LookupConfig(XPAR_XSCUTIMER_0_DEVICE_ID);

        Int32 reply = XScuTimer_CfgInitialize(pTimer, pConfig, pConfig->BaseAddr);
        if (reply != XST_SUCCESS) {
            return 0;
        }

        XScuTimer_SetPrescaler(pTimer, 10);
        XScuTimer_LoadTimer(pTimer, 0xFFFFFFFF);
        XScuTimer_EnableAutoReload(pTimer);
        XScuTimer_Start(pTimer);
        lastScuTickCount = ((UInt64)XScuTimer_GetCounterValue(pTimer));
    }

    scuTickCount = ((UInt64)XScuTimer_GetCounterValue(pTimer));

    if (scuTickCount > lastScuTickCount) {
        // Wrapped around, the last one should be close to 0
        // the current one should be close to max Int32
        TickCount += lastScuTickCount + (0xFFFFFFFF - scuTickCount);
    } else {
        TickCount += (lastScuTickCount - scuTickCount);
    }
    lastScuTickCount = scuTickCount;
    return TickCount;

#elif defined(VIREO_EMBEDDED_EXPERIMENT)
    // #error MicroSecondCount not defined
    return gTickCount;
#else
    return 0;
#endif
}

//------------------------------------------------------------
PlatformTickType PlatformTimer::MicrosecondsFromNowToTickCount(Int64 microsecondCount)
{
    return TickCount() + MicrosecondsToTickCount(microsecondCount);
}
//------------------------------------------------------------
PlatformTickType PlatformTimer::MillisecondsFromNowToTickCount(Int64 millisecondCount)
{
    return TickCount() + MicrosecondsToTickCount(millisecondCount * 1000);
}
//------------------------------------------------------------
PlatformTickType PlatformTimer::MicrosecondsToTickCount(Int64 microseconds)
{
#if defined(_WIN32) || defined(_WIN64)

    // Windows filetime base tick count is 100ns
    return microseconds * 10;

#elif defined(kVireoOS_macosxU)

    // Scaling according to the kernel parameters.
    static mach_timebase_info_data_t sTimebaseInfo = { 0, 0 };
    if (sTimebaseInfo.denom == 0) {
        (void) mach_timebase_info(&sTimebaseInfo);
    }
    return (microseconds * 1000) * sTimebaseInfo.denom / sTimebaseInfo.numer;

#elif defined(kVireoOS_wiring)

    // tick count is microseconds for arduino's wiring
    return ticks;

#elif defined(kVireoOS_linuxU)

    // tick count is nanoseconds for linux
    return microseconds * 1000;

#elif defined(kVireoOS_emscripten)

    // Tick count is already in microseconds
    return microseconds;

#elif defined(kVireoOS_ZynqARM)

    // Still experimental.
    return microseconds * 333333 / 10000;

#elif defined(VIREO_EMBEDDED_EXPERIMENT)

    return microseconds / 1000;

#elif defined(__rp2040__)

    return microseconds;

#else
    // #error MicroSecondCount not defined
    return 0;
#endif
}
//------------------------------------------------------------
Int64 PlatformTimer::TickCountToMilliseconds(PlatformTickType ticks)
{
    return (TickCountToMicroseconds(ticks) / 1000);
}
//------------------------------------------------------------
Int64 PlatformTimer::TickCountToMicroseconds(PlatformTickType ticks)
{
#if defined(_WIN32) || defined(_WIN64)

    // Windows filetime base tick count is 100ns
    return ticks / 10;

#elif defined(kVireoOS_macosxU)

    // Get scale factor used to convert to nanoseconds
    static mach_timebase_info_data_t sTimebaseInfo = { 0, 0 };
    if (sTimebaseInfo.denom == 0) {
        (void) mach_timebase_info(&sTimebaseInfo);
    }
    return (ticks * sTimebaseInfo.numer / sTimebaseInfo.denom) / 1000;

#elif defined(kVireoOS_wiring)

    // tick count is microseconds for arduino's wiring
    return ticks;

#elif defined(kVireoOS_linuxU)

    // tick count is nanoseconds for linux
    return ticks / 1000;

#elif defined(kVireoOS_emscripten)

    // Tick count is already in microseconds
    return ticks;

#elif defined(kVireoOS_ZynqARM)

    // Still experimental.
    return ticks * 10000 / 333333;

#elif defined(VIREO_EMBEDDED_EXPERIMENT)

    return ticks * 1000;

#elif defined(__rp2040__)

    return ticks; //ticks are uS

#else
    // #error MicroSecondCount not defined
    return 0;
#endif
}

#if !kVireoOS_emscripten  // Cannot sleep in emscripten code, must sleep on JS side
void PlatformTimer::SleepMilliseconds(Int64 milliseconds) {
#if defined(_WIN32) || defined(_WIN64)
    Sleep((DWORD)milliseconds);
#elif defined __rp2040__
    sleep_us(milliseconds * 1000);
#elif kVireoOS_macosxU || kVireoOS_linuxU
    usleep(UInt32(milliseconds * 1000));
#else
    #error "implement SleepMilliseconds"
#endif
}
#endif  // !kVireoOS_emscripten

#if VIREO_TRACK_MALLOC
VIREO_FUNCTION_SIGNATURE1(MemUsed, UInt32) {
    _Param(0) = gPlatform.Mem.TotalAllocated();

    return _NextInstruction();
}

DEFINE_VIREO_BEGIN(VPlatform)
    DEFINE_VIREO_FUNCTION(MemUsed, "p(o(UInt32))")
DEFINE_VIREO_END()
#endif //VIREO_TRACK_MALLOC

}  // namespace Vireo
