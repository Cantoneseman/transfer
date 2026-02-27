#ifndef HYPER_PROTOCOL_H
#define HYPER_PROTOCOL_H

#include <cstdint>
#include <iostream>
#include <iomanip>

// ============================================================================
// HYPER-TRANSFER v2.0 Binary Protocol Definition
// ============================================================================

// Magic Number
constexpr char HYPER_MAGIC[2] = {'H', 'T'};

// Packet Types
constexpr uint8_t TYPE_LZ4_DATA   = 0x01;  // LZ4 compressed data packet
constexpr uint8_t TYPE_DEDUP_HASH = 0x02;  // Deduplication hash packet

// ============================================================================
// Packet Header Structure (19 bytes, no padding)
// ============================================================================
#pragma pack(push, 1)
/**
 * @brief Packet header for Hyper-Transfer protocol.
 * 
 * Layout (19 bytes total):
 *   [0-1]   magic           : 2 bytes  - {'H', 'T'}
 *   [2]     type            : 1 byte   - Packet type (LZ4_DATA or DEDUP_HASH)
 *   [3-10]  offset          : 8 bytes  - Absolute file offset for pwrite
 *   [11-14] length          : 4 bytes  - Compressed payload length
 *   [15-18] original_length : 4 bytes  - Original uncompressed length
 */
struct HyperHeader {
    char     magic[2];        // Magic number: {'H', 'T'}
    uint8_t  type;            // 0x01 for LZ4_DATA, 0x02 for DEDUP_HASH
    uint64_t offset;          // Absolute file offset for pwrite
    uint32_t length;          // Compressed payload length in bytes
    uint32_t original_length; // Original uncompressed length in bytes
};
#pragma pack(pop)

// Compile-time size check
static_assert(sizeof(HyperHeader) == 19, "HyperHeader must be exactly 19 bytes");

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Debug helper to print header contents.
 * 
 * @param header The HyperHeader structure to print.
 */
inline void print_header(const HyperHeader& header) {
    std::cout << "┌─────────────────────────────────────────┐\n";
    std::cout << "│         HyperHeader Debug Dump          │\n";
    std::cout << "├─────────────────────────────────────────┤\n";
    std::cout << "│ Magic  : " << header.magic[0] << header.magic[1] 
              << "                              │\n";
    std::cout << "│ Type   : 0x" << std::hex << std::setw(2) << std::setfill('0') 
              << static_cast<int>(header.type) << std::dec;
    if (header.type == TYPE_LZ4_DATA) {
        std::cout << " (LZ4_DATA)                 │\n";
    } else if (header.type == TYPE_DEDUP_HASH) {
        std::cout << " (DEDUP_HASH)               │\n";
    } else {
        std::cout << " (UNKNOWN)                  │\n";
    }
    std::cout << "│ Offset : " << std::setw(20) << std::setfill(' ') 
              << header.offset << "          │\n";
    std::cout << "│ Length : " << std::setw(20) << std::setfill(' ') 
              << header.length << "          │\n";
    std::cout << "│ OrigLen: " << std::setw(20) << std::setfill(' ') 
              << header.original_length << "          │\n";
    std::cout << "└─────────────────────────────────────────┘" << std::endl;
}

/**
 * @brief Initialize a HyperHeader with default magic bytes.
 * 
 * @param type            Packet type (TYPE_LZ4_DATA or TYPE_DEDUP_HASH)
 * @param offset          File offset for pwrite
 * @param length          Compressed payload length
 * @param original_length Original uncompressed length
 * @return Initialized HyperHeader
 */
inline HyperHeader make_header(uint8_t type, uint64_t offset, uint32_t length, uint32_t original_length) {
    HyperHeader hdr;
    hdr.magic[0]        = HYPER_MAGIC[0];
    hdr.magic[1]        = HYPER_MAGIC[1];
    hdr.type            = type;
    hdr.offset          = offset;
    hdr.length          = length;
    hdr.original_length = original_length;
    return hdr;
}

/**
 * @brief Validate a HyperHeader's magic number.
 * 
 * @param header The header to validate
 * @return true if magic number matches, false otherwise
 */
inline bool validate_header(const HyperHeader& header) {
    return (header.magic[0] == HYPER_MAGIC[0] && 
            header.magic[1] == HYPER_MAGIC[1]);
}

#endif // HYPER_PROTOCOL_H
