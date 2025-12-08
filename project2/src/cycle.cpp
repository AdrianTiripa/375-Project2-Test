#include "cycle.h"

#include <iostream>
#include <memory>
#include <string>

#include "Utilities.h"
#include "cache.h"
#include "simulator.h"

static Simulator* simulator = nullptr;
static Cache* iCache        = nullptr;
static Cache* dCache        = nullptr;
static std::string output;

// Global cycle counter
static uint64_t cycleCount = 0;

// Program counter for the *next* fetch
static uint64_t PC = 0;

// Global flags / counters for control & stats
static bool halted                       = false;
static uint64_t loadUseStallCount        = 0;   // includes load-use and load-branch
static uint64_t branchStallCyclesRemain  = 0;   // extra cycles to stall a branch (for load-branch)

// Small helper: construct a microarchitectural NOP
static Simulator::Instruction nop(StageStatus status) {
    Simulator::Instruction n;
    n.instruction = 0x00000013;  // addi x0, x0, 0
    n.isLegal     = true;
    n.isNop       = true;
    n.status      = status;
    return n;
}

// Pipeline latches (state between stages)
static struct PipelineInfo {
    Simulator::Instruction ifInst  = nop(IDLE);
    Simulator::Instruction idInst  = nop(IDLE);
    Simulator::Instruction exInst  = nop(IDLE);
    Simulator::Instruction memInst = nop(IDLE);
    Simulator::Instruction wbInst  = nop(IDLE);
} pipelineInfo;

// initialize the simulator
Status initSimulator(CacheConfig& iCacheConfig, CacheConfig& dCacheConfig, MemoryStore* mem,
                     const std::string& output_name) {
    output    = output_name;
    simulator = new Simulator();
    simulator->setMemory(mem);

    iCache = new Cache(iCacheConfig, I_CACHE);
    dCache = new Cache(dCacheConfig, D_CACHE);

    // Reset global state
    cycleCount               = 0;
    PC                       = 0;
    halted                   = false;
    loadUseStallCount        = 0;
    branchStallCyclesRemain  = 0;

    pipelineInfo.ifInst  = nop(IDLE);
    pipelineInfo.idInst  = nop(IDLE);
    pipelineInfo.exInst  = nop(IDLE);
    pipelineInfo.memInst = nop(IDLE);
    pipelineInfo.wbInst  = nop(IDLE);

    return SUCCESS;
}

