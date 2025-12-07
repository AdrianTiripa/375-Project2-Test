// LRU cache implementation as required by the project description.

#include "cache.h"
#include <fstream>   // for ofstream, used in dump()

using namespace std;

// Simple integer log2 (floor) for positive x.
// Assumes x >= 1.
static uint64_t intLog2(uint64_t x) {
    uint64_t res = 0;
    while (x > 1) {
        x >>= 1;
        ++res;
    }
    return res;
}

// Constructor definition
Cache::Cache(CacheConfig configParam, CacheDataType cacheType)
    : hits(0),
      misses(0),
      type(cacheType),
      time(0),
      config(configParam),
      numSets(0),
      numBlocks(0) {

    // Number of blocks in the cache
    numBlocks = config.cacheSize / config.blockSize;

    // Number of sets = number of blocks / ways
    numSets = (config.ways == 0) ? 0 : (numBlocks / config.ways);

    // Defensive: if something is misconfigured, avoid division by zero or empty vectors
    if (numSets == 0 || config.ways == 0) {
        // Leave vectors empty; any access should be guarded by correct config in tests.
        return;
    }

    // Allocate tag array: [set][way]
    cacheArray.assign(numSets, vector<uint64_t>(config.ways, 0));

    // Valid bits for each block
    validBits.assign(numSets, vector<bool>(config.ways, false));

    // LRU counters for each block
    LRUCounter.assign(numSets, vector<uint64_t>(config.ways, 0));
}

// Access method definition
bool Cache::access(uint64_t address, CacheOperation readWrite) {
    // For this project, we treat reads and writes the same from the
    // cache's perspective (write-allocate, write-through).
    (void)readWrite;

    if (numSets == 0 || config.ways == 0 || config.blockSize == 0) {
        // Misconfigured cache; treat as always miss (defensive)
        ++misses;
        return false;
    }

    // Compute number of bits for block offset and index.
    // blockSize is guaranteed to be a power of 2.
    uint64_t blockOffsetBits = intLog2(config.blockSize);
    uint64_t indexBits       = (numSets > 1) ? intLog2(numSets) : 0;

    uint64_t blockOffsetMask = (blockOffsetBits == 0) ? 0ULL : ((1ULL << blockOffsetBits) - 1ULL);
    uint64_t indexMask       = (indexBits == 0) ? 0ULL : ((1ULL << indexBits) - 1ULL);

    // Extract index and tag from address
    uint64_t blockOffset = address & blockOffsetMask;
    (void)blockOffset; // Not used, but kept for clarity / debugging.

    uint64_t index = (indexBits == 0) ? 0ULL : ((address >> blockOffsetBits) & indexMask);
    uint64_t tag   = address >> (blockOffsetBits + indexBits);

    // Sanity: index should always be in range due to how indexBits is chosen
    if (index >= numSets) {
        // Defensive: if configuration is weird, clamp index (should not happen with valid configs)
        index = index % numSets;
    }

    // 1) Look for hit in the corresponding set
    for (uint64_t way = 0; way < config.ways; ++way) {
        if (validBits[index][way] && cacheArray[index][way] == tag) {
            // Hit
            ++hits;
            // Update LRU for this block
            LRUCounter[index][way] = ++time;
            return true;
        }
    }

    // 2) Miss: need to install the block (write-allocate) and pick an LRU victim.
    ++misses;

    // First, check for an invalid block to fill.
    for (uint64_t way = 0; way < config.ways; ++way) {
        if (!validBits[index][way]) {
            cacheArray[index][way] = tag;
            validBits[index][way]  = true;
            LRUCounter[index][way] = ++time;
            return false;
        }
    }

    // 3) All blocks are valid; choose the LRU block (smallest time).
    uint64_t lruWay    = 0;
    uint64_t leastTime = LRUCounter[index][0];

    for (uint64_t way = 1; way < config.ways; ++way) {
        if (LRUCounter[index][way] < leastTime) {
            leastTime = LRUCounter[index][way];
            lruWay    = way;
        }
    }

    // Replace the LRU block
    cacheArray[index][lruWay] = tag;
    validBits[index][lruWay]  = true;
    LRUCounter[index][lruWay] = ++time;

    return false;
}

void Cache::invalidate(uint64_t address) {
    if (numSets == 0 || config.ways == 0 || config.blockSize == 0) {
        return;
    }

    uint64_t blockOffsetBits = intLog2(config.blockSize);
    uint64_t indexBits       = (numSets > 1) ? intLog2(numSets) : 0;

    uint64_t blockOffsetMask = (blockOffsetBits == 0) ? 0ULL : ((1ULL << blockOffsetBits) - 1ULL);
    uint64_t indexMask       = (indexBits == 0) ? 0ULL : ((1ULL << indexBits) - 1ULL);

    uint64_t blockOffset = address & blockOffsetMask;
    (void)blockOffset;

    uint64_t index = (indexBits == 0) ? 0ULL : ((address >> blockOffsetBits) & indexMask);
    uint64_t tag   = address >> (blockOffsetBits + indexBits);

    if (index >= numSets) {
        index = index % numSets;
    }

    for (uint64_t way = 0; way < config.ways; ++way) {
        if (validBits[index][way] && cacheArray[index][way] == tag) {
            validBits[index][way] = false; // Invalidate the block
            break;
        }
    }
}

// debug: dump information as you need; this is not used for grading
Status Cache::dump(const std::string& base_output_name) {
    ofstream cache_out(base_output_name + "_cache_state.out");
    if (cache_out) {
        cache_out << "---------------------" << endl;
        cache_out << "Begin Cache State" << endl;
        cache_out << "---------------------" << endl;
        cache_out << "Cache Configuration:" << endl;
        cache_out << "  Size:         " << config.cacheSize   << " bytes"  << endl;
        cache_out << "  Block Size:   " << config.blockSize  << " bytes"  << endl;
        cache_out << "  Ways:         " << config.ways       << endl;
        cache_out << "  Miss Latency: " << config.missLatency << " cycles" << endl;
        cache_out << "Hits:   " << hits   << endl;
        cache_out << "Misses: " << misses << endl;
        cache_out << "---------------------" << endl;
        cache_out << "End Cache State" << endl;
        cache_out << "---------------------" << endl;
        return SUCCESS;
    } else {
        cerr << LOG_ERROR << "Could not create cache state dump file" << endl;
        return ERROR;
    }
}