// Sample cache implementation with random hit return
// TODO: Modify this file to model an LRU cache as in the project description

#include "cache.h"
#include <random>
#include <vector>
#include <cmath>    // for log2

using namespace std;

// Random generator for cache hit/miss simulation
static std::mt19937 generator(42);  // Fixed seed for deterministic results
std::uniform_real_distribution<double> distribution(0.0, 1.0);

// Constructor definition
Cache::Cache(CacheConfig configParam, CacheDataType cacheType) : config(configParam) {
    // Here you can initialize other cache-specific attributes
    // For instance, if you had cache tables or other structures, initialize them here
    

    hits  = 0;
    misses = 0;
    type  = cacheType;
    time  = 0;

    // Initialize cache structures
    numBlocks = config.cacheSize / config.blockSize;
    numSets   = numBlocks / config.ways;
    
    // cacheSize/blockSize gives number of blocks
    // (cacheSize/blockSize)/ways gives number of sets
    // configParam.ways gives number of blocks per set
    cacheArray.assign(numSets, vector<uint64_t>(config.ways, 0));

    // follows from above, every entry is set to false initially
    validBits.assign(numSets, vector<bool>(config.ways, false));

    // initialize LRU counters to 0, there is one counter per set
    LRUCounter.assign(numSets, vector<uint64_t>(config.ways, 0));
}

// Access method definition
bool Cache::access(uint64_t address, CacheOperation readWrite) {

    // find tag, index, block offset sizes (sizes are powers of two)
    uint64_t blockOffsetBits = static_cast<uint64_t>(std::log2(config.blockSize));
    uint64_t indexBits       = static_cast<uint64_t>(std::log2(numSets));

    uint64_t blockOffsetMask = (blockOffsetBits == 0) ? 0ULL : ((1ULL << blockOffsetBits) - 1ULL);
    uint64_t indexMask       = (indexBits == 0) ? 0ULL : ((1ULL << indexBits) - 1ULL);

    // extract tag, index, block offset from address
    uint64_t blockOffset = address & blockOffsetMask;
    (void)blockOffset; // not used but kept for clarity / future debugging

    uint64_t index = (indexBits == 0) ? 0ULL : ((address >> blockOffsetBits) & indexMask);
    uint64_t tag   = address >> (blockOffsetBits + indexBits);

    // look for hit in the corresponding set
    for (uint64_t way = 0; way < config.ways; ++way) {
        if (validBits[index][way] && cacheArray[index][way] == tag) {
            hits++;
            // update LRU counter
            LRUCounter[index][way] = ++time;
            return true;
        }
    }

    // miss: increase miss count and replace LRU block
    misses++;

    // look for an invalid block first
    for (uint64_t way = 0; way < config.ways; ++way) {
        if (!validBits[index][way]) {
            cacheArray[index][way] = tag;
            validBits[index][way]  = true;
            LRUCounter[index][way] = ++time;
            return false;
        }
    }

    // if all blocks are valid, replace LRU block
    uint64_t LRUBlock   = 0;
    uint64_t leastTime  = LRUCounter[index][0];

    // find LRU block
    for (uint64_t way = 1; way < config.ways; ++way) {
        if (LRUCounter[index][way] < leastTime) {
            leastTime = LRUCounter[index][way];
            LRUBlock  = way;
        }
    }

    cacheArray[index][LRUBlock] = tag;
    validBits[index][LRUBlock]  = true;
    LRUCounter[index][LRUBlock] = ++time;

    return false;
}

void Cache::invalidate(uint64_t address){
    // find tag, index, block offset sizes
    uint64_t blockOffsetBits = static_cast<uint64_t>(std::log2(config.blockSize));
    uint64_t indexBits       = static_cast<uint64_t>(std::log2(numSets));

    uint64_t blockOffsetMask = (blockOffsetBits == 0) ? 0ULL : ((1ULL << blockOffsetBits) - 1ULL);
    uint64_t indexMask       = (indexBits == 0) ? 0ULL : ((1ULL << indexBits) - 1ULL);

    // extract tag, index, block offset from address
    uint64_t blockOffset = address & blockOffsetMask;
    (void)blockOffset; // not used

    uint64_t index = (indexBits == 0) ? 0ULL : ((address >> blockOffsetBits) & indexMask);
    uint64_t tag   = address >> (blockOffsetBits + indexBits);

    for (uint64_t way = 0; way < config.ways; ++way) {
        if (validBits[index][way] && cacheArray[index][way] == tag) {
            validBits[index][way] = false; // invalidate the block
            break;
        }
    }
}

// debug: dump information as you needed, here are some examples
Status Cache::dump(const std::string& base_output_name) {
    ofstream cache_out(base_output_name + "_cache_state.out");
    if (cache_out) {
        cache_out << "---------------------" << endl;
        cache_out << "Begin Register Values" << endl;
        cache_out << "---------------------" << endl;
        cache_out << "Cache Configuration:" << std::endl;
        cache_out << "Size: " << config.cacheSize << " bytes" << std::endl;
        cache_out << "Block Size: " << config.blockSize << " bytes" << std::endl;
        cache_out << "Ways: " << config.ways << std::endl;
        cache_out << "Miss Latency: " << config.missLatency << " cycles" << std::endl;
        cache_out << "---------------------" << endl;
        cache_out << "End Register Values" << endl;
        cache_out << "---------------------" << endl;
        return SUCCESS;
    } else {
        cerr << LOG_ERROR << "Could not create cache state dump file" << endl;
        return ERROR;
    }
}