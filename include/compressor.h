#ifndef COMPRESSOR_H
#define COMPRESSOR_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <stdexcept>

// ============================================================================
// HYPER-TRANSFER v2.0 - LZ4 Compression Wrapper
// ============================================================================

namespace hyper {

/**
 * @brief Exception for compression errors.
 */
class CompressionError : public std::runtime_error {
public:
    explicit CompressionError(const char* msg) : std::runtime_error(msg) {}
    explicit CompressionError(const std::string& msg) : std::runtime_error(msg) {}
};

/**
 * @brief Result of a compression operation.
 */
struct CompressResult {
    std::vector<char> data;       // Compressed data
    size_t original_size;         // Original uncompressed size
    size_t compressed_size;       // Resulting compressed size
    double ratio;                 // Compression ratio (compressed/original)
    
    bool is_compressed() const { return compressed_size < original_size; }
};

/**
 * @brief Result of a decompression operation.
 */
struct DecompressResult {
    std::vector<char> data;       // Decompressed data
    size_t size;                  // Decompressed size
};

/**
 * @brief Compression level presets.
 */
enum class CompressionLevel {
    FAST = 1,           // LZ4 fast mode (default)
    DEFAULT = 0,        // LZ4 default
    HIGH = 9            // LZ4 HC high compression
};

/**
 * @brief LZ4 Compressor wrapper class.
 * 
 * Provides safe buffer handling and convenient interface
 * around the LZ4 compression library.
 */
class Compressor {
public:
    /**
     * @brief Construct compressor with specified level.
     */
    explicit Compressor(CompressionLevel level = CompressionLevel::FAST);
    ~Compressor() = default;
    
    // Non-copyable, movable
    Compressor(const Compressor&) = delete;
    Compressor& operator=(const Compressor&) = delete;
    Compressor(Compressor&&) = default;
    Compressor& operator=(Compressor&&) = default;
    
    /**
     * @brief Compress raw data.
     * 
     * @param data Pointer to raw data
     * @param size Size of raw data
     * @return CompressResult containing compressed data and stats
     * @throws CompressionError on failure
     */
    CompressResult compress(const char* data, size_t size);
    
    /**
     * @brief Compress from vector.
     */
    CompressResult compress(const std::vector<char>& data) {
        return compress(data.data(), data.size());
    }
    
    /**
     * @brief Decompress data.
     * 
     * @param compressed_data Pointer to compressed data
     * @param compressed_size Size of compressed data
     * @param original_size   Expected original size (must be known)
     * @return DecompressResult containing decompressed data
     * @throws CompressionError on failure
     */
    DecompressResult decompress(const char* compressed_data, 
                                size_t compressed_size,
                                size_t original_size);
    
    /**
     * @brief Decompress from vector.
     */
    DecompressResult decompress(const std::vector<char>& data, size_t original_size) {
        return decompress(data.data(), data.size(), original_size);
    }
    
    /**
     * @brief Get maximum compressed size for given input size.
     * 
     * Use this to pre-allocate buffers.
     */
    static size_t max_compressed_size(size_t input_size);
    
    /**
     * @brief Set compression level.
     */
    void set_level(CompressionLevel level) { level_ = level; }
    
    /**
     * @brief Get current compression level.
     */
    CompressionLevel level() const { return level_; }
    
private:
    CompressionLevel level_;
};

} // namespace hyper

#endif // COMPRESSOR_H
