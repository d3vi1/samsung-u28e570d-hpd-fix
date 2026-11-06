#include "monitorfw/storage.hpp"
#include "storage_internal.hpp"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <climits>
#include <iomanip>
#include <sstream>

namespace monitorfw {
namespace detail {
std::string recoveryKey(const MonitorIdentity& identity) {
    const std::string stable = jsonQuote(identity.manufacturer) + "\n" + jsonQuote(identity.model) + "\n" +
        (identity.serialAmbiguous || identity.serial.empty() ? "ambiguous-model" : jsonQuote(identity.serial));
    return sha256(Bytes(stable.begin(),stable.end()));
}
}
namespace {
void fail(const char* operation, DWORD code = GetLastError()) {
    std::ostringstream text; text << operation << " failed (Win32 " << code << ")";
    throw Error(text.str());
}
class Handle {
    HANDLE handle_;
public:
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
    ~Handle() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return handle_; }
    HANDLE release() { HANDLE value = handle_; handle_ = INVALID_HANDLE_VALUE; return value; }
};
void writeAll(HANDLE file, const Bytes& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD count = static_cast<DWORD>(std::min<std::size_t>(bytes.size()-offset,1024*1024));
        DWORD written = 0;
        if (!WriteFile(file,bytes.data()+offset,count,&written,0)) fail("WriteFile");
        if (written == 0) throw Error("WriteFile made no progress");
        offset += written;
    }
}
Bytes utf8Bytes(const std::string& text) { return Bytes(text.begin(),text.end()); }
std::wstring join(const std::wstring& directory, const std::wstring& leaf) {
    return directory + ((directory.back()==L'\\' || directory.back()==L'/') ? L"" : L"\\") + leaf;
}
std::wstring fullPath(const std::wstring& input) {
    if (input.empty() || input.find(L'\0') != std::wstring::npos) throw Error("Empty or invalid local path");
    const DWORD count = GetFullPathNameW(input.c_str(),0,0,0);
    if (count == 0) fail("GetFullPathName");
    std::vector<wchar_t> buffer(static_cast<std::size_t>(count)+1);
    const DWORD length = GetFullPathNameW(input.c_str(),static_cast<DWORD>(buffer.size()),buffer.data(),0);
    if (length == 0 || length >= buffer.size()) fail("GetFullPathName");
    std::wstring result(buffer.data(),length);
    if (result.size()<3 || result[1]!=L':' || (result[2]!=L'\\' && result[2]!=L'/'))
        throw Error("Use a local drive path, not a network or device path");
    while (result.size()>3 && (result.back()==L'\\' || result.back()==L'/')) result.pop_back();
    return result;
}
void ensureDirectory(const std::wstring& path) {
    if (!CreateDirectoryW(path.c_str(),0) && GetLastError()!=ERROR_ALREADY_EXISTS) fail("CreateDirectory");
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes==INVALID_FILE_ATTRIBUTES || !(attributes&FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) throw Error("Artifact directory must be an ordinary local directory");
}
std::string stamp() {
    SYSTEMTIME time; GetSystemTime(&time);
    std::ostringstream text; text << std::setfill('0') << std::setw(4) << time.wYear << '-'
        << std::setw(2) << time.wMonth << '-' << std::setw(2) << time.wDay << 'T'
        << std::setw(2) << time.wHour << ':' << std::setw(2) << time.wMinute << ':'
        << std::setw(2) << time.wSecond << '.' << std::setw(3) << time.wMilliseconds << 'Z';
    return text.str();
}
std::wstring temporaryLeaf() {
    static volatile LONG sequence = 0;
    FILETIME time; GetSystemTimeAsFileTime(&time);
    std::wostringstream text;
    text << L".new-" << std::hex << time.dwHighDateTime << L'-' << time.dwLowDateTime << L'-'
        << GetCurrentProcessId() << L'-' << InterlockedIncrement(&sequence) << L".tmp";
    return text.str();
}
std::wstring checkedLeaf(const std::string& leaf) {
    if (leaf.empty() || leaf.size()>128 || leaf=="." || leaf==".." || leaf.back()=='.') throw Error("Invalid artifact leaf name");
    for (unsigned char character : leaf) {
        if (!((character>='a' && character<='z') || (character>='A' && character<='Z') ||
            (character>='0' && character<='9') || character=='.' || character=='-' || character=='_'))
            throw Error("Artifact names cannot contain paths or special characters");
    }
    std::string stem = leaf.substr(0,leaf.find('.'));
    for (char& character : stem) if (character>='a' && character<='z') character=char(character-'a'+'A');
    if (stem=="CON" || stem=="PRN" || stem=="AUX" || stem=="NUL" ||
        (stem.size()==4 && (stem.substr(0,3)=="COM" || stem.substr(0,3)=="LPT") && stem[3]>='1' && stem[3]<='9'))
        throw Error("Reserved Windows artifact name");
    return std::wstring(leaf.begin(),leaf.end());
}
class LockedBundleDirectory {
    std::vector<std::unique_ptr<Handle>> handles_;
public:
    std::wstring path;
    explicit LockedBundleDirectory(const std::wstring& input) : path(fullPath(input)) {
        std::replace(path.begin(),path.end(),L'/',L'\\');
        if (path.size() >= MAX_PATH || path.find(L':',2) != std::wstring::npos)
            throw Error("Backup directory must be a bounded ordinary local path");
        for (std::size_t end=3; end<=path.size(); ++end) {
            if (end!=path.size() && path[end]!=L'\\') continue;
            const std::wstring component=path.substr(0,end);
            std::unique_ptr<Handle> handle(new Handle(CreateFileW(component.c_str(),FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ|FILE_SHARE_WRITE,0,OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,0)));
            if (handle->get()==INVALID_HANDLE_VALUE) fail("Lock backup directory");
            BY_HANDLE_FILE_INFORMATION info={};
            if (!GetFileInformationByHandle(handle->get(),&info)) fail("Inspect backup directory");
            if (GetFileType(handle->get())!=FILE_TYPE_DISK || !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) ||
                (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))
                throw Error("Backup path contains a non-directory or reparse point");
            // No FILE_SHARE_DELETE: the validated path components cannot be
            // renamed/replaced while the bundle's file handles are being read.
            handles_.push_back(std::move(handle));
        }
    }
};
class LocalBundleReader {
    struct FileId { DWORD volume, high, low; };
    std::vector<FileId> identities_;
    std::vector<std::unique_ptr<Handle>> files_;
public:
    LockedBundleDirectory manifestDirectory;
    LockedBundleDirectory artifactDirectory;
    LocalBundleReader(const std::wstring& manifestParent,const std::wstring& artifactParent)
        : manifestDirectory(manifestParent),artifactDirectory(artifactParent) {}
    Bytes read(const LockedBundleDirectory& directory,const std::string& leaf,std::uint32_t maximum) {
        if (!isSafeBackupArtifactLeaf(leaf)) throw Error("Unsafe backup artifact filename");
        const std::wstring path=join(directory.path,std::wstring(leaf.begin(),leaf.end()));
        if (path.size()>=MAX_PATH) throw Error("Backup artifact path exceeds the reviewed Windows path limit");
        std::unique_ptr<Handle> file(new Handle(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,
            0,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,0)));
        if (file->get()==INVALID_HANDLE_VALUE) fail("Open backup artifact");
        BY_HANDLE_FILE_INFORMATION info={};
        if (!GetFileInformationByHandle(file->get(),&info)) fail("Inspect backup artifact");
        if (GetFileType(file->get())!=FILE_TYPE_DISK ||
            (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))
            throw Error("Backup artifact is not an ordinary disk file");
        const FileId identity={info.dwVolumeSerialNumber,info.nFileIndexHigh,info.nFileIndexLow};
        for (const FileId& previous : identities_)
            if (previous.volume==identity.volume && previous.high==identity.high && previous.low==identity.low)
                throw Error("Backup artifacts must be distinct files, not aliases or hard links to one file");
        const std::uint64_t length=(std::uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
        if (length>maximum) throw Error("Backup artifact exceeds its allowed size");
        Bytes data(static_cast<std::size_t>(length));
        std::size_t offset=0;
        while (offset<data.size()) {
            const DWORD count=static_cast<DWORD>(std::min<std::size_t>(1024*1024,data.size()-offset));
            DWORD read=0;
            if (!ReadFile(file->get(),data.data()+offset,count,&read,0)) fail("Read backup artifact");
            if (!read) throw Error("Backup artifact ended before its declared size");
            offset+=read;
        }
        std::uint8_t extra=0; DWORD read=0;
        if (!ReadFile(file->get(),&extra,1,&read,0)) fail("Check backup artifact end");
        if (read) throw Error("Backup artifact grew during reading");
        identities_.push_back(identity);
        // Retain all source handles without write/delete sharing until all
        // independent artifacts have been read and the core has checked them.
        files_.push_back(std::move(file));
        return data;
    }
};
void atomicNew(const std::wstring& directory, const std::wstring& leaf, const Bytes& bytes) {
    const std::wstring destination = join(directory,leaf);
    std::wstring temporary;
    HANDLE raw = INVALID_HANDLE_VALUE;
    for (unsigned attempt=0; attempt<16; ++attempt) {
        temporary=join(directory,temporaryLeaf());
        raw=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,0,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,0);
        if (raw!=INVALID_HANDLE_VALUE) break;
        if (GetLastError()!=ERROR_FILE_EXISTS && GetLastError()!=ERROR_ALREADY_EXISTS) fail("CreateNew temporary artifact");
    }
    if (raw==INVALID_HANDLE_VALUE) throw Error("Could not allocate a unique temporary artifact");
    try {
        Handle file(raw); raw=INVALID_HANDLE_VALUE;
        writeAll(file.get(),bytes);
        if (!FlushFileBuffers(file.get())) fail("FlushFileBuffers artifact");
        const HANDLE completed=file.release();
        if (!CloseHandle(completed)) fail("CloseHandle artifact");
        // Same-directory publication; omitting REPLACE_EXISTING preserves every prior file.
        if (!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH)) fail("Publish new artifact");
    } catch (...) {
        if (raw!=INVALID_HANDLE_VALUE) CloseHandle(raw);
        DeleteFileW(temporary.c_str()); // Only this invocation's unpublished temporary file.
        throw;
    }
}
class WindowsArtifactStore : public IArtifactStore {
    std::wstring directory_, recoveryDirectory_, markerPath_;
    HANDLE journal_ = INVALID_HANDLE_VALUE, marker_ = INVALID_HANDLE_VALUE;
    bool armed_ = false;
public:
    WindowsArtifactStore(const std::wstring& directory, bool mock) : directory_(fullPath(directory)) {
        if (!CreateDirectoryW(directory_.c_str(),0)) fail("Create fresh run directory");
        try {
            if (mock) recoveryDirectory_=directory_;
            else {
                wchar_t local[MAX_PATH] = {};
                const HRESULT status=SHGetFolderPathW(0,CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,0,SHGFP_TYPE_CURRENT,local);
                if (FAILED(status)) throw Error("Cannot locate LocalAppData for the recovery interlock");
                const std::wstring root=join(fullPath(local),L"MonitorFirmware"); ensureDirectory(root);
                recoveryDirectory_=join(root,L"recovery"); ensureDirectory(recoveryDirectory_);
            }
            journal_=CreateFileW(join(directory_,L"journal.ndjson").c_str(),GENERIC_WRITE | FILE_APPEND_DATA,
                FILE_SHARE_READ,0,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,0);
            if (journal_==INVALID_HANDLE_VALUE) fail("Create journal");
            appendJournal(mock ? "MOCK artifact store created; recovery marker stays in this run" : "Artifact store created");
        } catch (...) { if (journal_!=INVALID_HANDLE_VALUE) { CloseHandle(journal_); journal_=INVALID_HANDLE_VALUE; } throw; }
    }
    ~WindowsArtifactStore() override {
        if (marker_!=INVALID_HANDLE_VALUE) CloseHandle(marker_);
        if (journal_!=INVALID_HANDLE_VALUE) CloseHandle(journal_);
        // An armed marker deliberately survives normal destruction and process failure.
    }
    void saveNew(const std::string& leaf, const Bytes& data) override { atomicNew(directory_,checkedLeaf(leaf),data); }
    void appendJournal(const std::string& event) override {
        if (journal_==INVALID_HANDLE_VALUE) throw Error("Journal is unavailable");
        writeAll(journal_,utf8Bytes("{\"utc\":"+jsonQuote(stamp())+",\"event\":"+jsonQuote(event)+"}\n"));
        if (!FlushFileBuffers(journal_)) fail("FlushFileBuffers journal");
    }
    void armRecovery(const MonitorIdentity& identity) override {
        if (armed_) throw Error("Recovery interlock already armed by this store");
        markerPath_=join(recoveryDirectory_,widenUtf8(detail::recoveryKey(identity))+L".recovery.json");
        marker_=CreateFileW(markerPath_.c_str(),GENERIC_READ | GENERIC_WRITE,FILE_SHARE_READ,0,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,0);
        if (marker_==INVALID_HANDLE_VALUE) {
            if (GetLastError()==ERROR_FILE_EXISTS || GetLastError()==ERROR_ALREADY_EXISTS || GetLastError()==ERROR_SHARING_VIOLATION)
                throw Error("An unresolved recovery marker already blocks this monitor; inspect its saved run before continuing");
            fail("Arm recovery interlock");
        }
        armed_=true;
        std::ostringstream body;
        body << "{\"schema\":\"monitorfw.recovery.v1\",\"utc\":" << jsonQuote(stamp())
             << ",\"runDirectory\":" << jsonQuote(narrowUtf8(directory_))
             << ",\"model\":" << jsonQuote(identity.model) << ",\"serial\":" << jsonQuote(identity.serial)
             << ",\"edidSha256\":" << jsonQuote(identity.edidSha256)
             << ",\"transportLocator\":" << jsonQuote(identity.transportLocator)
             << ",\"serialAmbiguous\":" << (identity.serialAmbiguous ? "true" : "false") << "}\n";
        // A failed or interrupted marker write still leaves a blocking marker.
        writeAll(marker_,utf8Bytes(body.str()));
        if (!FlushFileBuffers(marker_)) fail("FlushFileBuffers recovery interlock");
    }
    void clearRecovery() override {
        if (!armed_ || marker_==INVALID_HANDLE_VALUE) throw Error("This store does not own an armed recovery marker");
        // The exclusive write/delete sharing contract prevents another conforming
        // store from replacing the marker while this handle is held. XP has no
        // SetFileInformationByHandle; release then delete this owned marker only.
        HANDLE owned=marker_; marker_=INVALID_HANDLE_VALUE;
        if (!CloseHandle(owned)) fail("Close recovery interlock");
        if (!DeleteFileW(markerPath_.c_str())) fail("Clear recovery interlock");
        armed_=false;
    }
};
void validateUtf8(const std::string& text) {
    for (std::size_t index=0; index<text.size();) {
        const unsigned char first=static_cast<unsigned char>(text[index++]);
        if (first<0x80) continue;
        unsigned count=0; std::uint32_t point=0, minimum=0;
        if (first>=0xc2 && first<=0xdf) { count=1; point=first&0x1f; minimum=0x80; }
        else if (first>=0xe0 && first<=0xef) { count=2; point=first&0x0f; minimum=0x800; }
        else if (first>=0xf0 && first<=0xf4) { count=3; point=first&7; minimum=0x10000; }
        else throw Error("Invalid UTF-8 leading byte");
        if (count>text.size()-index) throw Error("Truncated UTF-8 sequence");
        for (unsigned part=0; part<count; ++part) {
            const unsigned char next=static_cast<unsigned char>(text[index++]);
            if ((next&0xc0)!=0x80) throw Error("Invalid UTF-8 continuation byte");
            point=(point<<6)|(next&0x3f);
        }
        if (point<minimum || point>0x10ffff || (point>=0xd800 && point<=0xdfff)) throw Error("Invalid UTF-8 code point");
    }
}
} // namespace

