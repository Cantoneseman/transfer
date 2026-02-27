#include "compressor.h"
#include <lz4.h>
#include <lz4hc.h>
#include <cstring>
#include <sstream>

namespace hyper {

// ============================================================================
// Compressor Implementation
// ============================================================================

Compressor::Compressor(CompressionLevel level)
    : level_(level)
{
}

size_t Compressor::max_compressed_size(size_t input_size) {
    return static_cast<size_t>(LZ4_compressBound(static_cast<int>(input_size)));
}

CompressResult Compressor::compress(const char* data, size_t size) {
    if (data == nullptr || size == 0) {
        throw CompressionError("Cannot compress null or empty data");
    }
    
    // Sanity check for LZ4's int limitation
    if (size > static_cast<size_t>(LZ4_MAX_INPUT_SIZE)) {
        std::ostringstream oss;
        oss << "Input size " << size << " exceeds LZ4 maximum " << LZ4_MAX_INPUT_SIZE;
        throw CompressionError(oss.str());
    }
    
    CompressResult result;
    result.original_size = size;
    
    // Allocate output buffer
    size_t max_dst_size = max_compressed_size(size);
    result.data.resize(max_dst_size);
    
    int compressed_size = 0;
    
    switch (level_) {
        case CompressionLevel::FAST:
            compressed_size = LZ4_compress_fast(
                data,
                result.data.data(),
                static_cast<int>(size),
                static_cast<int>(max_dst_size),
                1  // acceleration factor
            );
            break;
            
        case CompressionLevel::DEFAULT:
            compressed_size = LZ4_compress_default(
                data,
                result.data.data(),
                static_cast<int>(size),
                static_cast<int>(max_dst_size)
            );
            break;
            
        case CompressionLevel::HIGH:
            compressed_size = LZ4_compress_HC(
                data,
                result.data.data(),
                static_cast<int>(size),
                static_cast<int>(max_dst_size),
                LZ4HC_CLEVEL_DEFAULT
            );
            break;
    }
    
    if (compressed_size <= 0) {
        throw CompressionError("LZ4 compression failed");
    }
    
    result.compressed_size = static_cast<size_t>(compressed_size);
    result.data.resize(result.compressed_size);  // Shrink to actual size
    result.ratio = static_cast<double>(result.compressed_size) / 
                   static_cast<double>(result.original_size);
    
    return result;
}

DecompressResult Compressor::decompress(const char* compressed_data, 
                                        size_t compressed_size,
                                        size_t original_size) {
    if (compressed_data == nullptr || compressed_size == 0) {
        throw CompressionError("Cannot decompress null or empty data");
    }
    
    if (original_size == 0) {
        throw CompressionError("Original size must be specified for decompression");
    }
    
    DecompressResult result;
    result.data.resize(original_size);
    
    int decompressed_size = LZ4_decompress_safe(
        compressed_data,
        result.data.data(),
        static_cast<int>(compressed_size),
        static_cast<int>(original_size)
    );
    
    if (decompressed_size < 0) {
        throw CompressionError("LZ4 decompression failed - data may be corrupted");
    }
    
    if (static_cast<size_t>(decompressed_size) != original_size) {
        std::ostringstream oss;
        oss << "Decompressed size mismatch: expected " << original_size 
            << ", got " << decompressed_size;
        throw CompressionError(oss.str());
    }
    
    result.size = static_cast<size_t>(decompressed_size);
    return result;
}

} // namespace hyper
