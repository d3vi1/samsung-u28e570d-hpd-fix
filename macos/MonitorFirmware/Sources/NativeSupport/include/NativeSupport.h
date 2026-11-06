#pragma once
#include <stddef.h>
#include <stdint.h>
// Pure, bounded raw-DEFLATE decoding. Requires exact input consumption and output length.
int mf_inflate_exact(const uint8_t *input, size_t input_size, uint8_t *output, size_t output_size);
// Cached IORegistry enumeration only. Does not create IOAV services or send I2C requests.
// Caller owns the returned UTF-8 JSON and releases it with mf_free.
char *mf_copy_cached_routes(void);
void mf_free(void *pointer);
// Unqualified ABI candidate, used only by explicit selected-route operations.
// These mirror the subaddressed private API; they do not establish raw I2C semantics.
typedef struct MFIOAVCandidate MFIOAVCandidate;
MFIOAVCandidate *mf_ioav_candidate_open(uint64_t registry_id);
void mf_ioav_candidate_close(MFIOAVCandidate *service);
int32_t mf_ioav_candidate_write(MFIOAVCandidate *service, uint32_t chip, uint32_t data_address, const uint8_t *bytes, uint32_t count);
// The native wrapper does not return actual read length. Success is NOT proof of a full read.
int32_t mf_ioav_candidate_read(MFIOAVCandidate *service, uint32_t chip, uint32_t data_address, uint8_t *bytes, uint32_t count);
