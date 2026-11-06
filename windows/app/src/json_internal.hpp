#pragma once
#include "monitorfw/core.hpp"
#include <map>
namespace monitorfw { namespace detail {
inline void require(bool value,const std::string& message){if(!value)throw Error(message);}
// Small strict JSON reader for bounded evidence files. Reject duplicate keys,
// excessive nesting and invalid escapes; never executes data. Unknown fields
// may contain any JSON number, but consumed integers remain strictly bounded.
struct Json {
    enum Type { Null, String, Number, Boolean, Object, Array } type=Null;
    std::string string;std::uint64_t number=0;bool boolean=false, unsignedInteger=false;
    std::map<std::string,Json> object;std::vector<Json> array;
    const Json& at(const std::string& key)const{require(type==Object,"Manifest field is not an object");auto p=object.find(key);require(p!=object.end(),"Missing manifest field: "+key);return p->second;}
    std::string text()const{require(type==String,"Manifest field must be a string");return string;}
    std::uint64_t integer()const{require(type==Number && unsignedInteger,"Manifest field must be a bounded unsigned integer without fraction or exponent");return number;}
    bool flag()const{require(type==Boolean,"Manifest field must be boolean");return boolean;}
};
class JsonReader {
    const std::string& s_;std::size_t p_=0;
    void white(){while(p_<s_.size() && (s_[p_]==' '||s_[p_]=='\t'||s_[p_]=='\r'||s_[p_]=='\n'))++p_;}
    char take(){require(p_<s_.size(),"Truncated JSON");return s_[p_++];}
    void literal(const char* v){while(*v)require(take()==*v++,"Invalid JSON literal");}
    unsigned hex4(){unsigned v=0;for(int n=0;n<4;++n){char c=take();unsigned d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:99;require(d<16,"Invalid Unicode escape");v=v*16+d;}return v;}
    void utf8(std::string& o,unsigned c){if(c<128)o+=char(c);else if(c<2048){o+=char(0xc0|(c>>6));o+=char(0x80|(c&63));}else if(c<65536){o+=char(0xe0|(c>>12));o+=char(0x80|((c>>6)&63));o+=char(0x80|(c&63));}else{o+=char(0xf0|(c>>18));o+=char(0x80|((c>>12)&63));o+=char(0x80|((c>>6)&63));o+=char(0x80|(c&63));}}
    std::string string(){
        require(take()=='"',"Expected JSON string");std::string out;
        for(;;){unsigned char c=static_cast<unsigned char>(take());if(c=='"')break;require(c>=32,"Control byte in JSON string");
            if(c!='\\'){out+=char(c);continue;}
            char e=take();switch(e){case '"':out+='"';break;case '\\':out+='\\';break;case '/':out+='/';break;
            case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;
            case 'u':{unsigned code=hex4();if(code>=0xd800 && code<=0xdbff){require(take()=='\\' && take()=='u',"Missing Unicode low surrogate");unsigned low=hex4();require(low>=0xdc00 && low<=0xdfff,"Invalid Unicode low surrogate");code=0x10000+((code-0xd800)<<10)+(low-0xdc00);}else require(code<0xdc00 || code>0xdfff,"Unexpected Unicode low surrogate");utf8(out,code);break;}
            default:throw Error("Invalid JSON escape");}}
        return out;
    }
    Json value(unsigned depth){
        require(depth<16,"Manifest nesting too deep");white();require(p_<s_.size(),"Missing JSON value");Json out;char c=s_[p_];
        if(c=='"'){out.type=Json::String;out.string=string();return out;}
        if(c=='{' || c=='['){const bool obj=c=='{';out.type=obj?Json::Object:Json::Array;++p_;white();const char end=obj?'}':']';if(p_<s_.size() && s_[p_]==end){++p_;return out;}
            unsigned count=0;for(;;){require(++count<=1024,"Too many manifest fields");white();if(obj){std::string key=string();white();require(take()==':',"Missing JSON colon");Json child=value(depth+1);require(out.object.emplace(key,child).second,"Duplicate JSON key");}else out.array.push_back(value(depth+1));white();char separator=take();if(separator==end)break;require(separator==',',"Missing JSON comma");}return out;}
        if(c=='t'||c=='f'){out.type=Json::Boolean;out.boolean=c=='t';literal(out.boolean?"true":"false");return out;}
        if(c=='n'){literal("null");return out;}
        out.type=Json::Number;out.unsignedInteger=true;
        if(c=='-'){out.unsignedInteger=false;++p_;}
        require(p_<s_.size() && s_[p_]>='0' && s_[p_]<='9',"Invalid JSON number");
        const std::size_t digitsStart=p_;const bool zero=s_[p_]=='0';
        do{const unsigned digit=unsigned(take()-'0');if(out.unsignedInteger){if(out.number>(UINT64_MAX-digit)/10)out.unsignedInteger=false;else out.number=out.number*10+digit;}}
        while(p_<s_.size() && s_[p_]>='0' && s_[p_]<='9' && !zero);
        require(p_>digitsStart && !(zero && p_<s_.size() && s_[p_]>='0' && s_[p_]<='9'),"Leading zero in JSON number");
        if(p_<s_.size() && s_[p_]=='.'){
            out.unsignedInteger=false;const std::size_t start=++p_;while(p_<s_.size() && s_[p_]>='0' && s_[p_]<='9')++p_;require(p_>start,"Missing JSON fractional digits");
        }
        if(p_<s_.size() && (s_[p_]=='e' || s_[p_]=='E')){
            out.unsignedInteger=false;++p_;if(p_<s_.size() && (s_[p_]=='+' || s_[p_]=='-'))++p_;
            const std::size_t start=p_;while(p_<s_.size() && s_[p_]>='0' && s_[p_]<='9')++p_;require(p_>start,"Missing JSON exponent digits");
        }
        return out;
    }
public:
    explicit JsonReader(const std::string& s):s_(s){require(s.size()<=1024*1024,"Manifest exceeds 1 MiB");}
    Json parse(){Json out=value(0);white();require(p_==s_.size(),"Trailing JSON data");return out;}
};
} }
