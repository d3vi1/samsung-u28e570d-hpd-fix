#include "monitorfw/broker.hpp"
#include "monitorfw/storage.hpp"
#include "monitorfw/transport.hpp"
#include "mock_flash.hpp"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <ctime>
#include <sstream>
#include <memory>
#include <algorithm>
#include <iomanip>

#ifndef MONITORFW_ENABLE_HARDWARE
#define MONITORFW_ENABLE_HARDWARE 0
#endif
using namespace monitorfw;
namespace {
struct Handle { HANDLE h; explicit Handle(HANDLE value=INVALID_HANDLE_VALUE):h(value){} ~Handle(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);} Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete; };
std::wstring join(const std::wstring& a,const std::wstring& b){return a+L"\\"+b;}
std::string text(const Bytes& b){return std::string(b.begin(),b.end());}
void check(bool condition,const char* message){if(!condition)throw Error(std::string(message)+" (Win32 "+std::to_string(GetLastError())+")");}
void writeRecord(const std::wstring& path,const Bytes& data,bool replace=false){
    GUID guid;check(SUCCEEDED(CoCreateGuid(&guid)),"Cannot create temporary file ID");wchar_t id[40];StringFromGUID2(guid,id,40);
    const std::wstring temporary=path+L"."+id+L".tmp";bool created=false;
    try{
        {Handle file(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,0,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,0));check(file.h!=INVALID_HANDLE_VALUE,"Cannot create broker temporary file");created=true;
        DWORD done=0;check(WriteFile(file.h,data.empty()?"":reinterpret_cast<const char*>(data.data()),DWORD(data.size()),&done,0)!=0 && done==data.size(),"Incomplete broker record write");check(FlushFileBuffers(file.h)!=0,"Cannot flush broker record");}
        check(MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH|(replace?MOVEFILE_REPLACE_EXISTING:0))!=0,"Cannot publish broker record");created=false;
    }catch(...){if(created)DeleteFileW(temporary.c_str());throw;}
}
void writeRecord(const std::wstring& path,const std::string& data,bool replace=false){writeRecord(path,Bytes(data.begin(),data.end()),replace);}
// Retain handles without FILE_SHARE_DELETE on every existing directory in the
// fixed package path. Reparse points and renamed path components are rejected.
struct LockedPath {
    std::vector<std::unique_ptr<Handle>> handles;
    explicit LockedPath(const std::wstring& path){
        check(path.size()>3 && path[1]==L':' && path[2]==L'\\' && path.size()<MAX_PATH-50,"Broker path must be a bounded local absolute path");
        for(std::size_t n=3;n<=path.size();++n){if(n!=path.size() && path[n]!=L'\\')continue;
            std::wstring part=path.substr(0,n);auto file=std::unique_ptr<Handle>(new Handle(CreateFileW(part.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,0,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,0)));
            check(file->h!=INVALID_HANDLE_VALUE,"Cannot lock broker directory");BY_HANDLE_FILE_INFORMATION info={};check(GetFileInformationByHandle(file->h,&info)!=0,"Cannot inspect broker directory");
            check((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0 && !(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"Broker directory is not an ordinary directory");handles.push_back(std::move(file));
        }
    }
};
Bytes inputFile(const std::wstring& path,std::uint32_t maximum){
    Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,0));
    check(file.h!=INVALID_HANDLE_VALUE,"Cannot open broker input");BY_HANDLE_FILE_INFORMATION info={};
    check(GetFileInformationByHandle(file.h,&info)!=0 && GetFileType(file.h)==FILE_TYPE_DISK && !(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),"Broker input is not an ordinary disk file");
    const std::uint64_t size=(std::uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;check(size<=maximum,"Broker input exceeds its size bound");
    Bytes data(static_cast<std::size_t>(size));std::size_t offset=0;
    while(offset<data.size()){DWORD done=0;const DWORD count=DWORD(std::min<std::size_t>(1048576,data.size()-offset));check(ReadFile(file.h,data.data()+offset,count,&done,0)!=0 && done!=0,"Short broker input read");offset+=done;}
    unsigned char extra=0;DWORD done=0;check(ReadFile(file.h,&extra,1,&done,0)!=0 && done==0,"Broker input changed size");return data;
}
std::wstring registeredLocalState(){
    HKEY raw=0;check(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\MonitorFirmware\\WinRTBroker",0,KEY_QUERY_VALUE,&raw)==ERROR_SUCCESS,"Broker registration is missing");
    wchar_t family[256]={};DWORD type=0,size=sizeof(family);const LONG status=RegQueryValueExW(raw,L"PackageFamilyName",0,&type,reinterpret_cast<BYTE*>(family),&size);RegCloseKey(raw);
    check(status==ERROR_SUCCESS && type==REG_SZ && size>=2 && size<=sizeof(family) && family[size/2-1]==0,"Invalid registered package family");
    const std::wstring value(family),prefix=L"MonitorFirmware.WinRT_";
    check(value.size()==prefix.size()+13 && value.compare(0,prefix.size(),prefix)==0,"Unrecognized package identity");
    check(value.substr(prefix.size()).find_first_not_of(L"abcdefghijklmnopqrstuvwxyz0123456789")==std::wstring::npos,"Invalid package publisher ID");
    wchar_t local[MAX_PATH];check(SUCCEEDED(SHGetFolderPathW(0,CSIDL_LOCAL_APPDATA,0,SHGFP_TYPE_CURRENT,local)),"Cannot resolve LocalAppData");
    return std::wstring(local)+L"\\Packages\\"+value+L"\\LocalState";
}
std::string providersJson(){
    std::ostringstream s;s<<'[';bool first=true;for(const auto& p:listProviders()){if(!first)s<<',';first=false;s<<"{\"id\":"<<jsonQuote(p.id)<<",\"name\":"<<jsonQuote(p.name)<<",\"implemented\":"<<(p.implemented?"true":"false")<<",\"reason\":"<<jsonQuote(p.reason)<<'}';}s<<']';return s.str();
}
std::string endpointsJson(const std::vector<DisplayEndpoint>& endpoints){
    std::ostringstream s;s<<'[';bool first=true;for(const auto& endpoint:endpoints){const auto& i=endpoint.identity;if(!first)s<<',';first=false;
    s<<"{\"provider_id\":"<<jsonQuote(endpoint.providerId)<<",\"locator\":"<<jsonQuote(i.transportLocator)<<",\"model\":"<<jsonQuote(i.model)<<",\"manufacturer\":"<<jsonQuote(i.manufacturer)
      <<",\"product_code\":"<<i.productCode<<",\"serial\":"<<jsonQuote(i.serial)<<",\"edid_sha256\":"<<jsonQuote(i.edidSha256)<<",\"ambiguous\":"<<(i.serialAmbiguous?"true":"false")<<",\"ambiguity_reason\":"<<jsonQuote(i.ambiguityReason)<<'}';}s<<']';return s.str();
}
int runRequest(const std::wstring& localState,const std::string& id,bool autoConfirmMock=false){
    if(!validRequestId(id))throw Error("Invalid broker request ID");
    const std::wstring directory=join(join(localState,L"requests"),widenUtf8(id));LockedPath lock(directory);
    Handle claim(CreateFileW(join(directory,L"claimed.lock").c_str(),GENERIC_WRITE,FILE_SHARE_READ,0,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,0));
    check(claim.h!=INVALID_HANDLE_VALUE,"Request was already claimed; replay is not allowed");
    check(FlushFileBuffers(claim.h)!=0,"Cannot persist request claim");
    bool success=false;std::string error,result="null",responseMode="unknown",responseProvider;std::vector<DisplayEndpoint> endpoints;
    try{
        const BrokerRequest request=parseBrokerRequest(text(inputFile(join(directory,L"request.json"),65536)),id,std::uint64_t(std::time(0)));
        responseMode=request.mode;responseProvider=request.providerId;
        const bool mock=request.mode=="mock";
        if(autoConfirmMock && !mock)throw Error("Automatic confirmation is restricted to the mock self-test");
        auto provider=createProvider(request.providerId);if(!provider->info().implemented)throw Error(provider->info().reason);
        if(mock){DisplayEndpoint ep;ep.providerId="nvidia";ep.identity=testing::syntheticIdentity();endpoints.push_back(ep);}
        else {
#if MONITORFW_ENABLE_HARDWARE
            endpoints=provider->enumerate();
#else
            throw Error("Hardware access is disabled in this development broker");
#endif
        }
        if(request.action=="enumerate"){success=true;}
        else{
            auto match=endpoints.end();unsigned matches=0;
            for(auto p=endpoints.begin();p!=endpoints.end();++p)if(p->identity.transportLocator==request.endpointLocator && p->identity.edidSha256==request.edidSha256){match=p;++matches;}
            if(matches!=1)throw Error("Exactly one matching selected endpoint is required");
            Bytes input;BackupEvidence evidence;
            if(request.action=="upload"||request.action=="verify"){
                input=inputFile(join(directory,L"input.bin"),SpiCapacity);
                if(sha256(input)!=request.firmwareSha256)throw Error("Input bytes changed after frontend selection");
                if(request.imageKind==ImageKind::SamsungPackage)inspectPackage(input);
                else {
                    if(input.size()!=SpiCapacity)throw Error("Full SPI input must be exactly 2 MiB");
                    evidence=loadLocalBackupEvidence(join(directory,L"backup-evidence.json"),join(directory,L"evidence"));
                }
            }
            // Serialized, persistent synthetic state makes separate broker mock
            // requests behave like one simulated monitor. Never used in hardware.
            std::unique_ptr<Handle> mockLock;std::unique_ptr<IFlashDevice> device;testing::MockFlash* mockDevice=0;
            if(mock){
                mockLock.reset(new Handle(CreateFileW(join(localState,L"mock-session.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,0,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_DELETE_ON_CLOSE,0)));
                check(mockLock->h!=INVALID_HANDLE_VALUE,"Another mock request is active");Bytes initial=testing::syntheticDump();
                const std::wstring state=join(localState,L"mock-flash.bin");if(GetFileAttributesW(state.c_str())!=INVALID_FILE_ATTRIBUTES)initial=inputFile(state,SpiCapacity);
                mockDevice=new testing::MockFlash(initial,match->identity);device.reset(mockDevice);
            }else{
#if MONITORFW_ENABLE_HARDWARE
                device=makeMstarFlash(provider->open(*match),match->identity);
#endif
            }
            auto store=makeWindowsArtifactStore(join(directory,L"artifacts"),mock);
            ProgressSink progress=[&directory](const Progress& p){std::ostringstream s;s<<"{\"phase\":"<<jsonQuote(p.phase)<<",\"done\":"<<p.done<<",\"total\":"<<p.total<<'}';writeRecord(join(directory,L"progress.json"),s.str(),true);};
            Session session(*device,*store,match->identity,progress);const Backup backup=session.backupTwice();Result operation;
            if(request.action=="download")operation=session.finishBackup();
            else if(request.action=="verify")operation=session.verify(input,request.imageKind,request.imageKind==ImageKind::OwnSpiDump?&evidence:0);
            else{
                // Planning failures happen before mutation and close only the
                // already verified backup session, not a partially written image.
                bool engineStarted=false;
                try{
                    const FirmwarePolicy policy=mock?testing::syntheticPolicy({packageFromDump(backup.image),request.imageKind==ImageKind::SamsungPackage?input:packageFromDump(input)}):reviewedPolicy();
                    const FlashPlan plan=request.imageKind==ImageKind::SamsungPackage?planPackageOverlay(backup,input,policy):planOwnDump(backup,input,evidence,policy);
                    std::ostringstream prompt;prompt<<(mock?"MOCK - synthetic memory only":"Selected physical monitor")<<"\n"<<match->identity.transportLocator<<"\nSerial: "<<match->identity.serial
                    <<"\nChanged sectors: "<<plan.sectors().size()<<"\nTarget SHA256: "<<plan.targetHash()<<"\nPlan SHA256: "<<plan.planHash()<<"\n\nProgram this exact reviewed plan?";
                    const bool confirmed=autoConfirmMock || MessageBoxW(0,widenUtf8(prompt.str()).c_str(),L"Monitor firmware desktop broker",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING)==IDYES;
                    if(confirmed){engineStarted=true;operation=session.execute(plan);}else{operation=session.finishBackup();error="Upload declined before programming";}
                }catch(const std::exception& e){if(engineStarted)throw;operation=session.finishBackup();error=std::string("Planning failed: ")+e.what();}
            }
            if(mockDevice)writeRecord(join(localState,L"mock-flash.bin"),mockDevice->image,true);
            result=resultManifest(operation);success=operation.success && error.empty();if(!operation.error.empty())error+=operation.error;
        }
    }catch(const std::exception& e){success=false;error=e.what();}
    const std::string response="{\"schema\":\"monitorfw.response.v1\",\"request_id\":"+jsonQuote(id)+",\"mode\":"+jsonQuote(responseMode)+",\"provider_id\":"+jsonQuote(responseProvider)+",\"success\":"+(success?"true":"false")+",\"error\":"+jsonQuote(error)+",\"providers\":"+providersJson()+",\"endpoints\":"+endpointsJson(endpoints)+",\"result\":"+result+",\"artifacts_directory\":\"artifacts\"}\n";
    writeRecord(join(directory,L"response.json"),response);return success?0:2;
}
int selfTest(const std::wstring& root){
    // The caller supplies a fresh TEMP directory. No registry, NVAPI or AppContainer
    // deployment is accessed. Tests deliberately exercise the production broker.
    check(CreateDirectoryW(root.c_str(),0)!=0,"Self-test requires a new directory");check(CreateDirectoryW(join(root,L"requests").c_str(),0)!=0,"Cannot create test requests");
    const auto identity=testing::syntheticIdentity();unsigned done=0,replays=0,rejectedBundles=0;
    std::wstring originalArtifacts;
    const Bytes package=testing::syntheticPackage("1000.1",128);
    auto requestCase=[&](const std::string& action,bool ownDump,const std::string& fault){
        std::ostringstream number;number<<std::setfill('0')<<std::setw(12)<<done;
        const std::string id="12345678-1234-4234-8234-"+number.str();
        const auto dir=join(join(root,L"requests"),widenUtf8(id));check(CreateDirectoryW(dir.c_str(),0)!=0,"Cannot create request directory");
        Bytes input=package;
        if(ownDump){
            const auto artifacts=join(dir,L"evidence");check(CreateDirectoryW(artifacts.c_str(),0)!=0,"Cannot create evidence fixture");
            input=inputFile(join(originalArtifacts,L"backup-read-1.bin"),SpiCapacity);
            std::string manifest=text(inputFile(join(originalArtifacts,L"backup.json"),1024*1024));
            unsigned index=0;
            for(const wchar_t* leaf:{L"backup-read-1.bin",L"backup-read-2.bin",L"flash-sfdp.bin"}){
                const std::string suffix=std::to_string(index++);
                if(fault=="missing-"+suffix)continue;
                Bytes bytes=inputFile(join(originalArtifacts,leaf),leaf==std::wstring(L"flash-sfdp.bin")?64:SpiCapacity);
                if(fault=="corrupt-"+suffix)bytes.back()^=1;
                writeRecord(join(artifacts,leaf),bytes);
            }
            if(fault=="duplicate"||fault=="unsafe-leaf"){
                const std::string token="backup-read-2.bin";const auto offset=manifest.find(token);
                check(offset!=std::string::npos,"Manifest fixture token is missing");
                manifest.replace(offset,token.size(),fault=="duplicate"?"BACKUP-READ-1.BIN":"../escape.bin");
            }
            writeRecord(join(dir,L"backup-evidence.json"),manifest);
        }
        if(action=="upload"||action=="verify")writeRecord(join(dir,L"input.bin"),input);
        std::ostringstream request;request<<"{\"schema\":\"monitorfw.request.v1\",\"request_id\":"<<jsonQuote(id)<<",\"created_unix\":"<<std::time(0)
          <<",\"action\":"<<jsonQuote(action)<<",\"mode\":\"mock\",\"provider_id\":\"nvidia\",\"endpoint_locator\":"<<jsonQuote(identity.transportLocator)
          <<",\"expected_edid_sha256\":"<<jsonQuote(identity.edidSha256)<<",\"image_kind\":"<<jsonQuote(ownDump?"spi_full":"samsung_package")
          <<",\"firmware_sha256\":"<<jsonQuote(sha256(input))<<'}';
        Bytes stateBefore;if(!fault.empty())stateBefore=inputFile(join(root,L"mock-flash.bin"),SpiCapacity);
        writeRecord(join(dir,L"request.json"),request.str());const int status=runRequest(root,id,true);
        if(fault.empty()){
            if(status!=0)throw Error("Broker mock operation failed: "+action+(ownDump?" own SPI bundle":" package"));
            if(action=="download")originalArtifacts=join(dir,L"artifacts");
        }else{
            check(status==2,"Invalid backup bundle was accepted");
            const std::string response=text(inputFile(join(dir,L"response.json"),1024*1024));
            check(response.find("\"success\":false")!=std::string::npos && response.find("\"result\":null")!=std::string::npos,
                "Invalid bundle did not fail before the device session");
            check(GetFileAttributesW(join(dir,L"artifacts").c_str())==INVALID_FILE_ATTRIBUTES,"Invalid bundle reached the artifact/device session");
            check(inputFile(join(root,L"mock-flash.bin"),SpiCapacity)==stateBefore,"Invalid bundle changed mock flash state");
            ++rejectedBundles;
        }
        bool replayRejected=false;try{runRequest(root,id,true);}catch(...){replayRejected=true;}
        if(!replayRejected)throw Error("Broker replay was accepted");
        ++replays;++done;
    };
    for(const char* action:{"enumerate","download","upload","verify"})requestCase(action,false,"");
    requestCase("upload",true,"");requestCase("verify",true,"");
    for(const char* fault:{"missing-0","missing-1","missing-2","corrupt-0","corrupt-1","corrupt-2","duplicate","unsafe-leaf"})
        requestCase("upload",true,fault);
    std::ostringstream summary;summary<<"{\"passed\":"<<done<<",\"replaysRejected\":"<<replays<<",\"validOwnBundleOperations\":2,\"invalidBundlesRejected\":"
        <<rejectedBundles<<",\"hardwareAccessed\":false}\n";
    writeRecord(join(root,L"self-test.json"),summary.str());return 0;
}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){
    int count=0;LPWSTR* args=CommandLineToArgvW(GetCommandLineW(),&count);if(!args)return 2;int result=2;
    try{
        if(count==3 && std::wstring(args[1])==L"--protocol")result=runRequest(registeredLocalState(),requestIdFromUri(narrowUtf8(args[2])));
        else if(count==3 && std::wstring(args[1])==L"--self-test")result=selfTest(args[2]);
        else throw Error("Use --protocol with a strict request URI; registration and deployment are explicit separate steps");
    }catch(const std::exception& e){OutputDebugStringW(widenUtf8(e.what()).c_str());result=2;}
    LocalFree(args);return result;
}
