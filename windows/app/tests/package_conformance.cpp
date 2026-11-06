#include "mock_flash.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

// Standalone offline inspector and synthetic DEFLATE conformance tests.
// No transport object is constructed; file input is never written back.
using namespace monitorfw;
using namespace monitorfw::testing;
namespace {
void require(bool value,const char* message) { if(!value) throw Error(message); }
std::string hexValue(std::uint32_t value,unsigned digits) {
    std::ostringstream out; out<<std::uppercase<<std::hex<<std::setfill('0')<<std::setw(digits)<<value; return out.str();
}
std::uint32_t readBe(const Bytes& bytes,std::size_t position,unsigned count) {
    std::uint32_t result=0; while(count--)result=(result<<8)|bytes.at(position++); return result;
}
Bytes fromHex(const std::string& text) {
    require(text.size()%2==0,"Odd fixture hex length"); Bytes result;
    for(std::size_t i=0;i<text.size();i+=2) {
        const std::string pair=text.substr(i,2); unsigned value=0; std::istringstream input(pair); input>>std::hex>>value;
        require(!input.fail(),"Invalid fixture hex"); result.push_back(std::uint8_t(value));
    }
    return result;
}
Bytes wrapRaw(const Bytes& raw,std::uint32_t expanded) {
    const auto base=syntheticPackage();
    const auto end=std::uint32_t(0x23880+raw.size()+6), final=((end+7U)&~7U)+8U;
    Bytes package(final+4,0); std::copy(base.begin(),base.begin()+0x23880,package.begin());
    fixturePutLe32(package,0x10008,0x200000+end-0x20080);
    fixturePutLe32(package,0x1000c,end); fixturePutLe32(package,0x10020,final);
    fixturePutLe32(package,0x211ca,end-0x20080);
    std::copy(raw.begin(),raw.end(),package.begin()+0x23880);
    fixturePutLe32(package,end-6,expanded);
    const std::uint8_t marker[]={0x55,0xaa,0xab,0xcd,0,0};
    std::copy(marker,marker+6,package.begin()+final-8); repairSyntheticIntegrity(package); return package;
}
Bytes readFile(const char* path,std::size_t maximum) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    require(bool(input),"Cannot open input file"); const auto length=input.tellg();
    require(length>=0 && std::uint64_t(length)<=maximum,"Input exceeds the explicit image-kind limit");
    Bytes data(static_cast<std::size_t>(length)); input.seekg(0);
    if(!data.empty()) input.read(reinterpret_cast<char*>(data.data()),std::streamsize(data.size()));
    require(bool(input),"Input file changed or could not be read completely"); return data;
}
void report(const Bytes& package,const PackageInfo& info) {
    const auto end=fixtureLe32(package,0x1000c), final=fixtureLe32(package,0x10020);
    std::cout<<"{\n"
        <<"  \"format_valid\": true,\n"
        <<"  \"container_sha256\": \""<<info.sha256<<"\",\n"
        <<"  \"container_length\": "<<info.length<<",\n"
        <<"  \"main_end\": "<<info.mainEnd<<",\n"
        <<"  \"crc32_offset\": "<<final<<",\n"
        <<"  \"compressed_length\": "<<end-6-0x23880<<",\n"
        <<"  \"app_length\": "<<info.expandedLength<<",\n"
        <<"  \"app_sha256\": \""<<info.expandedSha256<<"\",\n"
        <<"  \"version\": \""<<info.version<<"\",\n"
        <<"  \"main_crc32\": \""<<hexValue(fixtureLe32(package,0x10014),8)<<"\",\n"
        <<"  \"inner_crc16\": \""<<hexValue(readBe(package,end-2,2),4)<<"\",\n"
        <<"  \"outer_crc16\": \""<<hexValue(readBe(package,final-2,2),4)<<"\",\n"
        <<"  \"file_crc32\": \""<<hexValue(readBe(package,final,4),8)<<"\",\n"
        <<"  \"service_sum16\": \""<<hexValue(info.sum16,4)<<"\",\n"
        <<"  \"validation_scope\": \"container integrity only; no programming authorization or hardware qualification\",\n"
        <<"  \"hardware_accessed\": false\n}\n";
}
unsigned syntheticTests() {
    unsigned passed=0;
    const auto stored=inspectPackage(syntheticPackage());
    require(stored.expandedLength>0 && stored.version=="m-E5728UMDB-1000.0","Stored block fixture failed"); ++passed;
    const auto multi=inspectPackage(syntheticPackage("1000.0",70000));
    require(multi.expandedLength==stored.expandedLength+70000,"Multiple stored blocks failed"); ++passed;
    const auto unlabelled=inspectPackage(wrapRaw(Bytes{1,1,0,0xfe,0xff,'A'},1));
    require(unlabelled.expandedLength==1 && unlabelled.version=="unknown","Missing optional version label invalidated the format"); ++passed;
    bool emptyTreeRejected=false;
    try { inspectPackage(wrapRaw(Bytes{5,0,0,0},1)); } catch(const std::exception&) { emptyTreeRejected=true; }
    require(emptyTreeRejected,"Empty dynamic code-length tree accepted"); ++passed;
    // Produced offline by zlib 1.3.2 with level9/wbits=-15; fixed uses Z_FIXED,
    // dynamic uses Z_DEFAULT_STRATEGY. Only synthetic bytes occur in the payload:
    // ASCII prefix below, followed by 8192 bytes 'A'+((i*i+7*i)%29).
    const std::string prefix="SYNTHETIC DEFLATE CONFORMANCE, NOT FIRMWARE\nm-E5728UMDB-1000.0\n";
    Bytes expected(prefix.begin(),prefix.end());
    for(unsigned i=0;i<8192;++i)expected.push_back(std::uint8_t(65+(i*i+7*i)%29));
    require(expected.size()==8255 && sha256(expected)=="3f0bf5e223a11eddc627b3b27f4d2ad8bc8dd257652ed095b88d6cfbf0c63da0","Synthetic payload identity changed"); ++passed;
    const std::string fixed=
        "0b8ef40bf1700df17456707175f3710c715570f6f773f30ff275f47376d551f0f30f5170f30cf20d770c72e5cad575353537b208f57571d235343030d033e072f40c760a700ef5718d8d8a8c8a75f509750e700af6748c08090a0a8918951c951c951c951c951c951c951c951c951c951c951c951c951c951c951c951c951c951c951c951c951c951c951c951c951c951c951c951c951c951c951c129200";
    const std::string dynamic=
        "edc94b0ac2301440d179579105588982e834495f30904fc907db4297e0fea7ae43b8677ada9efb5b7a706a111f4d17e54af6a526939d5c542e5df950d3c75499beb33c9ef7d7488b9d6f5aebab9e4c68767523ca79ecc72971b8d5b660b65e6bdf4892244992244992244992244992244992244992244992fc8bfc01";
    for(const auto& fixture:std::vector<std::pair<std::string,unsigned>>{{fixed,1},{dynamic,2}}) {
        const auto raw=fromHex(fixture.first); require(((raw[0]>>1)&3)==fixture.second,"Wrong fixture block type");
        const auto info=inspectPackage(wrapRaw(raw,std::uint32_t(expected.size())));
        require(info.expandedLength==expected.size() && info.expandedSha256==sha256(expected) && info.version=="m-E5728UMDB-1000.0","Huffman fixture decoded incorrectly"); ++passed;
        for(unsigned mutation=0;mutation<5;++mutation) {
            auto changed=raw; auto declared=std::uint32_t(expected.size());
            if(mutation==0) changed.pop_back();
            if(mutation==1) changed.push_back(0);
            if(mutation==2) changed[0]&=0xfe; // Missing final block; EOS cannot be inferred from output length.
            if(mutation==3) --declared;
            if(mutation==4) ++declared;
            bool rejected=false;
            try{inspectPackage(wrapRaw(changed,declared));}catch(const std::exception&){rejected=true;}
            require(rejected,"CRC-valid DEFLATE boundary/length mutation was accepted"); ++passed;
        }
    }
    std::cout<<"{\"synthetic_cases_passed\":"<<passed<<",\"stored_fixed_dynamic\":true,\"hardware_accessed\":false}\n";
    return passed;
}
}
int main(int argc,char** argv) {
    try {
        if(argc==1 || (argc==2 && std::string(argv[1])=="--self-test")) { syntheticTests(); return 0; }
        if(argc!=3 || (std::string(argv[1])!="--package" && std::string(argv[1])!="--dump")) {
            std::cerr<<"Usage: package_conformance [--self-test | --package FILE | --dump FILE]\n"; return 2;
        }
        const bool dump=std::string(argv[1])=="--dump";
        const auto input=readFile(argv[2],dump?SpiCapacity:PackageLimit);
        const auto package=dump?packageFromDump(input):input;
        report(package,inspectPackage(package)); return 0;
    } catch(const std::exception& error) { std::cerr<<"Validation failed: "<<error.what()<<"\n"; return 1; }
}
