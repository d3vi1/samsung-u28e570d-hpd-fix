#include "monitorfw/storage.hpp"
#include "../src/storage_internal.hpp"
#include "mock_flash.hpp"
#include <windows.h>
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
    try { cleanup(); } catch (const std::exception& error) { ++failed; std::cout << "FAIL temporary cleanup: " << error.what() << '\n'; }
    std::cout << "{\"test\":\"storage_tests\",\"passed\":" << passed << ",\"failed\":" << failed
        << ",\"mockScopeOnly\":true,\"hardwareAccessed\":false,\"uacRequested\":false}\n";
    return failed ? 1 : 0;
}