// run the simulator for a certain number of cycles
// return SUCCESS if reaching desired cycles.
// return HALT if the simulator halts on 0xfeedfeed
Status runCycles(uint64_t cycles) {
    Status status    = SUCCESS;
    uint64_t runSoFar = 0;

    PipeState pipeState = {0};

    // If already halted from a previous call, just dump current pipe state
    if (halted) {
        pipeState.cycle    = (cycleCount == 0 ? 0 : cycleCount - 1);
        pipeState.ifPC     = pipelineInfo.ifInst.PC;
        pipeState.ifStatus = pipelineInfo.ifInst.status;
        pipeState.idInstr  = pipelineInfo.idInst.instruction;
        pipeState.idStatus = pipelineInfo.idInst.status;
        pipeState.exInstr  = pipelineInfo.exInst.instruction;
        pipeState.exStatus = pipelineInfo.exInst.status;
        pipeState.memInstr = pipelineInfo.memInst.instruction;
        pipeState.memStatus = pipelineInfo.memInst.status;
        pipeState.wbInstr  = pipelineInfo.wbInst.instruction;
        pipeState.wbStatus = pipelineInfo.wbInst.status;
        dumpPipeState(pipeState, output);
        return HALT;
    }

    while (!halted && (cycles == 0 || runSoFar < cycles)) {
        // Snapshot current pipeline state at *start* of this cycle
        Simulator::Instruction oldIF  = pipelineInfo.ifInst;
        Simulator::Instruction oldID  = pipelineInfo.idInst;
        Simulator::Instruction oldEX  = pipelineInfo.exInst;
        Simulator::Instruction oldMEM = pipelineInfo.memInst;
        Simulator::Instruction oldWB  = pipelineInfo.wbInst;

        // This cycle number (0-based)
        pipeState.cycle = cycleCount;

        // ---- Hazard detection (simple) ----
        bool stallIF        = false;
        bool stallID        = false;
        bool insertBubbleEX = false;

        // Info about EX and ID stage at the beginning of this cycle
        bool exWritesRd = oldEX.writesRd && (oldEX.rd != 0);
        bool exIsLoad   = oldEX.readsMem && !oldEX.writesMem && exWritesRd;
        bool exIsArith  = oldEX.doesArithLogic && exWritesRd;

        bool idIsNop    = oldID.isNop;
        bool idIsBranch = (oldID.opcode == OP_BRANCH);
        bool idIsStore  = oldID.writesMem;  // store

        bool idUsesEXRd = false;
        if (!idIsNop && exWritesRd) {
            if (oldID.readsRs1 && oldID.rs1 == oldEX.rd) idUsesEXRd = true;
            if (oldID.readsRs2 && oldID.rs2 == oldEX.rd) idUsesEXRd = true;
        }

        // Ongoing branch stall from a previous load-branch hazard
        if (branchStallCyclesRemain > 0) {
            stallIF        = true;
            stallID        = true;
            insertBubbleEX = true;
            branchStallCyclesRemain--;
        } else {
            // New hazards
            if (idUsesEXRd) {
                // Load-branch: 2-cycle stall counted as one "load stall"
                if (exIsLoad && idIsBranch) {
                    stallIF        = true;
                    stallID        = true;
                    insertBubbleEX = true;
                    branchStallCyclesRemain = 1;  // this cycle + one more = 2 total
                    loadUseStallCount++;
                }
                // Load-use (non-branch) – do not stall for store, since we rely on forwarding.
                else if (exIsLoad && !idIsStore) {
                    stallIF        = true;
                    stallID        = true;
                    insertBubbleEX = true;
                    loadUseStallCount++;
                }
                // Arithmetic-branch: 1-cycle stall
                else if (exIsArith && idIsBranch) {
                    stallIF        = true;
                    stallID        = true;
                    insertBubbleEX = true;
                    // No extra cycles to remember
                }
            }
        }

        // ---- Stage computations for next cycle ----
        Simulator::Instruction nextWB  = nop(BUBBLE);
        Simulator::Instruction nextMEM = nop(BUBBLE);
        Simulator::Instruction nextEX  = nop(BUBBLE);
        Simulator::Instruction nextID  = nop(BUBBLE);
        Simulator::Instruction nextIF  = nop(IDLE);

        // WB: always try to write back whatever finished MEM last cycle
        nextWB = simulator->simWB(oldMEM);

        // MEM: take the instruction that finished EX last cycle
        // Also touch the D-cache for real loads/stores (no timing here, just stats).
        if (oldEX.readsMem || oldEX.writesMem) {
            CacheOperation op = oldEX.readsMem ? CACHE_READ : CACHE_WRITE;
            dCache->access(oldEX.memAddress, op);
        }
        nextMEM = simulator->simMEM(oldEX);

        // EX: either bubble (stall) or advance oldID
        if (insertBubbleEX) {
            nextEX = nop(BUBBLE);
        } else {
            nextEX = simulator->simEX(oldID);
        }

        // ID: either hold oldID (stall) or decode oldIF
        if (stallID) {
            nextID = oldID;  // hold in ID
        } else {
            nextID = simulator->simID(oldIF);
        }

        // ---- Branch prediction (always not taken) and PC update ----
        // Default next PC is sequential
        uint64_t newPC = PC + 4;

        bool idIsCtrl =
            (nextID.opcode == OP_BRANCH ||
             nextID.opcode == OP_JAL    ||
             nextID.opcode == OP_JALR);

        bool branchTaken = false;
        if (!nextID.isNop && nextID.isLegal && idIsCtrl) {
            // NextPC computed by decode / next-PC resolution
            if (nextID.nextPC != nextID.PC + 4) {
                branchTaken = true;
                newPC       = nextID.nextPC;
            }
        }

        // ---- IF stage: fetch if not stalled ----
        if (stallIF) {
            // Just keep oldIF in place
            nextIF = oldIF;
        } else {
            // Use I-cache for this fetch (stats only)
            iCache->access(PC, CACHE_READ);

            nextIF = simulator->simIF(PC);
            // Mark this IF instruction as speculative if there is a control op in ID
            if (idIsCtrl) {
                nextIF.status = SPECULATIVE;
            } else {
                nextIF.status = NORMAL;
            }
        }

        // If branch was taken, squash the speculative instruction we just fetched.
        // It should not make progress into ID.
        if (branchTaken) {
            nextIF = nop(SQUASHED);
        }

        // ---- HALT detection: check the instruction that just finished WB ----
        if (nextWB.isHalt) {
            halted = true;
            status = HALT;
        }

        // ---- Commit next pipeline state ----
        pipelineInfo.ifInst  = nextIF;
        pipelineInfo.idInst  = nextID;
        pipelineInfo.exInst  = nextEX;
        pipelineInfo.memInst = nextMEM;
        pipelineInfo.wbInst  = nextWB;

        // Advance PC and cycle counters
        PC = newPC;

        ++cycleCount;
        ++runSoFar;

        if (halted) {
            break;
        }
    }

    // Fill the pipeState with the *last* cycle’s pipeline contents
    pipeState.ifPC     = pipelineInfo.ifInst.PC;
    pipeState.ifStatus = pipelineInfo.ifInst.status;

    pipeState.idInstr  = pipelineInfo.idInst.instruction;
    pipeState.idStatus = pipelineInfo.idInst.status;

    pipeState.exInstr  = pipelineInfo.exInst.instruction;
    pipeState.exStatus = pipelineInfo.exInst.status;

    pipeState.memInstr  = pipelineInfo.memInst.instruction;
    pipeState.memStatus = pipelineInfo.memInst.status;

    pipeState.wbInstr  = pipelineInfo.wbInst.instruction;
    pipeState.wbStatus = pipelineInfo.wbInst.status;

    dumpPipeState(pipeState, output);
    return status;
}

// run till halt (call runCycles() with cycles == 1 each time) until
// status tells you to HALT or ERROR out
Status runTillHalt() {
    Status status = SUCCESS;
    while (true) {
        status = runCycles(1);
        if (status == HALT || status == ERROR) break;
    }
    return status;
}

// dump the state of the simulator
Status finalizeSimulator() {
    // Dump registers and memory (Project 1 style)
    simulator->dumpRegMem(output);

    // Collect statistics
    SimulationStats stats;
    stats.dynamicInstructions = simulator->getDin();
    stats.totalCycles         = cycleCount;

    stats.icHits   = iCache ? iCache->getHits()   : 0;
    stats.icMisses = iCache ? iCache->getMisses() : 0;
    stats.dcHits   = dCache ? dCache->getHits()   : 0;
    stats.dcMisses = dCache ? dCache->getMisses() : 0;

    stats.loadUseStalls = loadUseStallCount;

    dumpSimStats(stats, output);
    return SUCCESS;
}