#ifndef CHUNKING_H
#define CHUNKING_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <functional>

// ============================================================================
// HYPER-TRANSFER v2.0 - FastCDC Content-Defined Chunking
// ============================================================================

namespace hyper {

/**
 * @brief Represents a single chunk of data.
 */
struct Chunk {
    uint64_t    offset;      // Absolute offset in the source file
    uint32_t    length;      // Length of this chunk
    const char* data_ptr;    // Pointer to chunk data (non-owning)
    
    Chunk() : offset(0), length(0), data_ptr(nullptr) {}
    Chunk(uint64_t off, uint32_t len, const char* ptr)
        : offset(off), length(len), data_ptr(ptr) {}
};

/**
 * @brief Configuration for the chunker.
 */
struct ChunkerConfig {
    size_t min_chunk_size;    // Minimum chunk size
    size_t avg_chunk_size;    // Target average chunk size
    size_t max_chunk_size;    // Maximum chunk size
    
    // Default: Target 1MB average chunks
    ChunkerConfig()
        : min_chunk_size(256 * 1024)      // 256 KB min
        , avg_chunk_size(1024 * 1024)     // 1 MB average
        , max_chunk_size(4 * 1024 * 1024) // 4 MB max
    {}
    
    ChunkerConfig(size_t min_sz, size_t avg_sz, size_t max_sz)
        : min_chunk_size(min_sz)
        , avg_chunk_size(avg_sz)
        , max_chunk_size(max_sz)
    {}
};

/**
 * @brief FastCDC-style Content-Defined Chunker.
 * 
 * Uses a simplified rolling hash to find chunk boundaries based on
 * content patterns, providing deduplication-friendly chunking.
 */
class Chunker {
public:
    explicit Chunker(const ChunkerConfig& config = ChunkerConfig());
    ~Chunker() = default;
    
    // Non-copyable, movable
    Chunker(const Chunker&) = delete;
    Chunker& operator=(const Chunker&) = delete;
    Chunker(Chunker&&) = default;
    Chunker& operator=(Chunker&&) = default;
    
    /**
     * @brief Process a buffer and extract chunks.
     * 
     * @param data       Pointer to the data buffer
     * @param size       Size of the data buffer
     * @param base_offset Base offset in the file (for absolute positioning)
     * @return Vector of chunks found in the buffer
     * 
     * @note The returned Chunk::data_ptr points into the original buffer.
     *       Caller must ensure buffer lifetime exceeds chunk usage.
     */
    std::vector<Chunk> process(const char* data, size_t size, uint64_t base_offset = 0);
    
    /**
     * @brief Process with callback for streaming/memory efficiency.
     * 
     * @param data       Pointer to the data buffer
     * @param size       Size of the data buffer
     * @param base_offset Base offset in the file
     * @param callback   Called for each chunk found
     */
    void process_streaming(const char* data, size_t size, uint64_t base_offset,
                          std::function<void(const Chunk&)> callback);
    
    /**
     * @brief Get the current configuration.
     */
    const ChunkerConfig& config() const { return config_; }
    
private:
    ChunkerConfig config_;
    
    // Rolling hash state
    uint64_t mask_s_;  // Mask for small (normal) boundary detection
    uint64_t mask_l_;  // Mask for large (backup) boundary detection
    
    // Gear hash table for fast rolling hash
    static const uint64_t GEAR_TABLE[256];
    
    /**
     * @brief Find the next chunk boundary using FastCDC algorithm.
     * 
     * @param data Pointer to data
     * @param size Size of remaining data
     * @return Chunk length
     */
    size_t find_boundary(const char* data, size_t size);
    
    /**
     * @brief Calculate gear hash for rolling window.
     */
    inline uint64_t gear_hash(uint64_t hash, uint8_t byte) const {
        return (hash << 1) + GEAR_TABLE[byte];
    }
};

} // namespace hyper

#endif // CHUNKING_H
