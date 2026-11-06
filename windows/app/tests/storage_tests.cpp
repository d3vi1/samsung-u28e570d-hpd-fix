#include "monitorfw/storage.hpp"
#include "../src/storage_internal.hpp"
#include "mock_flash.hpp"
#include <windows.h>
#include <winioctl.h>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>

using namespace monitorfw;
namespace {
unsigned passed=0, failed=0;
std::wstring root;
std::vector<std::wstring> directories;
void check(bool condition, const char* message) { if (!condition) throw Error(message); }
template<class F> void rejects(F action, const char* message) {
    bool rejected=false; try { action(); } catch (const std::exception&) { rejected=true; }
    check(rejected,message);
}
void test(const char* name, const std::function<void()>& action) {
    try { action(); ++passed; std::cout << "PASS " << name << '\n'; }
    catch (const std::exception& error) { ++failed; std::cout << "FAIL " << name << ": " << error.what() << '\n'; }
}
std::wstring newRun() {
    std::wostringstream leaf; leaf << root << L"\\run-" << directories.size();
    directories.push_back(leaf.str()); return directories.back();
}
struct BundleFixture {
    std::wstring directory;
    Backup backup;
    explicit BundleFixture(bool second=true,bool sfdp=true) : directory(newRun()) {
        backup.monitor=testing::syntheticIdentity(); backup.flash=testing::syntheticFlashIdentity();
        backup.image=testing::syntheticDump(); backup.firstSha256=sha256(backup.image);
        backup.secondSha256=backup.firstSha256; backup.persisted=true;
        auto store=makeWindowsArtifactStore(directory,true);
        store->saveNew("backup-read-1.bin",backup.image);
        if (second) store->saveNew("backup-read-2.bin",backup.image);
        if (sfdp) store->saveNew("flash-sfdp.bin",backup.flash.sfdp);
        const std::string manifest=backupManifest(backup);
        store->saveNew("backup.json",Bytes(manifest.begin(),manifest.end()));
    }
    std::wstring path(const wchar_t* leaf) const { return directory+L"\\"+leaf; }
};
void replaceFixture(const std::wstring& path,const Bytes& bytes) {
    check(path.compare(0,root.size()+1,root+L"\\")==0,"Replacement escaped generated fixture root");
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,0,TRUNCATE_EXISTING,FILE_ATTRIBUTE_NORMAL,0);
    check(file!=INVALID_HANDLE_VALUE,"Cannot open fixture for replacement");
    DWORD written=0;
    const bool success=WriteFile(file,bytes.empty()?"":reinterpret_cast<const char*>(bytes.data()),
        static_cast<DWORD>(bytes.size()),&written,0)!=FALSE && written==bytes.size() && FlushFileBuffers(file)!=FALSE;
    CloseHandle(file); check(success,"Fixture replacement was incomplete");
}
std::string replaced(std::string text,const std::string& before,const std::string& after) {
    const std::size_t offset=text.find(before); check(offset!=std::string::npos,"Fixture token not found");
    text.replace(offset,before.size(),after); return text;
}
void replaceManifest(const BundleFixture& fixture,const std::string& text) {
    replaceFixture(fixture.path(L"backup.json"),Bytes(text.begin(),text.end()));
}
class OwnedJunction {
    std::wstring path_;
public:
    OwnedJunction(const std::wstring& path,const std::wstring& destination) : path_(path) {
        check(path.compare(0,root.size()+1,root+L"\\")==0 && destination.compare(0,root.size()+1,root+L"\\")==0,
            "Junction fixture must remain inside its generated root");
        check(CreateDirectoryW(path.c_str(),0)!=FALSE,"Cannot create junction fixture directory");
        const std::wstring substitute=L"\\??\\"+destination;
        const std::size_t substituteBytes=substitute.size()*sizeof(wchar_t);
        const std::size_t printBytes=destination.size()*sizeof(wchar_t);
        // REPARSE_DATA_BUFFER mount-point layout: tag/length/reserved, four
        // USHORT offsets/lengths, then the two NUL-terminated UTF-16 names.
        Bytes buffer(16+substituteBytes+sizeof(wchar_t)+printBytes+sizeof(wchar_t),0);
        const DWORD tag=IO_REPARSE_TAG_MOUNT_POINT;const WORD length=WORD(buffer.size()-8);
        const WORD substituteLength=WORD(substituteBytes),printOffset=WORD(substituteBytes+sizeof(wchar_t)),printLength=WORD(printBytes);
        std::memcpy(buffer.data(),&tag,4);std::memcpy(buffer.data()+4,&length,2);
        std::memcpy(buffer.data()+10,&substituteLength,2);std::memcpy(buffer.data()+12,&printOffset,2);std::memcpy(buffer.data()+14,&printLength,2);
        std::memcpy(buffer.data()+16,substitute.c_str(),substituteBytes);
        std::memcpy(buffer.data()+16+printOffset,destination.c_str(),printBytes);
        HANDLE directory=CreateFileW(path.c_str(),GENERIC_WRITE,0,0,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,0);
        DWORD returned=0;const bool success=directory!=INVALID_HANDLE_VALUE &&
            DeviceIoControl(directory,FSCTL_SET_REPARSE_POINT,buffer.data(),DWORD(buffer.size()),0,0,&returned,0)!=FALSE;
        if(directory!=INVALID_HANDLE_VALUE)CloseHandle(directory);
        if(!success){RemoveDirectoryW(path.c_str());throw Error("Cannot create owned NTFS junction fixture");}
    }
    ~OwnedJunction(){RemoveDirectoryW(path_.c_str());} // Removes only the link, never traverses its target.
};
unsigned count(const std::wstring& directory, const wchar_t* pattern) {
    WIN32_FIND_DATAW data; HANDLE find=FindFirstFileW((directory+L"\\"+pattern).c_str(),&data);
    if (find==INVALID_HANDLE_VALUE) return 0;
    unsigned result=0;
    do { if (!(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) ++result; } while (FindNextFileW(find,&data));
    FindClose(find); return result;
}
void cleanup() {
    for (const std::wstring& directory : directories) {
        check(directory.compare(0,root.size()+1,root+L"\\")==0,"Cleanup target escaped its generated root");
        WIN32_FIND_DATAW data; HANDLE find=FindFirstFileW((directory+L"\\*").c_str(),&data);
        if (find!=INVALID_HANDLE_VALUE) {
            do {
                if (lstrcmpW(data.cFileName,L".")==0 || lstrcmpW(data.cFileName,L"..")==0) continue;
                check(!(data.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),"Unexpected cleanup child");
                check(DeleteFileW((directory+L"\\"+data.cFileName).c_str())!=FALSE,"Cannot delete own temporary fixture file");
            } while (FindNextFileW(find,&data));
            FindClose(find);
        }
        check(RemoveDirectoryW(directory.c_str())!=FALSE,"Cannot remove own temporary fixture directory");
    }
    check(RemoveDirectoryW(root.c_str())!=FALSE,"Cannot remove own temporary fixture root");
}
}
int main() {
    wchar_t temporary[MAX_PATH]={}; const DWORD length=GetTempPathW(MAX_PATH,temporary);
    if (!length || length>=MAX_PATH) return 2;
    FILETIME time; GetSystemTimeAsFileTime(&time);
    std::wostringstream path; path << temporary << L"monitorfw storage ' \u03a9-" << std::hex
        << time.dwHighDateTime << L'-' << time.dwLowDateTime << L'-' << GetCurrentProcessId();
    root=path.str();
    if (!CreateDirectoryW(root.c_str(),0)) return 2;
    test("atomic new file preserves prior bytes and removes unpublished temporary files",[] {
        const std::wstring directory=newRun(); auto store=makeWindowsArtifactStore(directory,true);
        const Bytes original={0,1,2,0xff}; store->saveNew("backup.bin",original);
        check(readLocalFile(directory+L"\\backup.bin")==original,"Saved bytes differ");
        rejects([&]{store->saveNew("backup.bin",Bytes{9});},"Existing artifact was overwritten");
        check(readLocalFile(directory+L"\\backup.bin")==original,"Overwrite refusal changed prior bytes");
        check(count(directory,L".new-*")==0,"Unpublished temporary file remained");
    });
    test("fresh run directory rejects reuse",[] {
        const std::wstring directory=newRun(); auto store=makeWindowsArtifactStore(directory,true);
        rejects([&]{makeWindowsArtifactStore(directory,true);},"Existing run directory accepted");
    });
    test("artifact leaves reject traversal alternate streams and device names",[] {
        auto store=makeWindowsArtifactStore(newRun(),true);
        for (const char* name : {"../escape","..","sub/file","sub\\file","file:stream","CON","nul.bin","LPT1.txt","trailing.","with space"})
            rejects([&]{store->saveNew(name,Bytes{1});},"Unsafe artifact leaf accepted");
    });
    test("bounded reads permit exact length and reject larger files",[] {
        const std::wstring directory=newRun(); auto store=makeWindowsArtifactStore(directory,true);
        store->saveNew("file.bin",Bytes(17,0x7c));
        check(readLocalFile(directory+L"\\file.bin",17).size()==17,"Exact maximum rejected");
        rejects([&]{readLocalFile(directory+L"\\file.bin",16);},"Oversized file accepted");
        store->saveNew("empty.bin",Bytes()); check(readLocalFile(directory+L"\\empty.bin",0).empty(),"Empty file failed");
    });
    test("strict UTF8 and UTF16 conversion preserves non-ASCII text",[] {
        const std::wstring text=L"Text \u00e9 \u03a9 \u4e2d \U0001f642";
        check(widenUtf8(narrowUtf8(text))==text,"Unicode round trip differs");
        check(widenUtf8("").empty() && narrowUtf8(L"").empty(),"Empty conversion failed");
        for (const std::string& invalid : {std::string("\xc0\x80",2),std::string("\xed\xa0\x80",3),std::string("\xf4\x90\x80\x80",4),std::string("\xe2\x82",2)})
            rejects([&]{widenUtf8(invalid);},"Invalid UTF8 accepted");
        rejects([]{narrowUtf8(std::wstring(1,wchar_t(0xd800)));},"Unpaired high surrogate accepted");
        rejects([]{narrowUtf8(std::wstring(1,wchar_t(0xdc00)));},"Unpaired low surrogate accepted");
    });
    test("owned mock marker blocks rearming and clears only explicitly",[] {
        const std::wstring directory=newRun(); auto store=makeWindowsArtifactStore(directory,true);
        store->armRecovery(testing::syntheticIdentity()); check(count(directory,L"*.recovery.json")==1,"Marker missing");
        rejects([&]{store->armRecovery(testing::syntheticIdentity());},"Repeated arm succeeded");
        store->clearRecovery(); check(count(directory,L"*.recovery.json")==0,"Marker survived explicit clear");
        rejects([&]{store->clearRecovery();},"Unowned clear succeeded");
    });
    test("recovery identity is stable across ports product codes and EDID variants",[] {
        const MonitorIdentity original=testing::syntheticIdentity();
        MonitorIdentity changed=original;
        changed.transportLocator="mock://synthetic/another-port";
        changed.edidSha256=sha256(Bytes{1,2,3});
        for (std::uint16_t product : {std::uint16_t(0x0d6f),std::uint16_t(0x0d70),std::uint16_t(0x0d71)}) {
            changed.productCode=product;
            check(detail::recoveryKey(original)==detail::recoveryKey(changed),"Input change bypassed the same-unit marker key");
        }
        changed.serial="SYNTHETIC-UNIT-B";
        check(detail::recoveryKey(original)!=detail::recoveryKey(changed),"Different unique serials share a marker key");
    });
    test("ambiguous units conservatively share the profile recovery lock",[] {
        MonitorIdentity first=testing::syntheticIdentity(); first.serialAmbiguous=true;
        MonitorIdentity second=first; second.serial="OTHER-GENERIC-SERIAL"; second.productCode=0x0d6f;
        second.edidSha256=sha256(Bytes{8,9}); second.transportLocator="mock://different-port";
        check(detail::recoveryKey(first)==detail::recoveryKey(second),"Ambiguous identity became a per-port lock");
        second.serialAmbiguous=false; second.serial.clear();
        check(detail::recoveryKey(first)==detail::recoveryKey(second),"Empty serial did not use conservative profile lock");
    });
    test("unresolved mock marker survives store destruction",[] {
        const std::wstring directory=newRun();
        { auto store=makeWindowsArtifactStore(directory,true); store->armRecovery(testing::syntheticIdentity()); }
        check(count(directory,L"*.recovery.json")==1,"Destructor removed unresolved marker");
    });
    test("journal persists escaped event and UTC timestamp",[] {
        const std::wstring directory=newRun();
        { auto store=makeWindowsArtifactStore(directory,true); store->appendJournal("quoted \"event\"\nnext"); }
        const Bytes bytes=readLocalFile(directory+L"\\journal.ndjson"); const std::string journal(bytes.begin(),bytes.end());
        check(journal.find("\"utc\":\"")!=std::string::npos && journal.find("Z\"")!=std::string::npos,"UTC timestamp missing");
        check(journal.find("quoted \\\"event\\\"\\nnext")!=std::string::npos,"Journal event escaping differs");
    });
    test("bundle loader reopens both images and actual SFDP before issuing proof",[] {
        BundleFixture f;
        const BackupEvidence evidence=loadLocalBackupEvidence(f.path(L"backup.json"));
        check(evidence.artifactsValidated(),"Bundle did not receive verified artifact proof");
        check(evidence.imageSha256==f.backup.firstSha256 && evidence.sfdpBytes()==f.backup.flash.sfdp,"Loaded bytes or hashes differ");
        check(planOwnDump(f.backup,f.backup.image,evidence,
            testing::syntheticPolicy({packageFromDump(f.backup.image)})).sectors().empty(),"Valid own bundle failed no-op planning");
    });
    test("explicit evidence directory supports broker layout and extra JSON fields",[] {
        BundleFixture f;
        const std::wstring request=newRun(); auto store=makeWindowsArtifactStore(request,true);
        std::string manifest=backupManifest(f.backup); manifest.insert(1,"\"producer_extension\":{\"note\":\"ignored metadata\"},");
        store->saveNew("backup-evidence.json",Bytes(manifest.begin(),manifest.end()));
        const BackupEvidence evidence=loadLocalBackupEvidence(request+L"\\backup-evidence.json",f.directory);
        check(evidence.artifactsValidated(),"Explicit artifact root or extra fields rejected");
        rejects([&]{loadLocalBackupEvidence(request+L"\\backup-evidence.json");},"Missing adjacent artifact files were substituted");
    });
    test("missing second read or SFDP cannot be replaced by manifest hashes",[] {
        BundleFixture noSecond(false,true),noSfdp(true,false);
        rejects([&]{loadLocalBackupEvidence(noSecond.path(L"backup.json"));},"Missing second read accepted");
        rejects([&]{loadLocalBackupEvidence(noSfdp.path(L"backup.json"));},"Missing SFDP accepted");
    });
    test("corrupt bytes in either read or SFDP invalidate the bundle",[] {
        for (unsigned artifact=0; artifact<3; ++artifact) {
            BundleFixture f; Bytes data=artifact==2?f.backup.flash.sfdp:f.backup.image;
            data.back()^=1;
            replaceFixture(f.path(artifact==0?L"backup-read-1.bin":artifact==1?L"backup-read-2.bin":L"flash-sfdp.bin"),data);
            rejects([&]{loadLocalBackupEvidence(f.path(L"backup.json"));},"Changed artifact bytes accepted");
        }
    });
    test("short and oversized evidence artifacts are rejected",[] {
        for (int delta : {-1,1}) {
            BundleFixture f; replaceFixture(f.path(L"backup-read-2.bin"),Bytes(SpiCapacity+delta,0));
            rejects([&]{loadLocalBackupEvidence(f.path(L"backup.json"));},"Incorrect image length accepted");
            BundleFixture s; replaceFixture(s.path(L"flash-sfdp.bin"),Bytes(64+delta,0));
            rejects([&]{loadLocalBackupEvidence(s.path(L"backup.json"));},"Incorrect SFDP length accepted");
        }
    });
    test("unsafe and case-duplicate evidence references are rejected",[] {
        BundleFixture f; const std::string original=backupManifest(f.backup);
        for (const char* leaf : {"../escape.bin","sub/file.bin","file:stream","CON.bin","trailing.","BACKUP-READ-1.BIN"}) {
            replaceManifest(f,replaced(original,"backup-read-2.bin",leaf));
            rejects([&]{loadLocalBackupEvidence(f.path(L"backup.json"));},"Unsafe or duplicate artifact reference accepted");
        }
    });
    test("different leaf names cannot alias one underlying read file",[] {
        BundleFixture f;
        check(DeleteFileW(f.path(L"backup-read-2.bin").c_str())!=FALSE,"Cannot prepare hard-link fixture");
        check(CreateHardLinkW(f.path(L"backup-read-2.bin").c_str(),f.path(L"backup-read-1.bin").c_str(),0)!=FALSE,"Cannot create owned hard-link fixture");
        rejects([&]{loadLocalBackupEvidence(f.path(L"backup.json"));},"Two hard links were accepted as independent files");
    });
    test("concurrent writable source handle prevents bundle validation",[] {
        BundleFixture f;
        HANDLE writer=CreateFileW(f.path(L"backup-read-2.bin").c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
            0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);
        check(writer!=INVALID_HANDLE_VALUE,"Cannot hold fixture writer");
        bool rejected=false; try { loadLocalBackupEvidence(f.path(L"backup.json")); } catch (const std::exception&) { rejected=true; }
        CloseHandle(writer); check(rejected,"Writable source was accepted during bundle validation");
        check(loadLocalBackupEvidence(f.path(L"backup.json")).artifactsValidated(),"Read handles were not released after failed validation");
    });
    test("bundle source path rejects alternate streams and directory artifacts",[] {
        BundleFixture f;
        rejects([&]{loadLocalBackupEvidence(f.path(L"backup.json:stream"));},"Alternate manifest stream accepted");
        check(DeleteFileW(f.path(L"flash-sfdp.bin").c_str())!=FALSE,"Cannot prepare directory fixture");
        check(CreateDirectoryW(f.path(L"flash-sfdp.bin").c_str(),0)!=FALSE,"Cannot create directory artifact fixture");
        bool rejected=false; try { loadLocalBackupEvidence(f.path(L"backup.json")); } catch (const std::exception&) { rejected=true; }
        check(RemoveDirectoryW(f.path(L"flash-sfdp.bin").c_str())!=FALSE,"Cannot remove owned directory artifact fixture");
        check(rejected,"Directory accepted as SFDP file");
    });
    test("bundle directory and ancestor junctions cannot redirect evidence",[] {
        BundleFixture f;
        const std::wstring link=root+L"\\bundle-junction";
        OwnedJunction junction(link,f.directory);
        rejects([&]{loadLocalBackupEvidence(link+L"\\backup.json");},"Junction-backed manifest directory accepted");
        rejects([&]{loadLocalBackupEvidence(f.path(L"backup.json"),link);},"Junction-backed artifact directory accepted");
    });
    try { cleanup(); } catch (const std::exception& error) { ++failed; std::cout << "FAIL temporary cleanup: " << error.what() << '\n'; }
    std::cout << "{\"test\":\"storage_tests\",\"passed\":" << passed << ",\"failed\":" << failed
        << ",\"mockScopeOnly\":true,\"hardwareAccessed\":false,\"uacRequested\":false}\n";
    return failed ? 1 : 0;
}
