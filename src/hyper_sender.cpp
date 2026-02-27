#include "hyper_protocol.h"
#include "chunking.h"
#include "compressor.h"
#include "safe_queue.hpp"

#include <xxhash.h>

#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <atomic>
#include <unordered_set>
#include <chrono>
#include <iomanip>
#include <cstring>

// ============================================================================
// HYPER-TRANSFER v2.0 - High-Performance Multi-threaded Sender
// ============================================================================
// Pipeline Architecture:
//
//   ┌──────────┐     ┌──────────┐     ┌──────────┐     ┌──────────┐
//   │  Reader  │────►│ Chunker  │────►│  Dedup   │────►│  Sender  │──► stdout
//   │  Thread  │     │  Thread  │     │  Thread  │     │  (Main)  │
//   └──────────┘     └──────────┘     └──────────┘     └──────────┘
//        │                │                │                │
//        ▼                ▼                ▼                ▼
//    raw_queue       chunk_queue      send_queue       Binary Output
//
// ============================================================================

namespace {

// ============================================================================
// Configuration Constants
// ============================================================================
constexpr size_t READ_BLOCK_SIZE = 4 * 1024 * 1024;   // 4 MB read blocks
constexpr size_t RAW_QUEUE_SIZE = 8;                   // Max buffered raw blocks
constexpr size_t CHUNK_QUEUE_SIZE = 32;                // Max buffered chunks
constexpr size_t SEND_QUEUE_SIZE = 32;                 // Max buffered processed chunks

// ============================================================================
// Data Structures for Pipeline Stages
// ============================================================================

/**
 * @brief Raw data block read from file.
 */
struct RawBlock {
    std::vector<char> data;    // Raw file data
    uint64_t offset;           // Starting offset in file
    
    RawBlock() : offset(0) {}
    RawBlock(std::vector<char>&& d, uint64_t off) 
        : data(std::move(d)), offset(off) {}
};

/**
 * @brief Chunk extracted from raw block, ready for hashing.
 */
struct ChunkData {
    std::vector<char> data;    // Chunk data (owns the memory)
    uint64_t offset;           // Absolute file offset
    uint32_t length;           // Data length
    
    ChunkData() : offset(0), length(0) {}
    ChunkData(const char* ptr, uint32_t len, uint64_t off)
        : data(ptr, ptr + len), offset(off), length(len) {}
};

/**
 * @brief Processed chunk ready for sending.
 */
struct ProcessedChunk {
    uint64_t offset;           // File offset
    std::vector<char> data;    // Original chunk data (for compression)
    uint64_t hash;             // XXH3 hash
    bool is_duplicate;         // True if hash was seen before
    
    ProcessedChunk() : offset(0), hash(0), is_duplicate(false) {}
};

// ============================================================================
// Statistics Tracking
// ============================================================================
struct PipelineStats {
    std::atomic<uint64_t> bytes_read{0};
    std::atomic<uint64_t> bytes_chunked{0};
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> bytes_compressed{0};
    std::atomic<uint64_t> chunks_total{0};
    std::atomic<uint64_t> chunks_duplicate{0};
    std::atomic<uint64_t> chunks_unique{0};
};

// ============================================================================
// Pipeline Stage: Reader Thread
// ============================================================================
void reader_thread(
    const std::string& file_path,
    hyper::SafeQueue<RawBlock>& raw_queue,
    std::atomic<bool>& reader_done,
    PipelineStats& stats)
{
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[READER] ERROR: Cannot open file: " << file_path << "\n";
        reader_done = true;
        raw_queue.stop();
        return;
    }
    
    // Get file size
    file.seekg(0, std::ios::end);
    size_t file_size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    
    std::cerr << "[READER] File size: " << file_size << " bytes\n";
    
    uint64_t offset = 0;
    std::vector<char> buffer(READ_BLOCK_SIZE);
    
