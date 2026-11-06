#include "monitorfw/core.hpp"
#include "json_internal.hpp"
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>

namespace monitorfw {
namespace {
void require(bool v,const std::string& message){if(!v)throw Error(message);}
Bytes bytes(const std::string& s){return Bytes(s.begin(),s.end());}
Bytes slice(const Bytes& b,std::size_t p,std::size_t n){require(p<=b.size() && n<=b.size()-p,"Image range outside buffer");return Bytes(b.begin()+p,b.begin()+p+n);}
std::string hex(const Bytes& b){std::ostringstream s;s<<std::hex<<std::setfill('0');for(auto v:b)s<<std::setw(2)<<unsigned(v);return s.str();}
bool hashText(const std::string& s){return s.size()==64 && s.find_first_not_of("0123456789abcdef")==std::string::npos;}
std::string utcNow(){std::time_t now=std::time(0);std::tm* tm=std::gmtime(&now);char out[32]={0};if(tm)std::strftime(out,sizeof(out),"%Y-%m-%dT%H:%M:%SZ",tm);return out;}
void addError(Result& r,const std::string& s){if(!r.error.empty())r.error+="; ";r.error+=s;}
std::string identityJson(const MonitorIdentity& m){
    std::ostringstream s;s<<"{\"manufacturer\":"<<jsonQuote(m.manufacturer)<<",\"product_code\":"<<m.productCode
      <<",\"edid_serial\":"<<jsonQuote(m.serial)<<",\"edid_sha256\":"<<jsonQuote(m.edidSha256)
      <<",\"transport_locator\":"<<jsonQuote(m.transportLocator)<<",\"ambiguous\":"<<(m.serialAmbiguous?"true":"false")
      <<",\"ambiguity_reason\":"<<jsonQuote(m.ambiguityReason)<<"}";return s.str();
}
void validateBackup(const Backup& b){
    require(b.persisted && b.image.size()==SpiCapacity,"A persisted full double-read backup is required");
    require(hashText(b.firstSha256) && b.firstSha256==b.secondSha256 && sha256(b.image)==b.firstSha256,"Backup hashes do not agree");
    require(b.flash.capacity==SpiCapacity && b.flash.sectorSize==SectorSize && b.flash.pageSize==256 && b.flash.sfdp.size()==64,"Unsupported flash geometry or SFDP evidence");
}
void checkPolicy(const Backup& backup,const Bytes& package,const FirmwarePolicy& policy){
    validateBackup(backup);require(backup.monitor.model==policy.model,"Monitor model is outside this firmware policy");
    PackageInfo info=inspectPackage(package);
    require(std::find(policy.permittedPackageHashes.begin(),policy.permittedPackageHashes.end(),info.sha256)!=policy.permittedPackageHashes.end(),"Package hash is not in the reviewed firmware policy");
    require(std::equal(package.begin(),package.begin()+0x10000,backup.image.begin()),"Firmware changes the bootstrap");
    require(std::equal(package.begin()+0x10080,package.begin()+0x20080,backup.image.begin()+0x10080),"Firmware changes the PM51 slot");
    for(std::size_t n=0;n<0x80;++n){
        if((n>=8 && n<16)||(n>=20 && n<24)||(n>=32 && n<36))continue;
        require(package[0x10000+n]==backup.image[0x10000+n],"Firmware changes an invariant header byte");
    }
    if(policy.requireApplicationOnlyChange){
        for(std::size_t n=0x20080;n<0x23880;++n){if(n>=0x211ca && n<0x211ce)continue;
            require(package[n]==backup.image[n],"Firmware changes the uncompressed loader outside its length word");}
    }
}
using detail::Json;
using detail::JsonReader;
}

std::string jsonQuote(const std::string& v){std::ostringstream s;s<<'"';for(unsigned char c:v){switch(c){case '"':s<<"\\\"";break;case '\\':s<<"\\\\";break;case '\n':s<<"\\n";break;case '\r':s<<"\\r";break;case '\t':s<<"\\t";break;default:if(c<32)s<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<unsigned(c)<<std::dec;else s<<char(c);}}s<<'"';return s.str();}
bool MonitorIdentity::operator==(const MonitorIdentity& r)const{return model==r.model && manufacturer==r.manufacturer && serial==r.serial && edidSha256==r.edidSha256 && transportLocator==r.transportLocator && productCode==r.productCode && serialAmbiguous==r.serialAmbiguous;}
bool FlashIdentity::operator==(const FlashIdentity& r)const{return jedec==r.jedec && sfdp==r.sfdp && status==r.status && capacity==r.capacity && sectorSize==r.sectorSize && pageSize==r.pageSize && profile==r.profile;}
FirmwarePolicy reviewedPolicy(){FirmwarePolicy p;p.permittedPackageHashes={
    "60e0b2322236b03673d1bd8b5182b36462824baada9a8ec9bff6c724d30d060b",
    "3d5cda5eb49c6c0bb0378742aa1767becd31d3b3e85a6c0b9c023bd6ada2dac4"};return p;}
const char* codingEepromUnavailableReason(){return "No verified safe read-only path to the complete coding EEPROM. SPI backup and a partial configuration RAM view do not constitute an EEPROM backup. EEPROM read/write is unavailable.";}

std::string backupManifest(const Backup& b){
    std::ostringstream s;s<<"{\"schema\":\"monitorfw.backup.v1\",\"image_kind\":\"spi_full\",\"model\":"<<jsonQuote(b.monitor.model)
      <<",\"created_utc\":"<<jsonQuote(utcNow())<<",\"identity\":"<<identityJson(b.monitor)<<",\"flash\":{\"jedec\":"<<jsonQuote(hex(b.flash.jedec))
      <<",\"sfdp_sha256\":"<<jsonQuote(sha256(b.flash.sfdp))<<",\"sfdp_bytes\":"<<b.flash.sfdp.size()<<",\"sfdp_file\":\"flash-sfdp.bin\",\"programming_chunk_size\":32,\"capacity\":"<<b.flash.capacity<<",\"sector_size\":"<<b.flash.sectorSize<<",\"page_size\":"<<b.flash.pageSize
      <<",\"status_before\":"<<jsonQuote(hex(b.flash.status))<<",\"status_after\":"<<jsonQuote(hex(b.flash.status))<<"},\"reads\":["
      <<"{\"file\":\"backup-read-1.bin\",\"bytes\":"<<b.image.size()<<",\"sha256\":"<<jsonQuote(b.firstSha256)<<"},"
      <<"{\"file\":\"backup-read-2.bin\",\"bytes\":"<<b.image.size()<<",\"sha256\":"<<jsonQuote(b.secondSha256)<<"}],\"reads_equal\":"
      <<(b.firstSha256==b.secondSha256 && hashText(b.firstSha256)?"true":"false")<<",\"coding_eeprom\":{\"available\":false,\"reason\":"<<jsonQuote(codingEepromUnavailableReason())<<"}}\n";return s.str();
}
BackupEvidence parseBackupManifest(const std::string& text){
    Json root=JsonReader(text).parse();require(root.at("schema").text()=="monitorfw.backup.v1" && root.at("image_kind").text()=="spi_full","Not a full SPI backup manifest");
    BackupEvidence e;e.monitor.model=root.at("model").text();const Json& id=root.at("identity");
    e.monitor.manufacturer=id.at("manufacturer").text();const auto product=id.at("product_code").integer();require(product<=65535,"Product code overflow");e.monitor.productCode=std::uint16_t(product);
    e.monitor.serial=id.at("edid_serial").text();e.monitor.edidSha256=id.at("edid_sha256").text();e.monitor.transportLocator=id.at("transport_locator").text();
    auto ambiguous=id.object.find("ambiguous");e.monitor.serialAmbiguous=ambiguous!=id.object.end() && ambiguous->second.flag();
    auto reason=id.object.find("ambiguity_reason");if(reason!=id.object.end())e.monitor.ambiguityReason=reason->second.text();
    require(hashText(e.monitor.edidSha256) && !e.monitor.transportLocator.empty(),"Missing exact monitor binding");
    const Json& flash=root.at("flash");const auto capacity=flash.at("capacity").integer();require(capacity==SpiCapacity,"Unsupported backup capacity");e.capacity=std::uint32_t(capacity);
    require(flash.at("sector_size").integer()==SectorSize && flash.at("page_size").integer()==256,"Unsupported backup geometry");
    e.flashJedecHex=flash.at("jedec").text();e.sfdpSha256=flash.at("sfdp_sha256").text();require(hashText(e.sfdpSha256) && flash.at("sfdp_bytes").integer()==64,"Expected a hash of 64 actual SFDP bytes");
    require(flash.at("status_before").text()==flash.at("status_after").text(),"Backup flash status changed");
    const Json& reads=root.at("reads");require(reads.type==Json::Array && reads.array.size()==2,"Exactly two independent backup reads are required");
    e.imageSha256=reads.array[0].at("sha256").text();
    require(hashText(e.imageSha256) && e.imageSha256==reads.array[1].at("sha256").text(),"Backup read hashes disagree");
    for(const Json& read:reads.array)require(read.at("bytes").integer()==SpiCapacity && !read.at("file").text().empty(),"Incomplete backup read");
    require(reads.array[0].at("file").text()!=reads.array[1].at("file").text(),"Independent read files must be distinct");
    e.readsEqual=root.at("reads_equal").flag();require(e.readsEqual,"Backup reads are not verified equal");return e;
}

FlashPlan::FlashPlan(const Backup& backup,const Bytes& target,ImageKind kind):backup_(backup),target_(target),kind_(kind){
    require(target.size()==SpiCapacity,"Target must be full SPI capacity");targetHash_=sha256(target_);
    for(std::uint32_t address=0;address<SpiCapacity;address+=SectorSize){
        Bytes before=slice(backup_.image,address,SectorSize),after=slice(target_,address,SectorSize);
        if(before!=after){SectorChange change;change.address=address;change.beforeSha256=sha256(before);change.afterSha256=sha256(after);sectors_.push_back(change);}
    }
    auto header=std::find_if(sectors_.begin(),sectors_.end(),[](const SectorChange& s){return s.address==HeaderSector;});
    if(header!=sectors_.end()){SectorChange commit=*header;sectors_.erase(header);sectors_.push_back(commit);}
    std::ostringstream canonical;canonical<<backup_.firstSha256<<'\n'<<targetHash_<<'\n';
    for(const auto& s:sectors_)canonical<<s.address<<':'<<s.beforeSha256<<':'<<s.afterSha256<<'\n';
    planHash_=sha256(bytes(canonical.str()));
}
FlashPlan planPackageOverlay(const Backup& backup,const Bytes& package,const FirmwarePolicy& policy){checkPolicy(backup,package,policy);Bytes target=backup.image;std::copy(package.begin(),package.end(),target.begin());return FlashPlan(backup,target,ImageKind::SamsungPackage);}
FlashPlan planOwnDump(const Backup& backup,const Bytes& dump,const BackupEvidence& e,const FirmwarePolicy& policy){
    validateBackup(backup);require(!backup.monitor.serialAmbiguous && !e.monitor.serialAmbiguous,"Full SPI restore cannot bind an ambiguous EDID identity to one physical unit");
    require(e.monitor==backup.monitor && e.readsEqual && e.capacity==SpiCapacity,"Dump evidence belongs to a different monitor or is incomplete");
    require(dump.size()==SpiCapacity && sha256(dump)==e.imageSha256,"Own dump hash differs from its double-read evidence");
    require(e.flashJedecHex==hex(backup.flash.jedec) && e.sfdpSha256==sha256(backup.flash.sfdp),"Own dump flash identity differs");
    checkPolicy(backup,packageFromDump(dump),policy);
    require(std::equal(dump.begin()+PackageLimit,dump.end(),backup.image.begin()+PackageLimit),"Own dump changes the unclassified per-unit tail; this firmware tool does not restore it");
    return FlashPlan(backup,dump,ImageKind::OwnSpiDump);
}
std::string planManifest(const FlashPlan& p){
    std::ostringstream s;s<<"{\"schema\":\"monitorfw.plan.v1\",\"image_kind\":"<<jsonQuote(p.imageKind()==ImageKind::SamsungPackage?"samsung_package":"spi_full")
    <<",\"source_sha256\":"<<jsonQuote(p.backup().firstSha256)<<",\"target_sha256\":"<<jsonQuote(p.targetHash())<<",\"plan_sha256\":"<<jsonQuote(p.planHash())<<",\"header_last\":true,\"sectors\":[";
    bool first=true;for(const auto& block:p.sectors()){if(!first)s<<',';first=false;s<<"{\"address\":"<<block.address<<",\"before_sha256\":"<<jsonQuote(block.beforeSha256)<<",\"after_sha256\":"<<jsonQuote(block.afterSha256)<<"}";}s<<"]}\n";return s.str();
}
std::string resultManifest(const Result& r){
    std::ostringstream s;s<<"{\"schema\":\"monitorfw.result.v1\",\"success\":"<<(r.success?"true":"false")<<",\"backup_verified\":"<<(r.backupVerified?"true":"false")
    <<",\"target_verified\":"<<(r.targetVerified?"true":"false")<<",\"rollback_attempted\":"<<(r.rollbackAttempted?"true":"false")<<",\"rollback_verified\":"<<(r.rollbackVerified?"true":"false")
    <<",\"reset_attempted\":"<<(r.resetAttempted?"true":"false")<<",\"reset_succeeded\":"<<(r.resetSucceeded?"true":"false")<<",\"recovery_required\":"<<(r.recoveryRequired?"true":"false")
    <<",\"source_sha256\":"<<jsonQuote(r.sourceSha256)<<",\"target_sha256\":"<<jsonQuote(r.targetSha256)<<",\"plan_sha256\":"<<jsonQuote(r.planSha256)<<",\"error\":"<<jsonQuote(r.error)<<",\"touched_sectors\":[";
    for(std::size_t i=0;i<r.touchedSectors.size();++i){if(i)s<<',';s<<r.touchedSectors[i];}s<<"]}\n";return s.str();
}

Session::Session(IFlashDevice& d,IArtifactStore& s,const MonitorIdentity& expected,ProgressSink progress,CancelCheck cancel):device_(d),store_(s),expected_(expected),progress_(progress),cancel_(cancel){}
Session::~Session(){}
void Session::log(const std::string& event){try{store_.appendJournal(event);}catch(...){/* Logging cannot interrupt rollback or cause a reset. */}}
Bytes Session::readFull(const std::string& phase,bool allowCancel){
    Bytes image;image.reserve(SpiCapacity);
    try{
        for(std::uint32_t address=0;address<SpiCapacity;address+=256){
            if(allowCancel && cancel_ && cancel_())throw Error("Cancelled before mutation; incomplete read is not a verified backup");
            Bytes block=device_.read(address,256);require(block.size()==256,"Short flash read");image.insert(image.end(),block.begin(),block.end());
            if(progress_ && (image.size()%65536==0 || image.size()==SpiCapacity)){Progress p;p.phase=phase;p.done=std::uint32_t(image.size());p.total=SpiCapacity;try{progress_(p);}catch(...){log("Progress callback failed; device verification continues");}}
        }
    }catch(...){try{store_.saveNew(phase+"-partial.bin",image);}catch(...){}throw;}
    return image;
}
Bytes Session::readPair(const std::string& phase,bool allowCancel,bool persist){
    Bytes first=readFull(phase+"-read-1",allowCancel);if(persist)store_.saveNew(phase+"-read-1.bin",first);
    Bytes second=readFull(phase+"-read-2",allowCancel);if(persist)store_.saveNew(phase+"-read-2.bin",second);
    require(first==second,"Independent full reads disagree: "+phase);return first;
}
void Session::verifyIdentityAndStatus(const FlashIdentity& expected){require(device_.flashIdentity()==expected,"Flash identity or status changed");}
Backup Session::backupTwice(){
    try {
    require(!entered_ && !armed_ && !finished_,"Session already used");
    require(!expected_.model.empty() && !expected_.manufacturer.empty() && hashText(expected_.edidSha256) && !expected_.transportLocator.empty(),"Exact monitor identity and endpoint are required");
    require(device_.monitorIdentity()==expected_,"Selected monitor identity changed before ISP");
    store_.armRecovery(expected_);armed_=true;entered_=true;device_.enterIsp();
    backup_.monitor=expected_;backup_.flash=device_.flashIdentity();
    verifyIdentityAndStatus(backup_.flash);
    require(backup_.flash.capacity==SpiCapacity && backup_.flash.sectorSize==SectorSize && backup_.flash.pageSize==256 && backup_.flash.sfdp.size()==64,"Unsupported flash geometry or incomplete SFDP evidence");
    store_.saveNew("flash-sfdp.bin",backup_.flash.sfdp);
    backup_.image=readFull("backup-read-1",true);backup_.firstSha256=sha256(backup_.image);store_.saveNew("backup-read-1.bin",backup_.image);
    Bytes second=readFull("backup-read-2",true);backup_.secondSha256=sha256(second);store_.saveNew("backup-read-2.bin",second);
    if(backup_.image!=second){store_.saveNew("backup-invalid.json",bytes(backupManifest(backup_)));throw Error("Independent backup reads differ; both files were retained, backup is invalid");}
    verifyIdentityAndStatus(backup_.flash);store_.saveNew("backup.json",bytes(backupManifest(backup_)));backup_.persisted=true;log("Double-read full SPI backup persisted and verified");return backup_;
    } catch(const std::exception& error) {
        Result failure;failure.error=error.what();failure.recoveryRequired=entered_ || armed_;failure.sourceSha256=backup_.firstSha256;
        try{store_.saveNew("failure.json",bytes(resultManifest(failure)));}catch(...){log("Failure report could not be persisted");}
        throw;
    }
}
void Session::finishReset(Result& result){
    require(result.backupVerified && (result.targetVerified || result.rollbackVerified),"Reset withheld: no final verified image");
    verifyIdentityAndStatus(backup_.flash);
    store_.saveNew("verified-state.json",bytes(resultManifest(result)));
    try{device_.disableWrites();}catch(const std::exception& e){addError(result,std::string("Write-disable failed: ")+e.what());result.success=false;}
    result.resetAttempted=true;
    try{device_.resetVerified();result.resetSucceeded=true;store_.clearRecovery();armed_=false;result.recoveryRequired=false;}
    catch(const std::exception& e){addError(result,std::string("Reset/interlock cleanup failed: ")+e.what());result.success=false;}
}
void Session::saveResult(Result& result){try{store_.saveNew("result.json",bytes(resultManifest(result)));}catch(const std::exception& e){addError(result,std::string("Result persistence failed: ")+e.what());result.success=false;log(result.error);}finished_=true;}
Result Session::finishBackup(){
    Result r;r.sourceSha256=backup_.firstSha256;
    try{require(!finished_,"Session already finished");validateBackup(backup_);verifyIdentityAndStatus(backup_.flash);r.backupVerified=true;r.targetVerified=true;r.targetSha256=backup_.firstSha256;r.success=true;finishReset(r);}
    catch(const std::exception& e){r.success=false;addError(r,e.what());}saveResult(r);return r;
}
Result Session::verify(const Bytes& input,ImageKind kind,const BackupEvidence* evidence){
    Result r;r.sourceSha256=backup_.firstSha256;
    try{
        require(!finished_,"Session already finished");validateBackup(backup_);r.backupVerified=true;
        Bytes target;
        if(kind==ImageKind::SamsungPackage){inspectPackage(input);target=backup_.image;std::copy(input.begin(),input.end(),target.begin());}
        else{require(input.size()==SpiCapacity,"Verification requires a full 2 MiB SPI dump");require(evidence && evidence->readsEqual && !expected_.serialAmbiguous && evidence->monitor==expected_ && evidence->imageSha256==sha256(input),"Full dump verification requires matching unambiguous own-unit evidence");target=input;}
        Bytes actual=readPair("verify",true,true);require(actual==target,"Firmware verification mismatch");verifyIdentityAndStatus(backup_.flash);
        r.targetSha256=sha256(target);r.targetVerified=true;r.success=true;finishReset(r);
    }catch(const std::exception& e){r.success=false;addError(r,e.what());}saveResult(r);return r;
}
void Session::writeSector(std::uint32_t address,const Bytes& image,std::uint32_t chunk){
    require(address%SectorSize==0 && address<=SpiCapacity-SectorSize && (chunk==32 || chunk==256),"Invalid sector/page plan");
    for(unsigned attempt=0;attempt<2;++attempt){
        try{
            device_.waitReady();device_.eraseSector(address);device_.waitReady();
            const Bytes erased=device_.read(address,SectorSize);require(erased.size()==SectorSize && std::all_of(erased.begin(),erased.end(),[](std::uint8_t v){return v==0xff;}),"Full sector erase check failed");
            for(std::uint32_t offset=0;offset<SectorSize;offset+=chunk){Bytes page=slice(image,address+offset,chunk);if(std::all_of(page.begin(),page.end(),[](std::uint8_t v){return v==0xff;}))continue;device_.program(address+offset,page);device_.waitReady();}
            require(device_.read(address,SectorSize)==slice(image,address,SectorSize),"Sector read-back differs");return;
        }catch(const std::exception& e){log(std::string("Sector attempt failed: ")+e.what());if(attempt==1)throw;}
    }
}
Result Session::execute(const FlashPlan& plan){
    Result r;r.sourceSha256=backup_.firstSha256;r.targetSha256=plan.targetHash();r.planSha256=plan.planHash();
    try{
        require(!finished_,"Session already finished");validateBackup(backup_);
        require(plan.backup().monitor==expected_ && plan.backup().flash==backup_.flash && plan.backup().image==backup_.image,"Plan belongs to a different session backup");
        require(sha256(plan.target())==plan.targetHash(),"Target changed after planning");r.backupVerified=true;
        const Bytes source=readPair("preflight",true,true);require(source==backup_.image,"Full preflight source differs from the saved backup");verifyIdentityAndStatus(backup_.flash);
        store_.saveNew("plan.json",bytes(planManifest(plan)));store_.saveNew("target-full.bin",plan.target());
        // Once a sector is touched, cancellation is deferred until recovery or
        // complete verification. Never terminate the worker thread to cancel.
        for(std::size_t i=0;i<plan.sectors().size();++i){
            const auto& block=plan.sectors()[i];r.touchedSectors.push_back(block.address);
            writeSector(block.address,plan.target(),32);
            require(sha256(device_.read(block.address,SectorSize))==block.afterSha256,"Sector hash differs from immutable plan");
            if(progress_){Progress p;p.phase="program-and-verify-sectors";p.done=std::uint32_t(i+1);p.total=std::uint32_t(plan.sectors().size());try{progress_(p);}catch(...){log("Progress callback failed after mutation");}}
        }
        device_.disableWrites();const Bytes final=readPair("final",false,true);require(final==plan.target(),"Final full image verification mismatch");verifyIdentityAndStatus(backup_.flash);
        r.targetVerified=true;r.success=true;finishReset(r);
    }catch(const std::exception& e){
        r.success=false;addError(r,e.what());log(r.error);
        if(!r.touchedSectors.empty() && !r.resetAttempted){
            r.rollbackAttempted=true;r.targetVerified=false;
            for(auto address:r.touchedSectors){try{writeSector(address,backup_.image,32);}catch(const std::exception& restore){addError(r,std::string("Rollback sector failure: ")+restore.what());}}
            try{
                device_.disableWrites();const Bytes restored=readPair("rollback",false,true);require(restored==backup_.image,"Rollback full source verification mismatch");verifyIdentityAndStatus(backup_.flash);r.rollbackVerified=true;finishReset(r);
            }catch(const std::exception& restore){addError(r,std::string("Rollback/reset not verified: ")+restore.what());}
        }
    }
    saveResult(r);return r;
}
} // namespace monitorfw
