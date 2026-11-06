#include "monitorfw/broker.hpp"
#include "json_internal.hpp"
namespace monitorfw {
bool validRequestId(const std::string& s){
    if(s.size()!=36)return false;
    for(std::size_t i=0;i<s.size();++i){if(i==8||i==13||i==18||i==23){if(s[i]!='-')return false;}else if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return false;}
    return true;
}
std::string requestIdFromUri(const std::string& uri){
    const std::string prefix="monitorfw-bridge://request/";
    if(uri.compare(0,prefix.size(),prefix)!=0 || !validRequestId(uri.substr(prefix.size())))throw Error("Only a strict monitorfw request UUID URI is accepted");
    return uri.substr(prefix.size());
}
BrokerRequest parseBrokerRequest(const std::string& text,const std::string& id,std::uint64_t now){
    using detail::require;const detail::Json json=detail::JsonReader(text).parse();BrokerRequest r;
    require(validRequestId(id),"Invalid request ID");require(json.at("schema").text()=="monitorfw.request.v1","Unsupported broker request schema");
    r.id=json.at("request_id").text();require(r.id==id,"Request identity differs from activation URI");
    r.createdUnix=json.at("created_unix").integer();
    require((r.createdUnix<=now || r.createdUnix-now<=60) && (r.createdUnix>=now || now-r.createdUnix<=900),"Request is expired or from the future");
    r.action=json.at("action").text();require(r.action=="enumerate"||r.action=="download"||r.action=="upload"||r.action=="verify","Unsupported broker action");
    r.mode=json.at("mode").text();require(r.mode=="mock"||r.mode=="hardware","Unknown execution mode");
    r.providerId=json.at("provider_id").text();require(!r.providerId.empty() && r.providerId.size()<=32 && r.providerId[0]>='a' && r.providerId[0]<='z' && r.providerId.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-")==std::string::npos,"Invalid provider ID");
    r.endpointLocator=json.at("endpoint_locator").text();r.edidSha256=json.at("expected_edid_sha256").text();
    const std::string kind=json.at("image_kind").text();require(kind=="samsung_package"||kind=="spi_full","Unknown image kind");r.imageKind=kind=="spi_full"?ImageKind::OwnSpiDump:ImageKind::SamsungPackage;
    r.firmwareSha256=json.at("firmware_sha256").text();
    auto hash=[](const std::string& value){return value.size()==64 && value.find_first_not_of("0123456789abcdef")==std::string::npos;};
    if(r.action!="enumerate")require(!r.endpointLocator.empty() && r.endpointLocator.size()<=512 && hash(r.edidSha256),"An exact selected endpoint and EDID hash are required");
    if(r.action=="upload"||r.action=="verify")require(hash(r.firmwareSha256),"Input SHA256 is required");
    require(json.object.size()==10,"Unexpected broker fields; paths and command-line arguments are not accepted");return r;
}
}