    while (file.good() && offset < file_size) {
        size_t to_read = std::min(READ_BLOCK_SIZE, file_size - offset);
        file.read(buffer.data(), static_cast<std::streamsize>(to_read));
        size_t bytes_read = static_cast<size_t>(file.gcount());
        
        if (bytes_read == 0) break;
        
        // Create block with exact size
        std::vector<char> block_data(buffer.begin(), buffer.begin() + bytes_read);
        
        if (!raw_queue.push(RawBlock(std::move(block_data), offset))) {
            break;  // Queue stopped
        }
        
        stats.bytes_read += bytes_read;
        offset += bytes_read;
    }
    
    reader_done = true;
    std::cerr << "[READER] Done. Total read: " << stats.bytes_read.load() << " bytes\n";
}

// ============================================================================
// Pipeline Stage: Chunker Thread
// ============================================================================
void chunker_thread(
    hyper::SafeQueue<RawBlock>& raw_queue,
    hyper::SafeQueue<ChunkData>& chunk_queue,
    std::atomic<bool>& reader_done,
    std::atomic<bool>& chunker_done,
    PipelineStats& stats)
{
    hyper::ChunkerConfig config;
    hyper::Chunker chunker(config);
    
    RawBlock block;
    while (true) {
        // Try to pop with timeout for responsive shutdown
        if (!raw_queue.pop_with_timeout(block, 100)) {
            if (reader_done && raw_queue.empty()) {
                break;  // All done
            }
            continue;
        }
        
        // Process block into chunks
        auto chunks = chunker.process(block.data.data(), block.data.size(), block.offset);
        
        for (const auto& chunk : chunks) {
            // Copy chunk data since the block will be reused
            ChunkData chunk_data(chunk.data_ptr, chunk.length, chunk.offset);
            
            if (!chunk_queue.push(std::move(chunk_data))) {
                break;  // Queue stopped
            }
            
            stats.bytes_chunked += chunk.length;
            stats.chunks_total++;
        }
    }
    
    chunker_done = true;
    std::cerr << "[CHUNKER] Done. Total chunks: " << stats.chunks_total.load() << "\n";
}

// ============================================================================
// Pipeline Stage: Dedup/Hash Thread
// ============================================================================
void dedup_thread(
    hyper::SafeQueue<ChunkData>& chunk_queue,
    hyper::SafeQueue<ProcessedChunk>& send_queue,
    std::atomic<bool>& chunker_done,
    std::atomic<bool>& dedup_done,
    PipelineStats& stats)
{
    std::unordered_set<uint64_t> seen_hashes;
    seen_hashes.reserve(10000);  // Pre-allocate for performance
    
    ChunkData chunk;
    while (true) {
        if (!chunk_queue.pop_with_timeout(chunk, 100)) {
            if (chunker_done && chunk_queue.empty()) {
                break;
            }
            continue;
        }
        
        // Compute XXH3 hash (extremely fast)
        uint64_t hash = XXH3_64bits(chunk.data.data(), chunk.data.size());
        
        // Check for duplicate
        bool is_dup = (seen_hashes.find(hash) != seen_hashes.end());
        
        if (!is_dup) {
            seen_hashes.insert(hash);
            stats.chunks_unique++;
        } else {
            stats.chunks_duplicate++;
        }
        
        // Create processed chunk
        ProcessedChunk processed;
        processed.offset = chunk.offset;
        processed.data = std::move(chunk.data);
        processed.hash = hash;
        processed.is_duplicate = is_dup;
        
        if (!send_queue.push(std::move(processed))) {
            break;
        }
    }
    
    dedup_done = true;
    std::cerr << "[DEDUP] Done. Unique: " << stats.chunks_unique.load() 
              << ", Duplicates: " << stats.chunks_duplicate.load() << "\n";
}

