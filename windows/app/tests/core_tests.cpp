#include "mock_flash.hpp"
#include <iostream>
#include <sstream>

using namespace monitorfw;
using namespace monitorfw::testing;
namespace {
unsigned passed=0,failed=0;
void check(bool value,const std::string& message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F action,const std::string& message) {
    bool rejected=false; try { action(); } catch (const std::exception&) { rejected=true; }
    check(rejected,message);
}
template<class F> void test(const char* name,F action) {
    try { action(); ++passed; std::cout<<"PASS "<<name<<"\n"; }
    catch (const std::exception& error) { ++failed; std::cerr<<"FAIL "<<name<<": "<<error.what()<<"\n"; }
}
Backup fixtureBackup(const Bytes& image,MonitorIdentity identity=syntheticIdentity()) {
    Backup backup; backup.monitor=identity; backup.flash=syntheticFlashIdentity(); backup.image=image;
    backup.firstSha256=backup.secondSha256=sha256(image); backup.persisted=true; return backup;
}
BackupEvidence evidenceFor(const Bytes& image,MonitorIdentity identity=syntheticIdentity()) {
    return parseBackupManifest(backupManifest(fixtureBackup(image,identity)));
}
unsigned fullPasses(const MockFlash& device) {
    unsigned count=0; std::uint32_t next=0; bool active=false;
    for (const auto& read:device.reads) {
        if (read.address==0) { next=0; active=true; }
        if (!active) continue;
        if (read.address!=next || read.length==0 || read.length>SpiCapacity-next) { active=false; continue; }
        next+=read.length;
        if (next==SpiCapacity) { ++count; active=false; }
    }
    return count;
}
unsigned storedFullImages(const MemoryStore& store) {
    unsigned count=0; for (const auto& file:store.files) if (file.second.size()==SpiCapacity) ++count; return count;
}
bool hasValidBackupManifest(const MemoryStore& store) {
    for (const auto& file:store.files) {
        if (file.second.size()>65536) continue;
        try {
            const auto evidence=parseBackupManifest(std::string(file.second.begin(),file.second.end()));
            if (evidence.readsEqual && evidence.capacity==SpiCapacity) return true;
        } catch (const std::exception&) {}
    }
    return false;
}
Bytes repackRaw(const Bytes& source,const Bytes& raw,std::uint32_t expanded) {
    const auto end=std::uint32_t(0x23880+raw.size()+6), final=((end+7U)&~7U)+8U;
    Bytes result(final+4,0); std::copy(source.begin(),source.begin()+0x23880,result.begin());
    fixturePutLe32(result,0x10008,0x200000+end-0x20080);
    fixturePutLe32(result,0x1000c,end); fixturePutLe32(result,0x10020,final);
    fixturePutLe32(result,0x211ca,end-0x20080);
    std::copy(raw.begin(),raw.end(),result.begin()+0x23880);
    fixturePutLe32(result,end-6,expanded);
    const std::uint8_t marker[]={0x55,0xaa,0xab,0xcd,0,0};
    std::copy(marker,marker+6,result.begin()+final-8); repairSyntheticIntegrity(result); return result;
}
Bytes rawStream(const Bytes& package) {
    return Bytes(package.begin()+0x23880,package.begin()+fixtureLe32(package,0x1000c)-6);
}
}