std::unique_ptr<IArtifactStore> makeWindowsArtifactStore(const std::wstring& runDirectory, bool mockScope) {
    return std::unique_ptr<IArtifactStore>(new WindowsArtifactStore(runDirectory,mockScope));
}
Bytes readLocalFile(const std::wstring& path, std::uint32_t maximumBytes) {
    const std::wstring canonical=fullPath(path);
    Handle file(CreateFileW(canonical.c_str(),GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,0));
    if (file.get()==INVALID_HANDLE_VALUE) fail("Open input file");
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file.get(),&size)) fail("Get input file size");
    if (size.QuadPart<0 || static_cast<unsigned long long>(size.QuadPart)>maximumBytes) throw Error("Input file exceeds its allowed size");
    Bytes result(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset=0;
    while (offset<result.size()) {
        const DWORD count=static_cast<DWORD>(std::min<std::size_t>(result.size()-offset,1024*1024));
        DWORD actual=0;
        if (!ReadFile(file.get(),result.data()+offset,count,&actual,0)) fail("Read input file");
        if (actual==0) throw Error("Input file ended before its declared size");
        offset+=actual;
    }
    std::uint8_t extra=0; DWORD extraRead=0;
    if (!ReadFile(file.get(),&extra,1,&extraRead,0)) fail("Check input file end");
    if (extraRead!=0) throw Error("Input file grew during reading");
    return result;
}
BackupEvidence loadLocalBackupEvidence(const std::wstring& manifestPath,const std::wstring& artifactDirectory) {
    std::wstring canonical=fullPath(manifestPath);
    std::replace(canonical.begin(),canonical.end(),L'/',L'\\');
    const std::size_t separator=canonical.find_last_of(L'\\');
    if (separator==std::wstring::npos || separator+1>=canonical.size()) throw Error("Invalid backup manifest path");
    const std::wstring parent=canonical.substr(0,separator==2?3:separator);
    const std::string leaf=narrowUtf8(canonical.substr(separator+1));
    const std::shared_ptr<LocalBundleReader> reader(new LocalBundleReader(parent,
        artifactDirectory.empty()?parent:artifactDirectory));
    const Bytes manifest=reader->read(reader->manifestDirectory,leaf,1024*1024);
    return loadBackupEvidence(std::string(manifest.begin(),manifest.end()),
        [reader](const std::string& name,std::uint32_t maximum) {
            return reader->read(reader->artifactDirectory,name,maximum);
        });
}
std::string narrowUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    if (text.size()>INT_MAX) throw Error("Text is too large to encode");
    for (std::size_t index=0; index<text.size(); ++index) {
        const std::uint16_t unit=static_cast<std::uint16_t>(text[index]);
        if (unit>=0xd800 && unit<=0xdbff) {
            if (++index==text.size() || text[index]<0xdc00 || text[index]>0xdfff) throw Error("Invalid UTF-16 surrogate pair");
        } else if (unit>=0xdc00 && unit<=0xdfff) throw Error("Unpaired UTF-16 surrogate");
    }
    const int count=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),0,0,0,0);
    if (count==0) fail("Encode UTF-8");
    std::string result(static_cast<std::size_t>(count),'\0');
    if (WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),&result[0],count,0,0)!=count) fail("Encode UTF-8");
    return result;
}
std::wstring widenUtf8(const std::string& text) {
    if (text.empty()) return std::wstring();
    if (text.size()>INT_MAX) throw Error("Text is too large to decode");
    validateUtf8(text); // Same strict behavior on XP and newer Windows.
    const int count=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),0,0);
    if (count==0) fail("Decode UTF-8");
    std::wstring result(static_cast<std::size_t>(count),L'\0');
    if (MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),&result[0],count)!=count) fail("Decode UTF-8");
    return result;
}
} // namespace monitorfw