// ============================================================================
// Pipeline Stage: Sender (Main Thread)
// ============================================================================
void sender_main(
    hyper::SafeQueue<ProcessedChunk>& send_queue,
    std::atomic<bool>& dedup_done,
    PipelineStats& stats)
{
    hyper::Compressor compressor(hyper::CompressionLevel::FAST);
    
    // Combined buffer for atomic writes (header + payload)
    std::vector<char> output_buffer;
    output_buffer.reserve(8 * 1024 * 1024);  // 8 MB buffer
    
    ProcessedChunk chunk;
    while (true) {
        if (!send_queue.pop_with_timeout(chunk, 100)) {
            if (dedup_done && send_queue.empty()) {
                break;
            }
            continue;
        }
        
        output_buffer.clear();
        
        if (chunk.is_duplicate) {
            // Send DEDUP_HASH packet (hash only, no data)
            HyperHeader header = make_header(
                TYPE_DEDUP_HASH,
                chunk.offset,
                sizeof(uint64_t),           // Payload is the hash
                static_cast<uint32_t>(chunk.data.size())  // Original size
            );
            
            // Combine header + hash into single buffer for atomic write
            output_buffer.resize(sizeof(HyperHeader) + sizeof(uint64_t));
            std::memcpy(output_buffer.data(), &header, sizeof(HyperHeader));
            std::memcpy(output_buffer.data() + sizeof(HyperHeader), &chunk.hash, sizeof(uint64_t));
            
            std::cout.write(output_buffer.data(), static_cast<std::streamsize>(output_buffer.size()));
            
            stats.bytes_sent += output_buffer.size();
        } else {
            // Compress and send DATA packet
            try {
                auto compressed = compressor.compress(chunk.data.data(), chunk.data.size());
                
                HyperHeader header = make_header(
                    TYPE_LZ4_DATA,
                    chunk.offset,
                    static_cast<uint32_t>(compressed.compressed_size),
                    static_cast<uint32_t>(chunk.data.size())
                );
                
                // Combine header + compressed data into single buffer for atomic write
                size_t total_size = sizeof(HyperHeader) + compressed.compressed_size;
                output_buffer.resize(total_size);
                std::memcpy(output_buffer.data(), &header, sizeof(HyperHeader));
                std::memcpy(output_buffer.data() + sizeof(HyperHeader), 
                           compressed.data.data(), compressed.compressed_size);
                
                std::cout.write(output_buffer.data(), static_cast<std::streamsize>(total_size));
                
                stats.bytes_sent += total_size;
                stats.bytes_compressed += compressed.compressed_size;
                
            } catch (const hyper::CompressionError& e) {
                std::cerr << "[SENDER] Compression error: " << e.what() << "\n";
            }
        }
    }
    
    std::cout.flush();
    std::cerr << "[SENDER] Done. Total sent: " << stats.bytes_sent.load() << " bytes\n";
}

/**
 * @brief Get file size.
 */
size_t get_file_size(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return 0;
    return static_cast<size_t>(file.tellg());
}

} // anonymous namespace

