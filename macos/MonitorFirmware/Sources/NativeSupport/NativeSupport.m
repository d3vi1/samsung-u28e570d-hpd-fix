#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#include <zlib.h>
#include <dlfcn.h>
#include "NativeSupport.h"

int mf_inflate_exact(const uint8_t *input, size_t input_size, uint8_t *output, size_t output_size) {
    if (!input || !output || !input_size || !output_size || input_size > UINT_MAX || output_size > UINT_MAX) return -1;
    z_stream stream = {0};
    stream.next_in = (Bytef *)input; stream.avail_in = (uInt)input_size;
    stream.next_out = output; stream.avail_out = (uInt)output_size;
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return -2;
    int result = inflate(&stream, Z_FINISH);
    int valid = result == Z_STREAM_END && stream.total_in == input_size && stream.total_out == output_size;
    inflateEnd(&stream);
    return valid ? 0 : -3;
}

char *mf_copy_cached_routes(void) {
    @autoreleasepool {
        NSMutableArray *routes = [NSMutableArray array];
        io_iterator_t iterator = 0;
        IOReturn status = IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("DCPAVServiceProxy"), &iterator);
        if (status != KERN_SUCCESS) return NULL;
        io_service_t service;
        while ((service = IOIteratorNext(iterator))) {
            uint64_t identifier = 0;
            io_string_t path = {0};
            IORegistryEntryGetRegistryEntryID(service, &identifier);
            IORegistryEntryGetPath(service, kIOServicePlane, path);
            [routes addObject:@{@"registry_id": @(identifier), @"path": [NSString stringWithUTF8String:path] ?: @""}];
            IOObjectRelease(service);
        }
        IOObjectRelease(iterator);
        NSData *data = [NSJSONSerialization dataWithJSONObject:routes options:0 error:NULL];
        if (!data) return NULL;
        char *result = calloc(data.length + 1, 1);
        if (result) memcpy(result, data.bytes, data.length);
        return result;
    }
}
void mf_free(void *pointer) { free(pointer); }

struct MFIOAVCandidate {
    void *library;
    CFTypeRef service;
    IOReturn (*write)(CFTypeRef,uint32_t,uint32_t,void*,uint32_t);
    IOReturn (*read)(CFTypeRef,uint32_t,uint32_t,void*,uint32_t);
};
MFIOAVCandidate *mf_ioav_candidate_open(uint64_t registry_id) {
    if (!registry_id) return NULL;
    MFIOAVCandidate *candidate = calloc(1, sizeof(*candidate));
    if (!candidate) return NULL;
    candidate->library = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL);
    if (!candidate->library) { free(candidate); return NULL; }
    CFTypeRef (*create)(CFAllocatorRef,io_service_t) = dlsym(candidate->library,"IOAVServiceCreateWithService");
    candidate->write = dlsym(candidate->library,"IOAVServiceWriteI2C");
    candidate->read = dlsym(candidate->library,"IOAVServiceReadI2C");
    if (!create || !candidate->write || !candidate->read) { mf_ioav_candidate_close(candidate); return NULL; }
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IORegistryEntryIDMatching(registry_id));
    if (!service) { mf_ioav_candidate_close(candidate); return NULL; }
    uint64_t actual_id = 0;
    IORegistryEntryGetRegistryEntryID(service,&actual_id);
    if (actual_id == registry_id && IOObjectConformsTo(service,"DCPAVServiceProxy")) candidate->service = create(kCFAllocatorDefault,service);
    IOObjectRelease(service);
    if (!candidate->service) { mf_ioav_candidate_close(candidate); return NULL; }
    return candidate;
}
void mf_ioav_candidate_close(MFIOAVCandidate *candidate) {
    if (!candidate) return;
    if (candidate->service) CFRelease(candidate->service);
    if (candidate->library) dlclose(candidate->library);
    free(candidate);
}
int32_t mf_ioav_candidate_write(MFIOAVCandidate *candidate, uint32_t chip, uint32_t address, const uint8_t *bytes, uint32_t count) {
    if (!candidate || !candidate->service || count > 4096 || (count && !bytes)) return kIOReturnBadArgument;
    return candidate->write(candidate->service,chip,address,(void *)bytes,count);
}
int32_t mf_ioav_candidate_read(MFIOAVCandidate *candidate, uint32_t chip, uint32_t address, uint8_t *bytes, uint32_t count) {
    if (!candidate || !candidate->service || !bytes || !count || count > 4096) return kIOReturnBadArgument;
    return candidate->read(candidate->service,chip,address,bytes,count);
}
