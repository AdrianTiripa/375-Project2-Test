// Sample cache implementation with random hit return
// TODO: Modify this file to model an LRU cache as in the project description

#include "cache.h"
#include <random>
#include <vector>

using namespace std;

// Random generator for cache hit/miss simulation
static std::mt19937 generator(42);  // Fixed seed for deterministic results
std::uniform_real_distribution<double> distribution(0.0, 1.0);

// Constructor definition
Cache::Cache(CacheConfig configParam, CacheDataType cacheType) : config(configParam) {
    // Here you can initialize other cache-specific attributes
    // For instance, if you had cache tables or other structures, initialize them here
    

    hits=0;
    misses=0;
    type = cacheType;

    // Initialize cache structures
    numBlocks = config.cacheSize/config.blockSize;
    numSets = numBlocks/config.ways;
    

    // cacheSize/blockSize gives number of blocks
    // (cacheSize/blockSize)/ways gives number of sets
    // configParam.ways gives number of blocks per set
    cacheArray = (numSets, vector<uint64_t>(config.ways, 0));

    // follows from above, every entry is set to false initially
    validBits = (numSets, (config.ways, false));

    // initialize LRU counters to 0, there is one counter per set
    LRUCounter = (numSets, 0);

}

// Access method definition
bool Cache::access(uint64_t address, CacheOperation readWrite) {

    // find tag, index, block offset sizes
    uint64_t blockOffsetBits = log2(config.blockSize);
    uint64_t indexBits = log2(numSets); // not sure of the signature
    uint64_t tagBits = 64 - blockOffsetBits - indexBits;

    // extract tag, index, block offset from address
    uint64_t blockOffset = address & ((1 << blockOffsetBits) - 1);
    uint64_t index = (address >> blockOffsetBits) & ((1 << indexBits) - 1);
    uint64_t tag = address >> (blockOffsetBits + indexBits);

    // look for hit in the corresponding set
    bool hit = false;
    for(int i=0; i<config.ways; i++) {
        if(validBits[index][i] && cacheArray[index][i] == tag) {
            hit = true;
            hits++;
            // update LRU counter
            LRUCounter[index]++;
            if(LRUCounter[index] > (config.ways-1)) {
                LRUCounter[index] = 0; // reset if exceeds ways
            }
            break;
        }
    }

    // on miss, increase miss count and replace LRU block
    if(!hit){
        misses++;
        cacheArray[index][LRUCounter[index]] = tag;
        validBits[index][LRUCounter[index]] = true;

        // update which block is LRU in the set
        LRUCounter[index]++;
        if(LRUCounter[index] > (config.ways-1)){
            LRUCounter[index] = 0; // reset if exceeds ways
        }

    }

    // return result of cache access
    return hit;


    // For simplicity, we're using a random boolean to simulate cache hit/miss
    /*bool hit = distribution(generator) < 0.20;  // random 20% hit for a strange cache
    hits += hit;
    misses += !hit;
    return hit;*/
}


void invalidate(uint64_t address){
    // find tag, index, block offset sizes
    uint64_t blockOffsetBits = log2(config.blockSize);
    uint64_t indexBits = log2(numSets); // not sure of the signature
    uint64_t tagBits = 64 - blockOffsetBits - indexBits;

    // extract tag, index, block offset from address
    uint64_t blockOffset = address & ((1 << blockOffsetBits) - 1);
    uint64_t index = (address >> blockOffsetBits) & ((1 << indexBits) - 1);
    uint64_t tag = address >> (blockOffsetBits + indexBits);

    validBits[index][tag]=false;
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
        cache_out << "Ways: " << (config.ways == 1) << std::endl;
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
