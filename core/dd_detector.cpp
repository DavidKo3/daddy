﻿/// @brief     Remote based logging-SDK.
/// @license   MIT License
/// @author    BonexGoo
#include "dd_detector.hpp"

// Dependencies
#include "dd_string.hpp"
#include "dd_thread.hpp"
#include <chrono>
#include <string>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>

#if DD_OS_WINDOWS
    #include <windows.h>
    #include <memoryapi.h>
    #define LOG_VIEW_OPEN_FOR_WRITE(FM, OFFSET, LENGTH) MapViewOfFile((FM).mMap, FILE_MAP_WRITE, 0, OFFSET, LENGTH)
    #define LOG_VIEW_OPEN_FOR_READ(FM, OFFSET, LENGTH)  MapViewOfFile((FM).mMap, FILE_MAP_READ, 0, OFFSET, LENGTH)
    #define LOG_VIEW_CLOSE(BUF, LENGTH)                 UnmapViewOfFile(BUF)
    #define LOG_VIEW_FLUSH(BUF, LENGTH)                 FlushViewOfFile(BUF, LENGTH)
#else
    #include <cstring>
    #include <dirent.h>
    #include <unistd.h>
    #include <signal.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
    #define LOG_VIEW_OPEN_FOR_WRITE(FM, OFFSET, LENGTH) mmap(0, LENGTH, PROT_READ | PROT_WRITE, MAP_SHARED, (FM).mFD, OFFSET)
    #define LOG_VIEW_OPEN_FOR_READ(FM, OFFSET, LENGTH)  mmap(0, LENGTH, PROT_READ, MAP_SHARED, (FM).mFD, OFFSET)
    #define LOG_VIEW_CLOSE(BUF, LENGTH)                 munmap(BUF, LENGTH)
    #define LOG_VIEW_FLUSH(BUF, LENGTH)                 msync(BUF, LENGTH, MS_ASYNC)
#endif

extern "C" {
    #if DD_OS_WINDOWS_MINGW | DD_OS_LINUX | DD_OS_OSX | DD_OS_IOS
        extern int vasprintf(char**, const char*, va_list);
    #else
        extern int vsnprintf(char*, size_t, const char*, va_list);
    #endif
}