// ============================================================================
// Main Entry Point
// ============================================================================
int main(int argc, char* argv[]) {
    // Disable sync for maximum throughput
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(nullptr);
    
    // Parse arguments
    if (argc < 2) {
        std::cerr << "HYPER-TRANSFER v2.0 Multi-threaded Sender\n";
        std::cerr << "Usage: " << argv[0] << " <input_file_path>\n";
        std::cerr << "\nPipeline: Reader -> Chunker -> Dedup -> Sender -> stdout\n";
        return 1;
    }
    
    const std::string file_path = argv[1];
    size_t file_size = get_file_size(file_path);
    
    if (file_size == 0) {
        std::cerr << "[ERROR] Cannot open or empty file: " << file_path << "\n";
        return 1;
    }
    
    // Print banner
    std::cerr << "════════════════════════════════════════════════════════════\n";
    std::cerr << "  HYPER-TRANSFER v2.0 - Multi-threaded Pipeline Sender\n";
    std::cerr << "════════════════════════════════════════════════════════════\n";
    std::cerr << "  File: " << file_path << "\n";
    std::cerr << "  Size: " << file_size << " bytes (" 
              << std::fixed << std::setprecision(2) 
              << (file_size / (1024.0 * 1024.0)) << " MB)\n";
    std::cerr << "  Read Block: " << (READ_BLOCK_SIZE / 1024 / 1024) << " MB\n";
    std::cerr << "════════════════════════════════════════════════════════════\n";
    
    // Create queues
    hyper::SafeQueue<RawBlock> raw_queue;
    hyper::SafeQueue<ChunkData> chunk_queue;
    hyper::SafeQueue<ProcessedChunk> send_queue;
    
    // Control flags
    std::atomic<bool> reader_done{false};
    std::atomic<bool> chunker_done{false};
    std::atomic<bool> dedup_done{false};
    
    // Statistics
    PipelineStats stats;
    
    // Start timer
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Launch pipeline threads
    std::cerr << "[MAIN] Starting pipeline threads...\n";
    
    std::thread reader(reader_thread, 
                       std::ref(file_path), 
                       std::ref(raw_queue), 
                       std::ref(reader_done),
                       std::ref(stats));
    
    std::thread chunker(chunker_thread,
                        std::ref(raw_queue),
                        std::ref(chunk_queue),
                        std::ref(reader_done),
                        std::ref(chunker_done),
                        std::ref(stats));
    
    std::thread dedup(dedup_thread,
                      std::ref(chunk_queue),
                      std::ref(send_queue),
                      std::ref(chunker_done),
                      std::ref(dedup_done),
                      std::ref(stats));
    
    // Sender runs on main thread
    sender_main(send_queue, dedup_done, stats);
    
    // Wait for all threads
    reader.join();
    chunker.join();
    dedup.join();
    
    // Calculate elapsed time
    auto end_time = std::chrono::high_resolution_clock::now();
    double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();
    
    // Print final statistics
    std::cerr << "\n════════════════════════════════════════════════════════════\n";
    std::cerr << "  TRANSFER COMPLETE\n";
    std::cerr << "════════════════════════════════════════════════════════════\n";
    std::cerr << "  Bytes Read:       " << stats.bytes_read.load() << "\n";
    std::cerr << "  Bytes Sent:       " << stats.bytes_sent.load() << "\n";
    std::cerr << "  Bytes Compressed: " << stats.bytes_compressed.load() << "\n";
    std::cerr << "  Chunks Total:     " << stats.chunks_total.load() << "\n";
    std::cerr << "  Chunks Unique:    " << stats.chunks_unique.load() << "\n";
    std::cerr << "  Chunks Duplicate: " << stats.chunks_duplicate.load() << "\n";
    
    if (stats.chunks_unique.load() > 0) {
        double compression_ratio = static_cast<double>(stats.bytes_compressed.load()) / 
                                   stats.bytes_read.load() * 100.0;
        std::cerr << "  Compression:      " << std::fixed << std::setprecision(1) 
                  << compression_ratio << "%\n";
    }
    
    double dedup_ratio = stats.chunks_total.load() > 0 
        ? static_cast<double>(stats.chunks_duplicate.load()) / stats.chunks_total.load() * 100.0 
        : 0.0;
    std::cerr << "  Dedup Rate:       " << std::fixed << std::setprecision(1) 
              << dedup_ratio << "%\n";
    
    std::cerr << "────────────────────────────────────────────────────────────\n";
    std::cerr << "  Elapsed Time:     " << std::fixed << std::setprecision(3) 
              << elapsed_sec << " sec\n";
    std::cerr << "  Read Throughput:  " << std::setprecision(2) 
              << (stats.bytes_read.load() / (1024.0 * 1024.0)) / elapsed_sec << " MB/s\n";
    std::cerr << "  Send Throughput:  " 
              << (stats.bytes_sent.load() / (1024.0 * 1024.0)) / elapsed_sec << " MB/s\n";
    std::cerr << "════════════════════════════════════════════════════════════\n";
    
    return 0;
}
