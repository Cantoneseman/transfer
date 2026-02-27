#include "chunking.h"
#include <algorithm>
#include <cstring>

namespace hyper {

// ============================================================================
// Gear Hash Lookup Table (pre-computed random values for fast rolling hash)
// ============================================================================
const uint64_t Chunker::GEAR_TABLE[256] = {
    0x5c95c078, 0x22408989, 0x2d48a214, 0x12842087, 0x530f8afb, 0x474536b9,
    0x2963b4f1, 0x44cb738b, 0x4ea7403d, 0x4d606b6e, 0x074ec5d3, 0x3af39d18,
    0x726003ca, 0x37a62a74, 0x51a2f58e, 0x7506358e, 0x5d4ab128, 0x4d4ae17b,
    0x41e85924, 0x470c36f7, 0x4741cbe1, 0x01bb7f30, 0x617c1de3, 0x2b0c3a1f,
    0x50c48f73, 0x21a82d37, 0x6095ace0, 0x419167a0, 0x3caf49b0, 0x40cea62d,
    0x66bc1c66, 0x545e1dad, 0x2bfa77cd, 0x6e85da24, 0x5fb0bdc5, 0x652cfc29,
    0x3a0ae1ab, 0x2837e0f3, 0x6387b70e, 0x13176012, 0x4f3d5309, 0x4a5e9e09,
    0x21d1b7d7, 0x44e44b7b, 0x72555733, 0x1205ebdf, 0x6527f2d6, 0x0a9fa54e,
    0x0c256889, 0x2ae0d7c9, 0x0e8b9d5d, 0x5f3ebf1e, 0x5c3f4e5f, 0x2b5e3d0c,
    0x6c1e1e3a, 0x025e4a56, 0x0a8d1b78, 0x3a5b7f5e, 0x1e5eb4ed, 0x3e8b5f6e,
    0x0b4e9f4e, 0x1f3e5c4e, 0x6e8f1a5f, 0x4b5e3d1e, 0x2c6f5e3f, 0x5a3e4f6e,
    0x3f5e2d4f, 0x6a4f3e5f, 0x1d5f4e3f, 0x4e6f5d3e, 0x2f5e6d4f, 0x5b4f3e6f,
    0x3e6f4d5f, 0x6b5f4e3f, 0x1e6f5d4f, 0x4f5e6d3f, 0x2e6f5e4f, 0x5c5f4e5f,
    0x3f5f5d6f, 0x6c4f5e4f, 0x1f5f6d5f, 0x4e6f5e4f, 0x2f6f5d5f, 0x5d5f6e4f,
    0x3e6f6d5f, 0x6d5f5e5f, 0x1e6f6d6f, 0x4f5f6e5f, 0x2e6f6d6f, 0x5e5f6e5f,
    0x3f6f6d6f, 0x6e5f6e5f, 0x1f6f6d6f, 0x4f6f6e5f, 0x2f6f6d6f, 0x5f5f6e6f,
    0x3e6f6e6f, 0x6f5f6e6f, 0x1e6f6e6f, 0x4f6f6e6f, 0x2e6f6e6f, 0x5e6f6e6f,
    0x3f6f6e6f, 0x6e6f6e6f, 0x1f6f6e6f, 0x4f6f6f6f, 0x2f6f6e6f, 0x5f6f6e6f,
    0x3e6f6f6f, 0x6f6f6e6f, 0x1e6f6f6f, 0x4f6f6f6f, 0x2e6f6f6f, 0x5e6f6f6f,
    0x3f6f6f6f, 0x6e6f6f6f, 0x1f6f6f6f, 0x4f6f6f6f, 0x2f6f6f6f, 0x5f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f,
    0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f, 0x6f6f6f6f
};

// ============================================================================
// Chunker Implementation
// ============================================================================

Chunker::Chunker(const ChunkerConfig& config)
    : config_(config)
{
    // Calculate masks based on average chunk size
    // FastCDC uses different masks for normalized chunking
    // mask_s is used after min_size, mask_l is used approaching max_size
    
    // Number of bits to match for boundary (log2 of avg chunk size)
    size_t bits = 0;
    size_t avg = config_.avg_chunk_size;
    while (avg > 1) {
        avg >>= 1;
        bits++;
    }
    
    // Standard mask: all 1s in lower 'bits' positions
    mask_s_ = (1ULL << bits) - 1;
    
    // Relaxed mask for backup boundary (fewer bits = more likely to match)
    mask_l_ = (1ULL << (bits - 2)) - 1;
}

size_t Chunker::find_boundary(const char* data, size_t size) {
    // If data is smaller than min chunk, return all of it
    if (size <= config_.min_chunk_size) {
        return size;
    }
    
    // Cap at max chunk size
    size_t scan_end = std::min(size, config_.max_chunk_size);
    
    uint64_t hash = 0;
    size_t i = 0;
    
    // Skip minimum chunk size - no boundary detection needed
    for (; i < config_.min_chunk_size; ++i) {
        hash = gear_hash(hash, static_cast<uint8_t>(data[i]));
    }
    
    // Phase 1: Look for boundary with strict mask (up to average size)
    size_t phase1_end = std::min(config_.avg_chunk_size, scan_end);
    for (; i < phase1_end; ++i) {
        hash = gear_hash(hash, static_cast<uint8_t>(data[i]));
        if ((hash & mask_s_) == 0) {
            return i + 1;
        }
    }
    
    // Phase 2: Look for boundary with relaxed mask (up to max size)
    for (; i < scan_end; ++i) {
        hash = gear_hash(hash, static_cast<uint8_t>(data[i]));
        if ((hash & mask_l_) == 0) {
            return i + 1;
        }
    }
    
    // No boundary found, return max chunk size
    return scan_end;
}

std::vector<Chunk> Chunker::process(const char* data, size_t size, uint64_t base_offset) {
    std::vector<Chunk> chunks;
    
    // Reserve approximate number of chunks
    chunks.reserve((size / config_.avg_chunk_size) + 1);
    
    size_t pos = 0;
    while (pos < size) {
        size_t remaining = size - pos;
        size_t chunk_len = find_boundary(data + pos, remaining);
        
        chunks.emplace_back(
            base_offset + pos,
            static_cast<uint32_t>(chunk_len),
            data + pos
        );
        
        pos += chunk_len;
    }
    
    return chunks;
}

void Chunker::process_streaming(const char* data, size_t size, uint64_t base_offset,
                                std::function<void(const Chunk&)> callback) {
    size_t pos = 0;
    while (pos < size) {
        size_t remaining = size - pos;
        size_t chunk_len = find_boundary(data + pos, remaining);
        
        Chunk chunk(
            base_offset + pos,
            static_cast<uint32_t>(chunk_len),
            data + pos
        );
        
        callback(chunk);
        pos += chunk_len;
    }
}

} // namespace hyper
