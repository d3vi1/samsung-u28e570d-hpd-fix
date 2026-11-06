#include "monitorfw/broker.hpp"
#include <iostream>
#include <functional>
using namespace monitorfw;
int main(){unsigned passed=0;try{
    const std::string id="12345678-1234-4234-8234-123456789abc",hash(64,'a');
    const std::string valid="{\"schema\":\"monitorfw.request.v1\",\"request_id\":\""+id+"\",\"created_unix\":1000,\"action\":\"upload\",\"mode\":\"mock\",\"provider_id\":\"nvidia\",\"endpoint_locator\":\"mock://selected\",\"expected_edid_sha256\":\""+hash+"\",\"image_kind\":\"samsung_package\",\"firmware_sha256\":\""+hash+"\"}";
    auto reject=[&](const std::function<void()>& f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}if(!failed)throw Error("Invalid broker request accepted");++passed;};
    if(requestIdFromUri("monitorfw-bridge://request/"+id)!=id || parseBrokerRequest(valid,id,1000).action!="upload")throw Error("Valid broker request failed");++passed;
    for(const auto& suffix:{std::string("?action=upload"),std::string("/../x"),std::string("%22"),std::string("\" --command evil")})reject([&]{requestIdFromUri("monitorfw-bridge://request/"+id+suffix);});
    reject([&]{requestIdFromUri("https://request/"+id);});
    reject([&]{parseBrokerRequest(valid,id,1901);});
    reject([&]{parseBrokerRequest(valid,id,939);});
    reject([&]{parseBrokerRequest(valid,"12345678-1234-4234-8234-123456789abd",1000);});
    reject([&]{std::string extra=valid;extra.insert(1,"\"output_path\":\"C:\\\\Windows\",");parseBrokerRequest(extra,id,1000);});
    reject([&]{std::string extra=valid;extra.insert(1,"\"schema\":\"duplicate\",");parseBrokerRequest(extra,id,1000);});
    for(const auto& change:{std::make_pair("upload","erase-chip"),std::make_pair("mock","automatic"),std::make_pair("nvidia","../unknown"),std::make_pair("samsung_package","arbitrary")}){
        reject([&]{std::string modified=valid;auto p=modified.find(change.first);modified.replace(p,std::string(change.first).size(),change.second);parseBrokerRequest(modified,id,1000);});
    }
    std::cout<<"{\"passed\":"<<passed<<",\"hardwareAccessed\":false,\"registryAccessed\":false}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
