#include "hyper_protocol.h"
#include "compressor.h"

#include <iostream>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <chrono>
#include <iomanip>
#include <xxhash.h>

// POSIX headers for low-level I/O
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

// ============================================================================
// HYPER-TRANSFER v2.0 - Receiver Executable
// ============================================================================
// Reads HyperHeader + compressed payload from stdin, decompresses,
// and uses pwrite() for random-access file reconstruction.
// ============================================================================

namespace {

/**
 * @brief Read exactly 'size' bytes from stdin.
 * 
 * @param buffer Destination buffer
 * @param size   Number of bytes to read
 * @return true if all bytes read, false on EOF or error
 */
bool read_exact(char* buffer, size_t size) {
    size_t total_read = 0;
    while (total_read < size) {
        std::cin.read(buffer + total_read, 
                      static_cast<std::streamsize>(size - total_read));
        size_t bytes_read = static_cast<size_t>(std::cin.gcount());
        
        if (bytes_read == 0) {
            // EOF or error
            return false;
        }
        total_read += bytes_read;
    }
    return true;
}

/**
 * @brief Open output file with appropriate flags.
 * 
 * Uses standard buffered I/O. O_DIRECT requires aligned buffers and offsets
 * which would complicate the implementation for minimal gain in this use case.
 */
int open_output_file(const char* path, bool& using_direct_io) {
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    using_direct_io = false;
    int fd = ::open(path, flags, 0644);
    return fd;
}

/**
 * @brief Print progress to stderr.
 */
void print_progress(size_t bytes_written, size_t packets_received, 
                    double elapsed_sec) {
    double throughput_mbps = (bytes_written / (1024.0 * 1024.0)) / elapsed_sec;
    
    std::cerr << "\r[PROGRESS] "
              << packets_received << " packets | "
              << std::fixed << std::setprecision(2) 
              << (bytes_written / (1024.0 * 1024.0)) << " MB | "
              << throughput_mbps << " MB/s"
              << std::flush;
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    // ========================================================================
    // Parse command line arguments
    // ========================================================================
    if (argc < 2) {
        std::cerr << "HYPER-TRANSFER v2.0 Receiver\n";
        std::cerr << "Usage: " << argv[0] << " <output_file_path>\n";
        std::cerr << "\nInput: Binary stream from stdin (Header + Compressed Data)\n";
        return 1;
    }
    
    const char* output_path = argv[1];
    
    // ========================================================================
    // Open output file with low-level I/O
    // ========================================================================
    bool using_direct_io = false;
    int fd = open_output_file(output_path, using_direct_io);
    
    if (fd < 0) {
        std::cerr << "[ERROR] Cannot open output file: " << output_path 
                  << " (" << strerror(errno) << ")\n";
        return 1;
    }
    
    std::cerr << "[INFO] Output file: " << output_path << "\n";
    std::cerr << "[INFO] I/O mode: " << (using_direct_io ? "O_DIRECT" : "Buffered") << "\n";
    
    // ========================================================================
    // Initialize components
    // ========================================================================
    hyper::Compressor decompressor(hyper::CompressionLevel::FAST);

    // Dedup cache: hash -> original chunk data
    std::unordered_map<uint64_t, std::vector<char>> dedup_cache;
    dedup_cache.reserve(16384);
    
    // Buffers for reading
    std::vector<char> header_buf(sizeof(HyperHeader));
    std::vector<char> compressed_buf;
    compressed_buf.reserve(8 * 1024 * 1024);  // 8 MB initial capacity
    
    // Set stdin to binary mode
    std::ios_base::sync_with_stdio(false);
    
    // ========================================================================
    // Statistics tracking
    // ========================================================================
    size_t total_packets = 0;
    size_t total_bytes_compressed = 0;
    size_t total_bytes_written = 0;
    size_t max_offset_seen = 0;
    size_t error_count = 0;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    std::cerr << "[INFO] Starting receive pipeline...\n";
    std::cerr << "─────────────────────────────────────────\n";
    
    // ========================================================================
    // Main event loop: Read from stdin
    // ========================================================================
    while (true) {
        // --------------------------------------------------------------------
        // Step 1: Read header
        // --------------------------------------------------------------------
        if (!read_exact(header_buf.data(), sizeof(HyperHeader))) {
            // EOF - normal termination
            if (std::cin.eof()) {
                std::cerr << "\n[INFO] EOF received, finishing...\n";
                break;
            }
            std::cerr << "\n[ERROR] Failed to read header\n";
            error_count++;
            break;
        }
        
        // Parse header
        HyperHeader header;
        std::memcpy(&header, header_buf.data(), sizeof(HyperHeader));
        
        // --------------------------------------------------------------------
        // Step 2: Validate magic number
        // --------------------------------------------------------------------
        if (!validate_header(header)) {
            std::cerr << "\n[ERROR] Invalid magic number at packet #" 
                      << total_packets << ": got '" 
                      << header.magic[0] << header.magic[1] << "'\n";
            error_count++;
            // Try to recover by skipping? For now, abort.
            break;
        }
        
        // Validate packet type
        if (header.type != TYPE_LZ4_DATA && header.type != TYPE_DEDUP_HASH) {
            std::cerr << "\n[ERROR] Unknown packet type: 0x" 
                      << std::hex << static_cast<int>(header.type) << std::dec << "\n";
            error_count++;
            break;
        }
        
        // --------------------------------------------------------------------
        // Step 3: Read compressed payload
        // --------------------------------------------------------------------
        if (header.length > compressed_buf.capacity()) {
            compressed_buf.reserve(header.length * 2);
        }
        compressed_buf.resize(header.length);
        
        if (!read_exact(compressed_buf.data(), header.length)) {
            std::cerr << "\n[ERROR] Failed to read payload for packet #" 
                      << total_packets << "\n";
            error_count++;
            break;
        }
        
        total_bytes_compressed += header.length;
        
        // --------------------------------------------------------------------
        // Step 4: Handle packet by type
        // --------------------------------------------------------------------
        if (header.type == TYPE_LZ4_DATA) {
            // Decompress the payload
            hyper::DecompressResult decompressed;
            try {
                decompressed = decompressor.decompress(
                    compressed_buf.data(),
                    header.length,
                    header.original_length
                );
            } catch (const hyper::CompressionError& e) {
                std::cerr << "\n[ERROR] Decompression failed at packet #" 
                          << total_packets << ": " << e.what() << "\n";
                error_count++;
                continue;  // Try to continue with next packet
            }
            // Compute hash for dedup cache (must match sender's XXH3)
            uint64_t hash = XXH3_64bits(decompressed.data.data(), decompressed.size);
            dedup_cache.emplace(hash, decompressed.data);
            
            // ----------------------------------------------------------------
            // Step 5: Write to file at exact offset using pwrite()
            // ----------------------------------------------------------------
            ssize_t written = ::pwrite(
                fd,
                decompressed.data.data(),
                decompressed.size,
                static_cast<off_t>(header.offset)
            );
            
            if (written < 0) {
                std::cerr << "\n[ERROR] pwrite() failed at offset " 
                          << header.offset << ": " << strerror(errno) << "\n";
                error_count++;
                continue;
            }
            
            if (static_cast<size_t>(written) != decompressed.size) {
                std::cerr << "\n[WARN] Partial write at offset " << header.offset 
                          << ": expected " << decompressed.size 
                          << ", wrote " << written << "\n";
            }
            
            total_bytes_written += static_cast<size_t>(written);
            
            // Track max offset for file size verification
            size_t end_offset = header.offset + decompressed.size;
            if (end_offset > max_offset_seen) {
                max_offset_seen = end_offset;
            }
            
        } else if (header.type == TYPE_DEDUP_HASH) {
            // Deduplication: payload contains hash, fetch original chunk from cache
            if (header.length != sizeof(uint64_t)) {
                std::cerr << "\n[ERROR] DEDUP packet has unexpected length: "
                          << header.length << "\n";
                error_count++;
                continue;
            }

            uint64_t hash = 0;
            std::memcpy(&hash, compressed_buf.data(), sizeof(uint64_t));

            auto it = dedup_cache.find(hash);
            if (it == dedup_cache.end()) {
                std::cerr << "\n[ERROR] DEDUP hash not found in cache: 0x"
                          << std::hex << hash << std::dec << "\n";
                error_count++;
                continue;
            }

            const auto& cached = it->second;
            if (cached.size() != header.original_length) {
                std::cerr << "\n[WARN] DEDUP size mismatch: header="
                          << header.original_length << " cached="
                          << cached.size() << "\n";
            }

            ssize_t written = ::pwrite(
                fd,
                cached.data(),
                cached.size(),
                static_cast<off_t>(header.offset)
            );

            if (written < 0) {
                std::cerr << "\n[ERROR] pwrite() failed for DEDUP at offset "
                          << header.offset << ": " << strerror(errno) << "\n";
                error_count++;
                continue;
            }

            total_bytes_written += static_cast<size_t>(written);

            size_t end_offset = header.offset + cached.size();
            if (end_offset > max_offset_seen) {
                max_offset_seen = end_offset;
            }

            std::cerr << "\n[DEDUP] Reused cached chunk hash=0x" << std::hex << hash
                      << std::dec << " at offset " << header.offset << "\n";
        }
        
        total_packets++;
        
        // Print progress periodically
        if (total_packets % 10 == 0) {
            auto now = std::chrono::high_resolution_clock::now();
            double elapsed = std::chrono::duration<double>(now - start_time).count();
            print_progress(total_bytes_written, total_packets, elapsed);
        }
    }
    
    // ========================================================================
    // Finalize: Sync and close file
    // ========================================================================
    if (::fsync(fd) < 0) {
        std::cerr << "[WARN] fsync() failed: " << strerror(errno) << "\n";
    }
    
    if (::close(fd) < 0) {
        std::cerr << "[ERROR] close() failed: " << strerror(errno) << "\n";
    }
    
    // ========================================================================
    // Final statistics
    // ========================================================================
    auto end_time = std::chrono::high_resolution_clock::now();
    double total_elapsed = std::chrono::duration<double>(end_time - start_time).count();
    
    std::cerr << "\n─────────────────────────────────────────\n";
    std::cerr << "[DONE] Receive complete!\n";
    std::cerr << "[STATS] Total packets received:  " << total_packets << "\n";
    std::cerr << "[STATS] Compressed bytes read:   " << total_bytes_compressed << "\n";
    std::cerr << "[STATS] Bytes written to file:   " << total_bytes_written << "\n";
    std::cerr << "[STATS] Reconstructed file size: " << max_offset_seen << " bytes\n";
    std::cerr << "[STATS] Errors encountered:      " << error_count << "\n";
    std::cerr << "[STATS] Elapsed time:            " 
              << std::fixed << std::setprecision(3) << total_elapsed << " sec\n";
    std::cerr << "[STATS] Write throughput:        " 
              << std::setprecision(2) 
              << (total_bytes_written / (1024.0 * 1024.0)) / total_elapsed 
              << " MB/s\n";
    
    // Verify file integrity hint
    if (error_count == 0) {
        std::cerr << "[INFO] File reconstruction appears successful.\n";
        std::cerr << "[INFO] Verify with: sha256sum " << output_path << "\n";
    } else {
        std::cerr << "[WARN] " << error_count << " errors occurred during transfer!\n";
        return 1;
    }
    
    return 0;
}
