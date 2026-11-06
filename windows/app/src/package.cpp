#include "monitorfw/core.hpp"
#include <algorithm>
#include <limits>

namespace monitorfw {
namespace {
void require(bool condition, const char* message) { if (!condition) throw Error(message); }
std::uint32_t le32(const Bytes& b, std::size_t p) {
    require(p <= b.size() && b.size()-p >= 4, "Truncated little-endian field");
    return b[p] | (std::uint32_t(b[p+1])<<8) | (std::uint32_t(b[p+2])<<16) | (std::uint32_t(b[p+3])<<24);
}
std::uint32_t be(const Bytes& b, std::size_t p, unsigned n) {
    require(p <= b.size() && b.size()-p >= n, "Truncated big-endian field");
    std::uint32_t v=0; while(n--) v=(v<<8)|b[p++]; return v;
}
std::uint32_t crc(const Bytes& b, std::size_t begin, std::size_t end, unsigned width, bool zeroMainCrc=false) {
    require(begin <= end && end <= b.size(), "CRC range outside package");
    const std::uint32_t poly=width==16 ? 0x8005u : 0x04c11db7u;
    const std::uint32_t high=std::uint32_t(1)<<(width-1), mask=width==16 ? 0xffffu : 0xffffffffu;
    std::uint32_t value=0;
    for(std::size_t p=begin;p<end;++p) {
        const std::uint8_t byte=zeroMainCrc && p>=0x10014 && p<0x10018 ? 0 : b[p];
        value ^= std::uint32_t(byte) << (width-8);
        for(unsigned bit=0;bit<8;++bit) value=((value<<1)^((value&high)?poly:0))&mask;
    }
    return value;
}
class Bits {
    const Bytes& bytes_; std::size_t start_, end_, bit_=0;
public:
    Bits(const Bytes& b,std::size_t start,std::size_t end):bytes_(b),start_(start),end_(end){}
    unsigned take(unsigned n) {
        require(n<=16 && bit_ <= (end_-start_)*8 && n <= (end_-start_)*8-bit_, "Truncated DEFLATE stream");
        unsigned v=0;
        for(unsigned i=0;i<n;++i,++bit_) v|=((bytes_[start_+bit_/8]>>(bit_%8))&1u)<<i;
        return v;
    }
    void align() { bit_=(bit_+7)&~std::size_t(7); }
    std::size_t consumed() const { return (bit_+7)/8; }
};
class Huffman {
    struct Node { int child[2]; int symbol; Node():symbol(-1){child[0]=child[1]=-1;} };
    std::vector<Node> nodes_;
public:
    Huffman(const std::vector<unsigned>& lengths,bool codeTree=false,bool allowEmpty=false) {
        unsigned count[16]={0},next[16]={0},maximum=0,total=0;
        for(unsigned n:lengths){require(n<=15,"Invalid Huffman code length");if(n){++count[n];maximum=std::max(maximum,n);++total;}}
        nodes_.push_back(Node());
        if(!total){require(allowEmpty,"Empty Huffman tree");return;}
        int left=1;
        for(unsigned n=1;n<=15;++n){left=left*2-int(count[n]);require(left>=0,"Oversubscribed Huffman tree");}
        require(left==0 || (!codeTree && maximum==1),"Incomplete Huffman tree");
        unsigned code=0;
        for(unsigned n=1;n<=15;++n){code=(code+count[n-1])<<1;next[n]=code;}
        for(unsigned symbol=0;symbol<lengths.size();++symbol){
            const unsigned length=lengths[symbol];if(!length)continue;
            code=next[length]++;int node=0;
            for(int n=int(length)-1;n>=0;--n){
                require(nodes_[node].symbol<0,"Huffman prefix collision");
                const unsigned bit=(code>>n)&1;
                int child=nodes_[node].child[bit];
                if(child<0){child=int(nodes_.size());nodes_[node].child[bit]=child;nodes_.push_back(Node());}
                node=child;
            }
            require(nodes_[node].symbol<0 && nodes_[node].child[0]<0 && nodes_[node].child[1]<0,"Huffman code collision");
            nodes_[node].symbol=int(symbol);
        }
    }
    unsigned decode(Bits& bits)const{
        int node=0;
        for(unsigned n=0;n<=15;++n){
            require(node>=0,"Invalid Huffman code");
            if(nodes_[node].symbol>=0)return unsigned(nodes_[node].symbol);
            node=nodes_[node].child[bits.take(1)];
        }
        throw Error("Huffman code exceeds 15 bits");
    }
};
Bytes inflateRaw(const Bytes& b,std::size_t start,std::size_t end,std::uint32_t expected){
    require(start<end && end<=b.size(),"Invalid DEFLATE range");
    require(expected>0 && expected<=8*1024*1024,"Expanded image exceeds bounded size");
    Bits bits(b,start,end);Bytes out;out.reserve(expected);bool final=false;
    const unsigned lengthBase[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    const unsigned lengthExtra[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    const unsigned distanceBase[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    const unsigned distanceExtra[30]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    while(!final){
        final=bits.take(1)!=0;const unsigned type=bits.take(2);
        require(type!=3,"Reserved DEFLATE block type");
        if(type==0){
            bits.align();const unsigned length=bits.take(16), inverse=bits.take(16);
            require((length^inverse)==0xffff,"Stored DEFLATE block length mismatch");
            require(length<=expected-out.size(),"DEFLATE expansion exceeds declared size");
            for(unsigned n=0;n<length;++n)out.push_back(std::uint8_t(bits.take(8)));
            continue;
        }
        std::vector<unsigned> literal,distance;
        if(type==1){
            literal.resize(288);distance.assign(32,5);
            for(unsigned n=0;n<288;++n)literal[n]=n<=143?8:n<=255?9:n<=279?7:8;
        }else{
            const unsigned nl=bits.take(5)+257,nd=bits.take(5)+1,nc=bits.take(4)+4;
            require(nl<=286,"Invalid DEFLATE literal count");
            const unsigned order[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            std::vector<unsigned> codeLengths(19,0);
            for(unsigned n=0;n<nc;++n)codeLengths[order[n]]=bits.take(3);
            const Huffman codes(codeLengths,true);std::vector<unsigned> all;
            while(all.size()<nl+nd){
                const unsigned symbol=codes.decode(bits);
                if(symbol<16){all.push_back(symbol);continue;}
                unsigned repeated=0,value=0;
                if(symbol==16){require(!all.empty(),"DEFLATE repeat has no predecessor");value=all.back();repeated=bits.take(2)+3;}
                else if(symbol==17)repeated=bits.take(3)+3;
                else if(symbol==18)repeated=bits.take(7)+11;
                else throw Error("Invalid DEFLATE length symbol");
                require(repeated<=nl+nd-all.size(),"DEFLATE repeat overflows tree");
                all.insert(all.end(),repeated,value);
            }
            literal.assign(all.begin(),all.begin()+nl);distance.assign(all.begin()+nl,all.end());
        }
        require(literal.size()>256 && literal[256]!=0,"Missing DEFLATE end-of-block code");
        const Huffman lit(literal),dist(distance,false,true);
        for(;;){
            unsigned symbol=lit.decode(bits);
            if(symbol<256){require(out.size()<expected,"DEFLATE expansion exceeds declared size");out.push_back(std::uint8_t(symbol));continue;}
            if(symbol==256)break;
            require(symbol>=257 && symbol<=285,"Reserved DEFLATE length code");
            symbol-=257;const unsigned length=lengthBase[symbol]+bits.take(lengthExtra[symbol]);
            const unsigned ds=dist.decode(bits);require(ds<30,"Reserved DEFLATE distance code");
            const unsigned back=distanceBase[ds]+bits.take(distanceExtra[ds]);
            require(back<=out.size() && length<=expected-out.size(),"Invalid DEFLATE back-reference or expansion limit");
            for(unsigned n=0;n<length;++n)out.push_back(out[out.size()-back]);
        }
    }
    require(bits.consumed()==end-start,"Unused bytes after DEFLATE end-of-stream");
    require(out.size()==expected,"Expanded application length mismatch");
    return out;
}
}
PackageInfo inspectPackage(const Bytes& b){
    require(b.size()>0x23880+24 && b.size()<=PackageLimit,"Input is not a supported Samsung package (full SPI dumps are a separate kind)");
    const std::uint32_t mainEnd=le32(b,0x1000c),crcEnd=le32(b,0x10020);
    require(le32(b,0x10000)==0x20080 && le32(b,0x10004)==0x200000,"Unexpected Samsung main/load address");
    require(mainEnd>0x23880+6 && mainEnd<b.size(),"Main image end outside package");
    require(std::uint64_t(0x200000)+mainEnd-0x20080==le32(b,0x10008),"Main RAM end mismatch");
    const std::uint64_t aligned=(std::uint64_t(mainEnd)+7)&~std::uint64_t(7);
    require(aligned+8==crcEnd && std::uint64_t(crcEnd)+4==b.size(),"Package length, alignment and CRC boundary disagree");
    require(le32(b,0x10010)==0 && le32(b,0x10024)==0x10000,"Unsupported Samsung header flags/coverage");
    require(b[0x211c7]==0xbe && b[0x211c8]==0xad && b[0x211c9]==0xde,"Missing compression marker");
    require(le32(b,0x211ca)==mainEnd-0x20080 && le32(b,0x211ce)==0x3800,"Embedded compressed/head lengths disagree");
    require(crc(b,0x20080,mainEnd,32)==le32(b,0x10014),"Main CRC32 mismatch");
    require(crc(b,0,crcEnd,32)==be(b,crcEnd,4) && crc(b,0,b.size(),32)==0,"Container CRC32 mismatch");
    require(crc(b,0x10000,mainEnd-2,16,true)==be(b,mainEnd-2,2),"Inner CRC16 mismatch");
    for(std::size_t n=mainEnd;n<aligned;++n)require(b[n]==0,"Nonzero alignment padding");
    const std::uint8_t marker[6]={0x55,0xaa,0xab,0xcd,0,0};
    require(std::equal(marker,marker+6,b.begin()+std::size_t(aligned)),"Outer marker mismatch");
    require(crc(b,0x10000,crcEnd-2,16,true)==be(b,crcEnd-2,2),"Outer CRC16 mismatch");
    const std::uint32_t expanded=le32(b,mainEnd-6);
    const Bytes app=inflateRaw(b,0x23880,mainEnd-6,expanded);
    PackageInfo result;result.length=std::uint32_t(b.size());result.mainEnd=mainEnd;
    result.expandedLength=expanded;result.sha256=sha256(b);result.expandedSha256=sha256(app);
    for(std::uint8_t byte:b)result.sum16=std::uint16_t(result.sum16+byte);
    const std::string text(app.begin(),app.end()),prefix="m-E5728UMDB-";
    std::size_t pos=text.find(prefix);
    if(pos!=std::string::npos){pos+=prefix.size();while(pos<text.size() && result.version.size()<24 && ((text[pos]>='0' && text[pos]<='9') || text[pos]=='.'))result.version+=text[pos++];}
    if(result.version.empty())result.version="unknown";
    else result.version=prefix+result.version;
    return result;
}
Bytes packageFromDump(const Bytes& dump){
    require(dump.size()==SpiCapacity,"A full SPI image must be exactly 2 MiB");
    const std::uint64_t length=std::uint64_t(le32(dump,0x10020))+4;
    require(length<=PackageLimit && length>0x23880+24,"Full SPI dump contains an unsupported package boundary");
    Bytes package(dump.begin(),dump.begin()+std::size_t(length));inspectPackage(package);return package;
}
} // namespace monitorfw