int main() {
    const Bytes originalPackage=syntheticPackage("1000.0");
    const Bytes candidate=syntheticPackage("1001.0",6000);
    const Bytes source=syntheticDump(originalPackage);
    const auto policy=syntheticPolicy({originalPackage,candidate});

    test("independent CRC and SHA256 known vectors",[&] {
        const std::string input="123456789"; Bytes bytes(input.begin(),input.end());
        check(fixtureCrc32(bytes,0,bytes.size())==0x89a1897fU,"CRC32 vector");
        check(fixtureCrc16(bytes,0,bytes.size(),false)==0xfee8U,"CRC16 vector");
        check(sha256(Bytes())=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","Empty SHA256");
        check(sha256(Bytes{'a','b','c'})=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","ABC SHA256");
    });
    test("synthetic stored-DEFLATE packages and explicit dump extraction",[&] {
        const auto original=inspectPackage(originalPackage), next=inspectPackage(candidate);
        check(original.version=="m-E5728UMDB-1000.0" && next.version=="m-E5728UMDB-1001.0","Version extraction");
        check(next.expandedLength==original.expandedLength+6000,"Expanded length");
        check(packageFromDump(source)==originalPackage,"Explicit package extraction");
        rejects([&]{inspectPackage(source);},"A full dump was accepted as a package");
        rejects([&]{packageFromDump(originalPackage);},"A package was accepted as a full dump");
    });
    test("all integrity layers reject independent mutations",[&] {
        const std::size_t positions[]={0x10014,fixtureLe32(candidate,0x1000c)-2,
            fixtureLe32(candidate,0x10020)-2,fixtureLe32(candidate,0x10020),0x23885};
        for (const auto position:positions) {
            auto bad=candidate; bad[position]^=1;
            rejects([&]{inspectPackage(bad);},"Corrupted integrity layer accepted");
        }
    });
    test("package bounds overflow length and exact end are enforced",[&] {
        auto bad=candidate; fixturePutLe32(bad,0x1000c,0xffffffffU);
        rejects([&]{inspectPackage(bad);},"Overflowed mainEnd accepted");
        bad=candidate; fixturePutLe32(bad,0x10020,0xffffffffU);
        rejects([&]{inspectPackage(bad);},"Overflowed CRC offset accepted");
        bad=candidate; bad.pop_back(); rejects([&]{inspectPackage(bad);},"Truncation accepted");
        bad=candidate; bad.push_back(0); rejects([&]{inspectPackage(bad);},"Trailing package byte accepted");
        rejects([&]{inspectPackage(Bytes(PackageLimit+1,0));},"Oversized package accepted");
    });
    test("CRC-valid loader magic lengths and marker damage are rejected",[&] {
        const std::size_t positions[]={0x211c7,0x211ca,0x211ce,fixtureLe32(candidate,0x10020)-8};
        for (const auto position:positions) {
            auto bad=candidate; bad[position]^=1; repairSyntheticIntegrity(bad);
            rejects([&]{inspectPackage(bad);},"CRC-valid structural corruption accepted");
        }
        auto bad=candidate; fixturePutLe32(bad,0x10008,0x200001); repairSyntheticIntegrity(bad);
        rejects([&]{inspectPackage(bad);},"Bad load bounds accepted");
    });
    test("CRC-valid DEFLATE EOS truncation excess input and length are rejected",[&] {
        const auto expanded=fixtureLe32(candidate,fixtureLe32(candidate,0x1000c)-6);
        auto raw=rawStream(candidate); raw.push_back(0);
        auto bad=repackRaw(candidate,raw,expanded); rejects([&]{inspectPackage(bad);},"Unused DEFLATE byte accepted");
        raw=rawStream(candidate); raw.pop_back(); bad=repackRaw(candidate,raw,expanded);
        rejects([&]{inspectPackage(bad);},"Truncated DEFLATE accepted");
        raw=rawStream(candidate); raw[0]=7; bad=repackRaw(candidate,raw,expanded);
        rejects([&]{inspectPackage(bad);},"Reserved DEFLATE block accepted");
        raw=rawStream(candidate);
        for (auto length:{0U,expanded-1,expanded+1,0x800001U}) {
            bad=repackRaw(candidate,raw,length); rejects([&]{inspectPackage(bad);},"Invalid expanded bound accepted");
        }
    });
    test("package overlay preserves boot PM51 and every trailing byte",[&] {
        auto backup=fixtureBackup(source); backup.image[0x190000]=0x5a;
        backup.firstSha256=backup.secondSha256=sha256(backup.image);
        const auto plan=planPackageOverlay(backup,candidate,policy);
        check(std::equal(candidate.begin(),candidate.end(),plan.target().begin()),"Candidate prefix");
        check(std::equal(backup.image.begin()+candidate.size(),backup.image.end(),plan.target().begin()+candidate.size()),"Tail was changed");
        check(std::equal(source.begin(),source.begin()+0x10000,plan.target().begin()),"Bootstrap changed");
        check(std::equal(source.begin()+0x10080,source.begin()+0x20080,plan.target().begin()+0x10080),"PM51 changed");
        check(!plan.sectors().empty() && plan.sectors().back().address==HeaderSector,"Header not last");
        check(plan.sectors().size()==5,"Fixture changes header, loader length and three payload sectors");
        const auto originalSnapshot=backup.image;
        backup.image.assign(SpiCapacity,0);
        check(plan.backup().image==originalSnapshot,"Plan shares mutable backup storage");
    });
    test("format validity cannot override package allowlist or protected bytes",[&] {
        const auto backup=fixtureBackup(source);
        FirmwarePolicy deny=policy; deny.permittedPackageHashes.clear();
        rejects([&]{planPackageOverlay(backup,candidate,deny);},"Unapproved package accepted");
        for (auto offset:{0U,0x10080U,0x18000U,0x20000U,0x20080U}) {
            auto changed=candidate; changed[offset]^=1; repairSyntheticIntegrity(changed);
            const auto permit=syntheticPolicy({changed});
            rejects([&]{planPackageOverlay(backup,changed,permit);},"Protected bootstrap/PM51/loader byte accepted");
        }
    });
    test("backup manifests round-trip escaped fields and reject malformed JSON",[&] {
        auto identity=syntheticIdentity("SYNTHETIC-\"QUOTED\\UNIT");
        const auto manifest=backupManifest(fixtureBackup(source,identity));
        const auto parsed=parseBackupManifest(manifest);
        check(parsed.monitor==identity && parsed.imageSha256==sha256(source) && parsed.readsEqual,"Manifest evidence changed");
        rejects([&]{parseBackupManifest(manifest+" trailing");},"Trailing JSON accepted");
        const auto last=manifest.find_last_not_of(" \r\n\t");
        rejects([&]{parseBackupManifest(manifest.substr(0,last));},"Truncated JSON accepted");
        auto duplicate=manifest; duplicate.insert(1,"\"schema\":\"duplicate\",");
        rejects([&]{parseBackupManifest(duplicate);},"Duplicate JSON member accepted");
    });
    test("own dump requires matching identity verified evidence and unambiguous serial",[&] {
        const auto backup=fixtureBackup(source); const auto target=syntheticDump(candidate);
        auto evidence=evidenceFor(target);
        check(planOwnDump(backup,target,evidence,policy).target()==target,"Own firmware dump not accepted");
        evidence.monitor.serial="SYNTHETIC-UNIT-B";
        rejects([&]{planOwnDump(backup,target,evidence,policy);},"Cross-unit restore accepted");
        evidence=evidenceFor(target); evidence.readsEqual=false;
        rejects([&]{planOwnDump(backup,target,evidence,policy);},"Unverified evidence accepted");
        evidence=evidenceFor(target); evidence.imageSha256=sha256(source);
        rejects([&]{planOwnDump(backup,target,evidence,policy);},"Wrong full dump hash accepted");
        auto ambiguous=backup; ambiguous.monitor.serialAmbiguous=true; ambiguous.monitor.ambiguityReason="Synthetic duplicate serial";
        evidence=evidenceFor(target,ambiguous.monitor);
        rejects([&]{planOwnDump(ambiguous,target,evidence,policy);},"Ambiguous identity accepted");
    });
    test("own dump cannot alter unknown unit-data tail or protected boot",[&] {
        const auto backup=fixtureBackup(source);
        for (auto offset:{0U,0x10080U,0x90000U,0x1fffffU}) {
            auto target=syntheticDump(candidate); target[offset]^=1;
            if (offset<PackageLimit) {
                Bytes package(target.begin(),target.begin()+fixtureLe32(target,0x10020)+4);
                repairSyntheticIntegrity(package); std::copy(package.begin(),package.end(),target.begin());
            }
            const auto evidence=evidenceFor(target);
            rejects([&]{planOwnDump(backup,target,evidence,policy);},"Protected own-dump byte accepted");
        }
    });
    test("backup uses two complete independent reads and persists both",[&] {
        MockFlash device(source); MemoryStore store;
        Session session(device,store,device.identity); const auto backup=session.backupTwice();
        check(fullPasses(device)==2 && backup.image==source && backup.persisted,"Double backup incomplete");
        check(storedFullImages(store)>=2 && hasValidBackupManifest(store),"Backup evidence not persisted");
        check(device.eraseCalls==0 && device.programCalls==0 && device.resetCalls==0,"Backup caused mutation/reset");
        const auto result=session.finishBackup();
        check(result.success && result.backupVerified && result.resetSucceeded && device.resetCalls==1,"Verified backup exit");
        check(!store.recoveryArmed,"Successful backup retained recovery interlock");
    });
    test("mismatching backup preserves both complete reads and no valid manifest",[&] {
        MockFlash device(source); MemoryStore store; device.invertReadPass=2;
        Session session(device,store,device.identity);
        rejects([&]{session.backupTwice();},"Different second backup accepted");
        check(fullPasses(device)==2 && storedFullImages(store)>=2,"Mismatching full reads were discarded");
        check(!hasValidBackupManifest(store),"Mismatch emitted valid backup manifest");
        check(device.eraseCalls==0 && device.programCalls==0 && device.resetCalls==0 && store.recoveryArmed,"Mismatch allowed mutation/reset");
    });
    test("single corrupt byte in second backup is rejected",[&] {
        MockFlash device(source); MemoryStore store;
        device.onRead=[](MockFlash& d,std::uint32_t address,std::uint32_t) {
            if (d.readPass==2 && address==0) d.corruptReadCall=d.readCalls;
        };
        Session session(device,store,device.identity);
        rejects([&]{session.backupTwice();},"One-byte mismatch accepted");
        check(device.eraseCalls==0 && device.resetCalls==0 && storedFullImages(store)>=2,"One-byte mismatch evidence/reset");
    });
    test("short reads read errors and changed status prevent backup reset",[&] {
        for (unsigned mode=0; mode<3; ++mode) {
            MockFlash device(source); MemoryStore store;
            if (mode==0) device.shortReadCall=1;
            if (mode==1) device.failReadCall=2;
            if (mode==2) device.onFlashIdentity=[](MockFlash& d){if(d.flashIdentityCalls>=2)d.flash.status[0]=0x04;};
            Session session(device,store,device.identity);
            rejects([&]{session.backupTwice();},"Invalid backup accepted");
            check(device.eraseCalls==0 && device.programCalls==0 && device.resetCalls==0,"Invalid backup caused mutation/reset");
        }
    });
    test("backup persistence failure holds reset and writes no flash",[&] {
        MockFlash device(source); MemoryStore store; store.failSaveCall=1;
        Session session(device,store,device.identity);
        rejects([&]{session.backupTwice();},"Unpersisted backup accepted");
        check(device.eraseCalls==0 && device.programCalls==0 && device.resetCalls==0,"Persistence failure allowed flash mutation/reset");
    });
    test("recovery interlock and monitor mismatch block ISP entry",[&] {
        MockFlash device(source); MemoryStore store; store.recoveryArmed=true;
        Session session(device,store,device.identity);
        rejects([&]{session.backupTwice();},"Existing recovery interlock ignored");
        check(device.enterCalls==0 && device.readCalls==0,"Interlock allowed device access");
        MockFlash other(source); MemoryStore otherStore; auto expected=other.identity; expected.serial="SYNTHETIC-OTHER";
        Session second(other,otherStore,expected);
        rejects([&]{second.backupTwice();},"Different monitor accepted");
        check(other.enterCalls==0 && other.eraseCalls==0,"Wrong monitor entered ISP");
    });
    test("session destruction never resets or implicitly writes",[&] {
        MockFlash device(source); MemoryStore store;
        { Session session(device,store,device.identity); session.backupTwice(); }
        check(device.resetCalls==0 && device.eraseCalls==0 && device.entered && store.recoveryArmed,"Destructor performed hardware recovery");
    });
    test("forward execution verifies paired full reads and commits header last",[&] {
        MockFlash device(source); MemoryStore store; Session session(device,store,device.identity);
        const auto backup=session.backupTwice(); const auto plan=planPackageOverlay(backup,candidate,policy);
        const auto result=session.execute(plan);
        check(result.success && result.targetVerified && result.resetSucceeded && !result.rollbackAttempted,"Forward execution failed: "+result.error);
        check(device.image==plan.target() && fullPasses(device)==6,"Backup/preflight/final are not separate full pairs");
        check(device.eraseCalls==plan.sectors().size() && device.erasedAddresses.back()==HeaderSector,"Sector count/header-last");
        check(result.touchedSectors.size()==plan.sectors().size() && !store.recoveryArmed,"Touched ledger/interlock");
    });
    test("preflight second-pass mismatch prevents every erase and reset",[&] {
        MockFlash device(source); MemoryStore store; Session session(device,store,device.identity);
        const auto backup=session.backupTwice(); const auto plan=planPackageOverlay(backup,candidate,policy); device.invertReadPass=4;
        const auto result=session.execute(plan);
        check(!result.success && !result.resetAttempted && !result.rollbackAttempted,"Bad preflight state");
        check(fullPasses(device)==4 && device.eraseCalls==0 && device.programCalls==0 && device.resetCalls==0,"Preflight mismatch allowed mutation/reset");
    });
    test("plan persistence failure occurs before first flash mutation",[&] {
        MockFlash device(source); MemoryStore store; Session session(device,store,device.identity);
        const auto backup=session.backupTwice(); const auto plan=planPackageOverlay(backup,candidate,policy);
        store.failSaveCall=store.saveCalls+1;
        const auto result=session.execute(plan);
        check(!result.success && device.eraseCalls==0 && device.programCalls==0,"Plan persistence failure followed by erase");
    });
    test("failure after erase restores the throwing sector and source pair",[&] {
        MockFlash device(source); MemoryStore store; Session session(device,store,device.identity);
        const auto backup=session.backupTwice(); const auto plan=planPackageOverlay(backup,candidate,policy);
        device.failEraseCalls={2,3}; // Exhaust both attempts for the second touched sector.
        const auto result=session.execute(plan);
        check(!result.success && result.rollbackAttempted && result.rollbackVerified && result.resetSucceeded,"Restoration failed: "+result.error);
        check(result.touchedSectors.size()==2 && device.eraseCalls>=4 && device.image==source,"Throwing erase omitted from rollback");
        check(fullPasses(device)>=6,"Rollback did not verify a full read pair");
    });
    test("failed rollback attempts remaining touched sectors and prohibits reset",[&] {
        MockFlash device(source); MemoryStore store; Session session(device,store,device.identity);
        const auto backup=session.backupTwice(); const auto plan=planPackageOverlay(backup,candidate,policy);
        device.failEraseCalls={2,3,4,5}; // Forward second sector, then rollback first sector.
        const auto result=session.execute(plan);
        check(!result.success && result.rollbackAttempted && !result.rollbackVerified && result.recoveryRequired,"Failed rollback state");
        check(device.eraseCalls>=4 && device.resetCalls==0 && store.recoveryArmed,"Failed rollback skipped remaining sectors or reset");
    });
    test("final second-pass mismatch triggers full rollback with header last",[&] {
        MockFlash device(source); MemoryStore store; Session session(device,store,device.identity);
        const auto backup=session.backupTwice(); const auto plan=planPackageOverlay(backup,candidate,policy);
        device.invertReadPass=6;
        const auto result=session.execute(plan);
        check(!result.success && result.rollbackVerified && result.resetSucceeded && device.image==source,"Final mismatch rollback failed");
        check(fullPasses(device)>=8 && device.erasedAddresses.back()==HeaderSector,"Rollback full pair/header-last");
    });
    test("untouched unknown-tail corruption prevents reset after rollback",[&] {
        MockFlash device(source); MemoryStore store; Session session(device,store,device.identity);
        const auto backup=session.backupTwice(); const auto plan=planPackageOverlay(backup,candidate,policy);
        device.onRead=[](MockFlash& d,std::uint32_t address,std::uint32_t) {if(d.readPass==5 && address==0)d.image[0x190000]^=1;};
        const auto result=session.execute(plan);
        check(!result.success && result.rollbackAttempted && !result.rollbackVerified && device.resetCalls==0,"Unknown tail corruption ignored");
    });
    test("no-op final pair mismatch prohibits reset without artificial writes",[&] {
        MockFlash device(source); MemoryStore store; Session session(device,store,device.identity);
        const auto backup=session.backupTwice(); const auto plan=planPackageOverlay(backup,originalPackage,policy);
        check(plan.sectors().empty(),"No-op plan has artificial header sector"); device.invertReadPass=6;
        const auto result=session.execute(plan);
        check(!result.success && !result.resetAttempted && device.resetCalls==0 && device.eraseCalls==0 && device.programCalls==0,"No-op failure allowed reset/mutation");
    });
    test("no-op success still requires all three read pairs",[&] {
        MockFlash device(source); MemoryStore store; Session session(device,store,device.identity);
        const auto backup=session.backupTwice(); const auto plan=planPackageOverlay(backup,originalPackage,policy);
        const auto result=session.execute(plan);
        check(result.success && result.targetVerified && result.resetSucceeded && fullPasses(device)==6,"No-op skipped verification");
        check(device.eraseCalls==0 && device.programCalls==0,"No-op programmed flash");
    });
    test("explicit package verification uses a full pair and never programs",[&] {
        MockFlash device(source); MemoryStore store; Session session(device,store,device.identity);
        session.backupTwice();
        const auto result=session.verify(originalPackage,ImageKind::SamsungPackage);
        check(result.success && result.targetVerified && result.resetSucceeded,"Package verify failed: "+result.error);
        check(fullPasses(device)>=2 && fullPasses(device)%2==0 && device.eraseCalls==0 && device.programCalls==0,"Verify did not use paired full reads");
    });
    test("explicit verification rejects disagreement in its second full pass",[&] {
        MockFlash device(source); MemoryStore store;
        Session session(device,store,device.identity);
        session.backupTwice();device.invertReadPass=4;
        const auto result=session.verify(originalPackage,ImageKind::SamsungPackage);
        check(!result.success && device.resetCalls==0 && device.eraseCalls==0,"Verify mismatch allowed reset/programming");
    });
    test("cancellation after mutation is deferred until complete target verification",[&] {
        MockFlash device(source); MemoryStore store;
        Session session(device,store,device.identity,ProgressSink(),[&]{return device.eraseCalls>0;});
        const auto backup=session.backupTwice(); const auto plan=planPackageOverlay(backup,candidate,policy);
        const auto result=session.execute(plan);
        check(result.success && !result.rollbackAttempted && result.targetVerified && result.resetSucceeded && device.image==plan.target(),"Cancellation abandoned modified flash");
    });
    test("progress delivery failures do not interrupt target verification",[&] {
        MockFlash device(source); MemoryStore store;
        Session session(device,store,device.identity,[](const Progress&){throw Error("Synthetic UI delivery failure");});
        const auto backup=session.backupTwice();const auto plan=planPackageOverlay(backup,candidate,policy);
        const auto result=session.execute(plan);
        check(result.success && result.resetSucceeded && device.image==plan.target(),"Progress error interrupted transaction");
    });
    test("no-op final status mismatch prohibits reset",[&] {
        MockFlash device(source); MemoryStore store;Session session(device,store,device.identity);
        const auto backup=session.backupTwice();const auto plan=planPackageOverlay(backup,originalPackage,policy);
        device.onFlashIdentity=[](MockFlash& d){if(d.flashIdentityCalls>=5)d.flash.status[0]=4;};
        const auto result=session.execute(plan);
        check(!result.success && !result.resetAttempted && device.eraseCalls==0,"Final status mismatch allowed reset");
    });
    test("final read persistence failure restores the source before reset",[&] {
        MockFlash device(source); MemoryStore store;Session session(device,store,device.identity);
        const auto backup=session.backupTwice();const auto plan=planPackageOverlay(backup,candidate,policy);
        store.failSaveCall=store.saveCalls+5; // Two preflight files, plan, target, first final read.
        const auto result=session.execute(plan);
        check(!result.success && result.rollbackVerified && result.resetSucceeded && device.image==source,"Persistence failure did not restore verified source");
    });
    test("EEPROM coding remains unavailable independently of mock success",[&] {
        const char* reason=codingEepromUnavailableReason(); check(reason && *reason,"Missing explicit EEPROM block reason");
        check(std::string(reason).find("EEPROM")!=std::string::npos,"Coding block does not identify EEPROM boundary");
    });

    std::cout<<"{\"passed\":"<<passed<<",\"failed\":"<<failed
        <<",\"fixtures\":\"synthetic-only\",\"hardwareAccessed\":false}\n";
    return failed?1:0;
}