namespace Daddy {

////////////////////////////////////////////////////////////////////////////////////////////////////
// ■ FileMapP
class FileMapP
{
public:
    FileMapP(utf8s filename, uint32_t filesize) // 쓰기용
    {
        #if DD_OS_WINDOWS
            mFD = CreateFileA(filename, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

            SECURITY_ATTRIBUTES SA;
            SA.nLength = sizeof(SECURITY_ATTRIBUTES);
            SA.lpSecurityDescriptor = nullptr;
            SA.bInheritHandle = true;
            mMap = CreateFileMappingA(mFD, &SA, PAGE_READWRITE, 0, filesize, filename);
        #else
            mFD = open(filename, O_RDWR | O_CREAT | O_TRUNC, (mode_t) 0600);
            lseek(mFD, filesize - 1, SEEK_SET);
            write(mFD, "", 1);
            fstat(mFD, &mSB);
        #endif
    }
    FileMapP(utf8s filename) // 읽기용
    {
        #if DD_OS_WINDOWS
            mFD = nullptr;
            mMap = OpenFileMappingA(GENERIC_READ, true, filename);

            if(!mMap) // 현재 로그작성자가 연결되어 있지 않은 경우
            {
                mFD = CreateFileA(filename, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

                SECURITY_ATTRIBUTES SA;
                SA.nLength = sizeof(SECURITY_ATTRIBUTES);
                SA.lpSecurityDescriptor = nullptr;
                SA.bInheritHandle = true;
                mMap = CreateFileMappingA(mFD, &SA, PAGE_READONLY, 0, 0, nullptr);
            }
        #else
            mFD = open(filename, O_RDONLY);
            fstat(mFD, &mSB);
        #endif
    }
    ~FileMapP()
    {
        #if DD_OS_WINDOWS
            CloseHandle(mMap);
            CloseHandle(mFD);
        #else
            close(mFD);
        #endif
    }

public:
    bool isValid() const
    {
        #if DD_OS_WINDOWS
            return (mMap != nullptr);
        #else
            return (mFD != -1);
        #endif
    }

public:
    #if DD_OS_WINDOWS
        HANDLE mFD;
        HANDLE mMap;
    #else
        int mFD;
        struct stat mSB;
    #endif
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// ■ LogPageP
class LogPageP : public dEscaper
{
public:
    enum {LOG_FILE_SIZE = 4096 * 256 * 5}; // 5MB
    enum {LOG_PAGE_SIZE = 4096 * 16}; // 64KB
    enum {LOG_PAGE_COUNT = LOG_FILE_SIZE / LOG_PAGE_SIZE}; // 80개
    enum {LOG_UNIT_PACKING = 4};
    /// @brief 페이지내 청크구조 - 헤더
    /// @sheet ┏━━━━━━━┳━━━━━━━━━━━━━━━━━━━━┳━━━━━━━━━━━━━━┳━━━━━━━┓
    /// @sheet ┃ 시작기호[1]  ┃ 페이지 활성화 여부[1]                  ┃ 총 유니트 패킹수량[2]      ┃ 페이지ID[4]  ┃
    /// @sheet ┣━━━━━━━╋━━━━━━━━━━━━━━━━━━━━╋━━━━━━━━━━━━━━╋━━━━━━━┫
    /// @sheet ┃ '#'          ┃ 작업중:'+', 작업완료:'-', 로깅종료:'/' ┃ 4바이트단위 길이(uint16_t) ┃ uint32_t     ┃
    /// @sheet ┗━━━━━━━┻━━━━━━━━━━━━━━━━━━━━┻━━━━━━━━━━━━━━┻━━━━━━━┛
    /// @brief 페이지내 청크구조 - 로그유니트
    /// @sheet ┏━━━━━━━━━━━━━━┳━━━━━━┳━━━━━━━━━━━┓
    /// @sheet ┃ 현재 유니트 패킹수량[2]    ┃ 함수ID[2]  ┃ 페이로드[N]          ┃
    /// @sheet ┣━━━━━━━━━━━━━━╋━━━━━━╋━━━━━━━━━━━┫
    /// @sheet ┃ 4바이트단위 길이(uint16_t) ┃ uint16_t   ┃ 버전 및 함수아규먼트 ┃
    /// @sheet ┗━━━━━━━━━━━━━━┻━━━━━━┻━━━━━━━━━━━┛

public:
    static uint32_t alignedSize(uint32_t size)
    {
        return (size + LOG_UNIT_PACKING - 1) / LOG_UNIT_PACKING * LOG_UNIT_PACKING;
    }

protected:
    struct PageHeader
    {
        char mCode;
        char mActivity;
        uint16_t mPackingCount;
        uint32_t mPageID;
    };
    struct UnitHeader
    {
        uint16_t mPackingCount;
        uint16_t mFuncID;
    };

DD_escaper(LogPageP, dEscaper):
    void _init_(InitType type)
    {
        mBuffer = nullptr;
        mBufferOffset = 0;
        mPageOffset = 0;
        mPageID = 0;
    }
    void _quit_()
    {
        if(mBuffer)
            LOG_VIEW_CLOSE(mBuffer, LOG_PAGE_SIZE);
    }
    void _move_(_self_&& rhs)
    {
        mBuffer = rhs.mBuffer;
        mBufferOffset = rhs.mBufferOffset;
        mPageOffset = rhs.mPageOffset;
        mPageID = rhs.mPageID;
    }
    uint8_t* mBuffer;
    uint32_t mBufferOffset;
    uint32_t mPageOffset;
    uint32_t mPageID;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// ■ LogPageWriterP
class LogPageWriterP : public LogPageP
{
public:
    void open()
    {
    }
    void close()
    {
        _quit_();
        _init_(InitType::ClearOnly);
    }
    uint8_t* writeLock(const FileMapP& fm, uint16_t funcId, uint32_t payloadSize)
    {
        mMutex.lock();//////////////////////////////////////////////////////
        // 세마포어ON //////////////////////////////////////////////////////
        {
            UnitHeader NewHeader = {
                uint16_t((payloadSize + LOG_UNIT_PACKING - 1) / LOG_UNIT_PACKING), funcId};
            // 공간확보
            const uint32_t PackingPayloadSize = LOG_UNIT_PACKING * NewHeader.mPackingCount;
            validPage(fm, sizeof(UnitHeader) + PackingPayloadSize);
            // 유니트-헤더 복사
            memcpy(mBuffer + mBufferOffset, &NewHeader, sizeof(UnitHeader));
        }
        return mBuffer + mBufferOffset + sizeof(UnitHeader);
    }
    void writeUnlock(uint8_t* alignedPtr)
    {
        {
            mBufferOffset = uint32_t(alignedPtr - mBuffer);
            // 페이지-헤더 갱신
            rewriteHeader('+', mBufferOffset);
        }
        // 세마포어OFF //////////////////////////////////////////////////////
        mMutex.unlock();//////////////////////////////////////////////////////
    }

private:
    void validPage(const FileMapP& fm, uint32_t space)
    {
        if(!mBuffer || LOG_PAGE_SIZE < mBufferOffset + space)
        {
            // 현재페이지 정리
            if(mBuffer)
            {
                rewriteHeader('-', mBufferOffset); // 페이지 비활성화
                LOG_VIEW_CLOSE(mBuffer, LOG_PAGE_SIZE);
                mBuffer = nullptr;
                mBufferOffset = 0;
                mPageOffset = (mPageOffset + 1) % LOG_PAGE_COUNT;
                mPageID = 0;
            }

            // 새페이지 구성
            mBuffer = (uint8_t*) LOG_VIEW_OPEN_FOR_WRITE(fm, LOG_PAGE_SIZE * mPageOffset, LOG_PAGE_SIZE);
            mBufferOffset = rewriteHeader('+', sizeof(UnitHeader)); // 페이지 초기화
        }
    }
    uint32_t rewriteHeader(char actcode, uint32_t offset)
    {
        PageHeader NewHeader = {'#', actcode,
            uint16_t((offset - sizeof(PageHeader)) / LOG_UNIT_PACKING), mPageID};

        // 헤더복사
        memcpy(mBuffer, &NewHeader, sizeof(PageHeader));
        LOG_VIEW_FLUSH(mBuffer, offset);
        return sizeof(PageHeader);
    }

DD_escaper(LogPageWriterP, LogPageP):
    void _init_(InitType type)
    {
        _super_::_init_(type);
    }
    void _quit_()
    {
        if(mBuffer)
            rewriteHeader('/', mBufferOffset);
        _super_::_quit_();
    }
    void _move_(_self_&& rhs)
    {
        _super_::_move_(DD_rvalue(rhs));
        mMutex = rhs.mMutex;
    }
    dMutex mMutex;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// ■ LogPageReaderP
class LogPageReaderP : public LogPageP
{
public:
    void open(const FileMapP& fm)
    {
        loadPage(fm);
    }
    void close()
    {
        _quit_();
        _init_(InitType::ClearOnly);
    }

public:
    dDetector::ReadResult readOnce(const FileMapP& fm, dDetector::ReadCB cb)
    {
        // 페이지 재정비
        if(mBufferOffset == mPageSize)
        {
            if(mPageBusy)
            {
                dDetector::ReadResult Result = dDetector::Readed;
                // 세마포어ON //////////////////////////////////////////////////////
                {
                    const PageHeader& CurHeader = *((PageHeader*) mBuffer);
                    mPageBusy = (CurHeader.mActivity == '+');
                    mPageSize = sizeof(PageHeader) + LOG_UNIT_PACKING * CurHeader.mPackingCount;
                    if(CurHeader.mActivity == '/')
                        Result = dDetector::ExitProgram;
                    else if(mBufferOffset == mPageSize)
                        Result = dDetector::Unreaded;
                }
                // 세마포어OFF //////////////////////////////////////////////////////
                if(Result != dDetector::Readed)
                    return Result;
            }
            else
            {
                const uint32_t OldPageOffset = mPageOffset;
                mPageOffset = (mPageOffset + 1) % LOG_PAGE_COUNT;
                const dDetector::ReadResult Result = loadPage(fm);
                if(Result != dDetector::Readed)
                {
                    mPageOffset = OldPageOffset;
                    return Result;
                }
            }
        }

        const bool Busy = mPageBusy;
        if(Busy) DD_nothing; // 세마포어ON //////////////////////////////////////////////////////
        {
            const UnitHeader& NewHeader = *((UnitHeader*) (mBuffer + mBufferOffset));
            cb((dDetector::FuncID) NewHeader.mFuncID, mBuffer + mBufferOffset + sizeof(UnitHeader), LOG_UNIT_PACKING * NewHeader.mPackingCount);
            mBufferOffset += sizeof(UnitHeader) + LOG_UNIT_PACKING * NewHeader.mPackingCount;
        }
        if(Busy) DD_nothing; // 세마포어OFF //////////////////////////////////////////////////////
        return dDetector::Readed;
    }

private:
    dDetector::ReadResult loadPage(const FileMapP& fm)
    {
        dDetector::ReadResult Result = dDetector::Readed;
        // 세마포어ON //////////////////////////////////////////////////////
        {
            auto NewBuffer = (uint8_t*) LOG_VIEW_OPEN_FOR_READ(fm, LOG_PAGE_SIZE * mPageOffset, LOG_PAGE_SIZE);
            const PageHeader& NewHeader = *((PageHeader*) NewBuffer);
            if(NewHeader.mCode == '#')
            {
                mBuffer = NewBuffer;
                mBufferOffset = sizeof(PageHeader);
                mPageID = NewHeader.mPageID;
                mPageBusy = (NewHeader.mActivity == '+');
                mPageSize = sizeof(PageHeader) + LOG_UNIT_PACKING * NewHeader.mPackingCount;
                if(NewHeader.mActivity == '/')
                    Result = dDetector::ExitProgram;
            }
            else Result = dDetector::Unreaded; // 현재 페이지에 로그쓰기가 되어 있지 않은 경우
        }
        // 세마포어OFF //////////////////////////////////////////////////////
        return Result;
    }

DD_escaper(LogPageReaderP, LogPageP):
    void _init_(InitType type)
    {
        _super_::_init_(type);
        mPageBusy = false;
        mPageSize = 0;
    }
    void _quit_()
    {
        _super_::_quit_();
    }
    void _move_(_self_&& rhs)
    {
        _super_::_move_(DD_rvalue(rhs));
        mPageBusy = rhs.mPageBusy;
        mPageSize = rhs.mPageSize; 
    }
    bool mPageBusy;
    uint32_t mPageSize;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// ■ DetectorWriterP
class DetectorWriterP
{
public:
    static DetectorWriterP& ST()
    {DD_global DetectorWriterP _; return _;}

private:
    DetectorWriterP() : mLogFM("nabang.blog", LogPageP::LOG_FILE_SIZE)
    {
        mPageWriter.open();
    }
    ~DetectorWriterP()
    {
        mPageWriter.close();
    }

public:
    void writeS(dDetector::FuncID id, utf8s_nn s, int32_t sn)
    {
        const uint32_t PayloadSize1 = LogPageP::alignedSize(sizeof(uint16_t) + sn + 1);
        auto PayloadPtr = mPageWriter.writeLock(mLogFM, id, PayloadSize1);

        // 페이로드 구성1
        *((uint16_t*) PayloadPtr) = uint16_t(sn);
        memcpy(PayloadPtr + sizeof(uint16_t), s, sn);
        PayloadPtr[sizeof(uint16_t) + sn] = '\0'; // null문자처리
        PayloadPtr += PayloadSize1;

        // 완료
        mPageWriter.writeUnlock(PayloadPtr);
    }
    void writeSS(dDetector::FuncID id, utf8s_nn s1, int32_t sn1, utf8s_nn s2, int32_t sn2)
    {
        const uint32_t PayloadSize1 = LogPageP::alignedSize(sizeof(uint16_t) + sn1 + 1);
        const uint32_t PayloadSize2 = LogPageP::alignedSize(sizeof(uint16_t) + sn2 + 1);
        auto PayloadPtr = mPageWriter.writeLock(mLogFM, id, PayloadSize1);

        // 페이로드 구성1
        *((uint16_t*) PayloadPtr) = uint16_t(sn1);
        memcpy(PayloadPtr + sizeof(uint16_t), s1, sn1);
        PayloadPtr[sizeof(uint16_t) + sn1] = '\0'; // null문자처리
        PayloadPtr += PayloadSize1;

        // 페이로드 구성2
        *((uint16_t*) PayloadPtr) = uint16_t(sn2);
        memcpy(PayloadPtr + sizeof(uint16_t), s2, sn2);
        PayloadPtr[sizeof(uint16_t) + sn2] = '\0'; // null문자처리
        PayloadPtr += PayloadSize2;

        // 완료
        mPageWriter.writeUnlock(PayloadPtr);
    }
    template <typename T>
    void writeT(dDetector::FuncID id, T n)
    {
        const uint32_t PayloadSize1 = LogPageP::alignedSize(sizeof(T));
        auto PayloadPtr = mPageWriter.writeLock(mLogFM, id, PayloadSize1);

        // 페이로드 구성1
        *((T*) PayloadPtr) = n;
        PayloadPtr += PayloadSize1;

        // 완료
        mPageWriter.writeUnlock(PayloadPtr);
    }
    template <typename T>
    void writeST(dDetector::FuncID id, utf8s_nn s, int32_t sn, T t)
    {
        const uint32_t PayloadSize1 = LogPageP::alignedSize(sizeof(uint16_t) + sn + 1);
        const uint32_t PayloadSize2 = LogPageP::alignedSize(sizeof(T));
        auto PayloadPtr = mPageWriter.writeLock(mLogFM, id, PayloadSize1 + PayloadSize2);

        // 페이로드 구성1
        *((uint16_t*) PayloadPtr) = uint16_t(sn);
        memcpy(PayloadPtr + sizeof(uint16_t), s, sn);
        PayloadPtr[sizeof(uint16_t) + sn] = '\0'; // null문자처리
        PayloadPtr += PayloadSize1;

        // 페이로드 구성2
        *((T*) PayloadPtr) = t;
        PayloadPtr += PayloadSize2;

        // 완료
        mPageWriter.writeUnlock(PayloadPtr);
    }

public:
    static int64_t now()
    {
        auto Now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Now.time_since_epoch()).count();
    }
    static utf8s createString(int32_t& length, utf8s format, va_list args)
    {
        #if DD_OS_LINUX | DD_OS_OSX | DD_OS_IOS
            char* Result = nullptr;
            length = vasprintf(&Result, format, args);
            return (utf8s) Result;
        #else
            length = vsnprintf(nullptr, 0, format, args);
            char* Result = (char*) std::malloc(length + 1);
            vsnprintf(Result, length + 1, format, args);
            return (utf8s) Result;
        #endif
    }
    static void releaseString(utf8s ptr)
    {
        std::free((void*) ptr);
    }

private:
    FileMapP mLogFM;
    LogPageWriterP mPageWriter;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// ■ DetectorReaderP
class DetectorReaderP
{
public:
    static DetectorReaderP& ST()
    {DD_global DetectorReaderP _; return _;}

private:
    DetectorReaderP() : mLogFM("nabang.blog")
    {
        if(mLogFM.isValid())
            mPageReader.open(mLogFM);
    }
    ~DetectorReaderP()
    {
        if(mLogFM.isValid())
            mPageReader.close();
    }

public:
    dDetector::ReadResult readOnce(dDetector::ReadCB cb)
    {
        if(mLogFM.isValid())
            return mPageReader.readOnce(mLogFM, cb);
        return dDetector::LogNotFound;
    }

private:
    FileMapP mLogFM;
    LogPageReaderP mPageReader;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// ■ Stack
dDetector::Stack::Stack(const dLiteral& name) : mName(name)
{
    DetectorWriterP::ST().writeST(ScopeBeginST, mName.string(), mName.length(), DetectorWriterP::now());
}

dDetector::Stack::Stack(Stack&& rhs) : mName(DD_rvalue(rhs.mName))
{
}

dDetector::Stack::~Stack()
{
    DetectorWriterP::ST().writeST(ScopeEndST, mName.string(), mName.length(), DetectorWriterP::now());
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// ■ dDetector
void dDetector::runClient(dLiteral exepath, dLiteral option, dLiteral hostname, dLiteral workpath)
{
    #if DD_OS_WINDOWS
        #define PATH_MAX (1024 + 1)
        CHAR CommandLine[PATH_MAX];
        GetCurrentDirectoryA(PATH_MAX, CommandLine);
        strcat_s(CommandLine, "\\");
        strcat_s(CommandLine, exepath.buildNative());
        strcat_s(CommandLine, " ");
        strcat_s(CommandLine, hostname.buildNative());

        // 프로세스 실행
        STARTUPINFOA SI;
        ZeroMemory(&SI, sizeof(SI));
        SI.cb = sizeof(SI);
        PROCESS_INFORMATION PI;
        ZeroMemory(&PI, sizeof(PI));
        #if DD_BUILD_DEBUG
            if(!strcmp(option.buildNative(), "run"))
                CreateProcessA(NULL, CommandLine, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &SI, &PI);
        #else
            if(!strcmp(option.buildNative(), "run"))
                CreateProcessA(NULL, CommandLine, NULL, NULL, FALSE, 0, NULL, NULL, &SI, &PI);
        #endif
    #elif DD_OS_LINUX
        #define PATH_MAX (1024 + 1)
        char Link[PATH_MAX];
        sprintf(Link, "/proc/%d/exe", getpid());
        char ExecutePath[PATH_MAX] = {0};
        readlink(Link, ExecutePath, PATH_MAX);
        int LastSlash = 0;
        for(int i = 0; ExecutePath[i]; ++i)
            if(ExecutePath[i] == '/')
                LastSlash = i;
        ExecutePath[LastSlash + 1] = '\0';
        strcat(ExecutePath, exepath.buildNative());

        if(chmod(ExecutePath, 0755) != -1)
        {
            // 프로세스 실행
            if(!strcmp(option.buildNative(), "certify_only")) {} // 스킵
            else if(!strcmp(option.buildNative(), "run"))
            {
                if(fork() == 0)
                {
                    chmod(ExecutePath, 0755);
                    if(0 < hostname.length())
                        execlp(ExecutePath, ExecutePath, hostname.buildNative(), NULL);
                    else execlp(ExecutePath, ExecutePath, NULL);
                }
            }
            else if(!strcmp(option.buildNative(), "run_with_console"))
            {
                chmod(ExecutePath, 0755);
                char SystemCall[PATH_MAX];
                SystemCall[0] = '\0';
                if(0 < workpath.length())
                {
                    strcat(SystemCall, "mkdir -p ");
                    strcat(SystemCall, workpath.buildNative());
                    strcat(SystemCall, " && cd ");
                    strcat(SystemCall, workpath.buildNative());
                    strcat(SystemCall, " && ");
                }
                strcat(SystemCall, "gnome-terminal -e '");
                strcat(SystemCall, ExecutePath);
                if(0 < hostname.length())
                {
                    strcat(SystemCall, " ");
                    strcat(SystemCall, hostname.buildNative());
                }
                strcat(SystemCall, "\'");
                system(SystemCall);
            }
        }
    #else
        #error [daddy] this platform is not ready!
    #endif
}

void dDetector::killClient(dLiteral name, bool all)
{
    #if DD_OS_WINDOWS
        /* 나중에 정리할 것!
        #include <windows.h>
        #include <tlhelp32.h>
        #include <iostream>
        #include <string>
        #include "psapi.h"

        DWORD GetProcessByFileName(char* name){
            DWORD process_id_array[1024];
            DWORD bytes_returned;
            DWORD num_processes;
            HANDLE hProcess;
            char image_name[256];
            char buffer[256];
            int i;
            DWORD exitcode;
            EnumProcesses(process_id_array, 256*sizeof(DWORD), &bytes_returned);
            num_processes = (bytes_returned/sizeof(DWORD));
            for (i = 0; i < num_processes; i++) {
                hProcess=OpenProcess(PROCESS_ALL_ACCESS,TRUE,process_id_array[i]);
                if(GetModuleBaseName(hProcess,0,image_name,256)){
                    if(!stricmp(image_name,name)){
                        CloseHandle(hProcess);
                        return process_id_array[i];
                    }
                }
                CloseHandle(hProcess);
            }
            return 0;
        }
        void __cdecl main(int argc, char *argv[])
        {
            DWORD dwPID;
            dwPID = GetProcessByFileName("calc.exe");
            printf("%lu", (unsigned long)dwPID);
            return;
        }*/
    #elif DD_OS_LINUX
        DIR* CurDir = opendir("/proc/");
        dirent* CurDirEntry = nullptr;
        while((CurDirEntry = readdir(CurDir)) != nullptr)
        {
            if(strspn(CurDirEntry->d_name, "0123456789") == strlen(CurDirEntry->d_name))
            {
                char ExeLink[252];
                strcpy(ExeLink, "/proc/");
                strcat(ExeLink, CurDirEntry->d_name);
                strcat(ExeLink, "/exe");

                char TargetName[252];
                int TargetLength = readlink(ExeLink, TargetName, 252);
                if(0 < TargetLength)
                {
                    TargetName[TargetLength] = '\0';
                    if(strstr(TargetName, name.buildNative()))
                    {
                        const int ProcessID = atoi(CurDirEntry->d_name);
                        kill(ProcessID, SIGINT);
                        if(!all) break;
                    }
                }
            }
        }
        closedir(CurDir);
    #else
        #error [daddy] this platform is not ready!
    #endif
}

void dDetector::stamp(dLiteral name)
{
    DetectorWriterP::ST().writeST(StampST, name.string(), name.length(), DetectorWriterP::now());
}

dDetector::Stack dDetector::scope(dLiteral name)
{
    return Stack(name);
}

void dDetector::trace(Level level, utf8s format, ...)
{
    va_list Args;
    va_start(Args, format);
    int32_t Length;
    utf8s Result = DetectorWriterP::createString(Length, format, Args);
    va_end(Args);

    switch(level)
    {
    case InfoLevel: printf("<info> %s\n", Result); break;
    case WarnLevel: printf("<warn> %s\n", Result); break;
    case ErrorLevel: printf("<error> %s\n", Result); break;
    }
    DetectorWriterP::ST().writeST(TraceST, Result, Length, (int32_t) level);
    DetectorWriterP::releaseString(Result);
}

void dDetector::valid(bool& condition, utf8s format, ...)
{
    if(!condition)
    {
        va_list Args;
        va_start(Args, format);
        int32_t Length;
        utf8s Result = DetectorWriterP::createString(Length, format, Args);
        va_end(Args);

        DD_global int32_t gValidKey = -1;
        char ValidSemaphore[1024];
        sprintf(ValidSemaphore, "nabang-valid-%d", ++gValidKey);

        printf("<valid:%d> %s\n", gValidKey, Result);
        DetectorWriterP::ST().writeST(ValidST, Result, Length, gValidKey);
        DetectorWriterP::releaseString(Result);

        dSemaphore Waiting;
        Waiting.bind(ValidSemaphore);
        Waiting.lock();
        Waiting.lock();
        Waiting.unlock();

        int32_t Command = 0;
        if(FILE* NewFile = fopen(ValidSemaphore, "rb"))
        {
            fread(&Command, 4, 1, NewFile);
            fclose(NewFile);
            remove(ValidSemaphore);
        }

        switch(Command)
        {
        case 0: // break
            DD_crash;
            break;
        case 1: // continue
            break;
        case 2: // ignore
            condition = true;
            break;
        }
    }
}

void dDetector::setValue(dLiteral name, dLiteral value)
{
    DetectorWriterP::ST().writeSS(SetValueSS, name.string(), name.length(), value.string(), value.length());
}

void dDetector::setValue(dLiteral name, int32_t value)
{
    DetectorWriterP::ST().writeST(SetValueST, name.string(), name.length(), value);
}

void dDetector::addValue(dLiteral name, int32_t addition)
{
    DetectorWriterP::ST().writeST(AddValueST, name.string(), name.length(), addition);
}

dDetector::ReadResult dDetector::readOnce(ReadCB cb)
{
    return DetectorReaderP::ST().readOnce(cb);
}

int32_t dDetector::parseInt32(addr& payload)
{
    const int32_t Result = *((int32_t*) payload);
    payload = ((uint8_t*) payload) + LogPageP::alignedSize(sizeof(int32_t));
    return Result;
}

int64_t dDetector::parseInt64(addr& payload)
{
    const int64_t Result = *((int64_t*) payload);
    payload = ((uint8_t*) payload) + LogPageP::alignedSize(sizeof(int64_t));
    return Result;
}

utf8s dDetector::parseString(addr& payload)
{
    const uint16_t StringLength = *((uint16_t*) payload);
    utf8s Result = utf8s(((uint8_t*) payload) + sizeof(uint16_t));
    payload = ((uint8_t*) payload) + LogPageP::alignedSize(sizeof(uint16_t) + StringLength + 1);
    return Result;
}

} // namespace Daddy
