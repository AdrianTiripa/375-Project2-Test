#include "cycle.h"

#include <iostream>
#include <memory>
#include <string>

#include "Utilities.h"
#include "cache.h"
#include "simulator.h"

static Simulator* simulator = nullptr;
static Cache* iCache = nullptr;
static Cache* dCache = nullptr;
static std::string output;
static uint64_t cycleCount = 0;

static uint64_t PC = 0;

/**TODO: Implement pipeline simulation for the RISCV machine in this file.
 * A basic template is provided below that doesn't account for any hazards.
 */

Simulator::Instruction nop(StageStatus status) {
    Simulator::Instruction nop;
    nop.instruction = 0x00000013;
    nop.isLegal = true;
    nop.isNop = true;
    nop.status = status;
    return nop;
}

static struct PipelineInfo {
    Simulator::Instruction ifInst  = nop(IDLE);
    Simulator::Instruction idInst  = nop(IDLE);
    Simulator::Instruction exInst  = nop(IDLE);
    Simulator::Instruction memInst = nop(IDLE);
    Simulator::Instruction wbInst  = nop(IDLE);
} pipelineInfo;

// forwarding helper 
static void forwarding(uint64_t rs, bool readsRs, uint64_t &opVal,
                       const Simulator::Instruction &exPrev,
                       const Simulator::Instruction &memPrev) {
    if (!readsRs || rs == 0) return;

    // forward from EX
    if (exPrev.writesRd && exPrev.rd == rs && exPrev.doesArithLogic) {
        opVal = exPrev.arithResult;
        return;
    }

    // forward from MEM (load or arith)
    if (memPrev.writesRd && memPrev.rd == rs) {
        if (memPrev.readsMem) {
            opVal = memPrev.memResult;
        } else if (memPrev.doesArithLogic) {
            opVal = memPrev.arithResult;
        }
    }
}

// keep track of stall cycles
static uint64_t stallCyclesCount   = 0;  // extra cycle for load-branch
static uint64_t loadStallCount     = 0;  // number of load stalls (load-use + load-branch)
static uint64_t iCacheStallCycles  = 0;
static uint64_t dCacheStallCycles  = 0;

// initialize the simulator
Status initSimulator(CacheConfig& iCacheConfig, CacheConfig& dCacheConfig, MemoryStore* mem,
                     const std::string& output_name) {
    output = output_name;
    simulator = new Simulator();
    simulator->setMemory(mem);
    iCache = new Cache(iCacheConfig, I_CACHE);
    dCache = new Cache(dCacheConfig, D_CACHE);

    // reset global state
    cycleCount = 0;
    PC = 0;
    pipelineInfo.ifInst  = nop(IDLE);
    pipelineInfo.idInst  = nop(IDLE);
    pipelineInfo.exInst  = nop(IDLE);
    pipelineInfo.memInst = nop(IDLE);
    pipelineInfo.wbInst  = nop(IDLE);

    stallCyclesCount   = 0;
    loadStallCount     = 0;
    iCacheStallCycles  = 0;
    dCacheStallCycles  = 0;

    return SUCCESS;
}

Status runCycles(uint64_t cycles) {
    uint64_t count = 0;
    auto status = SUCCESS;

    PipeState pipeState = {0};

    while (cycles == 0 || count < cycles) {
        pipeState.cycle = cycleCount;
        count++;
        cycleCount++;

        // keep track of the previous instructions in each stage
        Simulator::Instruction ifPrev  = pipelineInfo.ifInst;
        Simulator::Instruction idPrev  = pipelineInfo.idInst;
        Simulator::Instruction exPrev  = pipelineInfo.exInst;
        Simulator::Instruction memPrev = pipelineInfo.memInst;
        Simulator::Instruction wbPrev  = pipelineInfo.wbInst;

        bool StallID    = false;
        bool BubbleEx   = false;
        bool squashIF   = false;
        bool skipRest   = false;

        bool idPrevIsBranch;
        bool idPrevIsStore;
        bool branchTaken;
        Simulator::Instruction newIdInst; // what will be in ID
        bool illegalInID;

        bool iStall     = (iCacheStallCycles > 0);
        bool dStall     = (dCacheStallCycles > 0);

        // WB default
        pipelineInfo.wbInst = nop(BUBBLE);

        // MEM + WB + D-cache
        if (dStall) {
            // D-cache stall in progress
            pipelineInfo.memInst = memPrev;   // hold MEM
            pipelineInfo.exInst  = exPrev;    // hold EX
            pipelineInfo.idInst  = idPrev;    // hold ID
            pipelineInfo.ifInst  = ifPrev;    // hold IF

            dCacheStallCycles--;

            // allow I-cache miss latency to advance even while D-stalling
            if (iStall && iCacheStallCycles > 0) {
                iCacheStallCycles--;
            }

            // IF check for illegal PC (safe: no simIF call here)
            if (pipelineInfo.ifInst.PC >= MEMORY_SIZE) {
                pipelineInfo.ifInst = simulator->simIF(0x8000);
                status = ERROR;
            }

            // WB check for halt
            if (pipelineInfo.wbInst.isHalt) {
                status = HALT;
            }

            // dump pipeline
            pipeState.ifPC      = pipelineInfo.ifInst.PC;
            pipeState.ifStatus  = pipelineInfo.ifInst.status;
            pipeState.idInstr   = pipelineInfo.idInst.instruction;
            pipeState.idStatus  = pipelineInfo.idInst.status;
            pipeState.exInstr   = pipelineInfo.exInst.instruction;
            pipeState.exStatus  = pipelineInfo.exInst.status;
            pipeState.memInstr  = pipelineInfo.memInst.instruction;
            pipeState.memStatus = pipelineInfo.memInst.status;
            pipeState.wbInstr   = pipelineInfo.wbInst.instruction;
            pipeState.wbStatus  = pipelineInfo.wbInst.status;
            dumpPipeState(pipeState, output);

            if (status == HALT || status == ERROR) return status;
            continue;
        } else {
            // no D-stall: normal MEM/WB path

            // start from EX value that will go into MEM
            Simulator::Instruction exToMem = exPrev;

            // WB->MEM forwarding for load->store (store data)
            if (exToMem.writesMem && !exToMem.readsMem &&
                exToMem.readsRs2 && exToMem.rs2 != 0) {
                if (wbPrev.writesRd && wbPrev.rd == exToMem.rs2 && wbPrev.readsMem) {
                    exToMem.op2Val = wbPrev.memResult;
                }
            }

            bool needData = exToMem.readsMem || exToMem.writesMem;

            // memory exception (out of range)
            bool memError = needData && (exToMem.memAddress >= MEMORY_SIZE);

            if (memError) {
                // older MEM -> WB still happens
                pipelineInfo.wbInst  = simulator->simWB(memPrev);

                // squash younger stages
                pipelineInfo.memInst = nop(SQUASHED);
                pipelineInfo.exInst  = nop(SQUASHED);
                pipelineInfo.idInst  = nop(SQUASHED);
                pipelineInfo.ifInst  = nop(SQUASHED);

                PC     = 0x8000;
                status = ERROR;

                iCacheStallCycles = 0;
                dCacheStallCycles = 0;

                skipRest = true;
            } else if (needData) {
                CacheOperation type = exToMem.readsMem ? CACHE_READ : CACHE_WRITE;
                bool hit = dCache->access(exToMem.memAddress, type);

                if (!hit) {
                    // new D-cache miss
                    dCacheStallCycles   = dCache->config.missLatency;
                    pipelineInfo.memInst = simulator->simMEM(exToMem);
                    pipelineInfo.wbInst  = simulator->simWB(memPrev);
                } else {
                    // D-cache hit
                    pipelineInfo.memInst = simulator->simMEM(exToMem);
                    pipelineInfo.wbInst  = simulator->simWB(memPrev);
                }
            } else {
                // no memory access
                pipelineInfo.memInst = simulator->simMEM(exToMem);
                pipelineInfo.wbInst  = simulator->simWB(memPrev);
            }
        }

        if (skipRest) {
            goto dump_state;
        }

        // Hazard detection
        idPrevIsBranch = (idPrev.opcode == OP_BRANCH) ||
                         (idPrev.opcode == OP_JAL)    ||
                         (idPrev.opcode == OP_JALR);

        idPrevIsStore  = idPrev.writesMem && !idPrev.readsMem;

        if (stallCyclesCount > 0) {
            // extra stall cycle for load-branch
            StallID  = true;
            BubbleEx = true;
            stallCyclesCount--;
        } else {
            // load-use (1 cycle)
            bool hazardRs1 = (idPrev.rs1 == exPrev.rd);
            bool hazardRs2 = (idPrev.rs2 == exPrev.rd);

            bool loadUse = exPrev.readsMem && exPrev.writesRd &&
                           exPrev.rd != 0 &&
                           !idPrevIsBranch &&
                           (hazardRs1 ||
                           (hazardRs2 && !idPrevIsStore));

            if (loadUse) {
                StallID  = true;
                BubbleEx = true;
                loadStallCount++;
            }

            // arith-branch (1 cycle)
            if (exPrev.doesArithLogic && exPrev.writesRd && exPrev.rd != 0 &&
                idPrevIsBranch && !StallID &&
                (idPrev.rs1 == exPrev.rd || idPrev.rs2 == exPrev.rd)) {
                StallID  = true;
                BubbleEx = true;
            }

            // load-branch (2 cycles total)
            if (exPrev.readsMem && exPrev.writesRd && exPrev.rd != 0 &&
                idPrevIsBranch &&
                (idPrev.rs1 == exPrev.rd || idPrev.rs2 == exPrev.rd) &&
                !StallID) {
                StallID        = true;
                BubbleEx       = true;
                stallCyclesCount = 1;
                loadStallCount++;
            }
        }

        // EX
        if (BubbleEx) {
            // insert bubble in EX
            pipelineInfo.exInst = nop(BUBBLE);
        } else {
            // normal EX with forwarding
            forwarding(idPrev.rs1, idPrev.readsRs1, idPrev.op1Val, exPrev, memPrev);
            forwarding(idPrev.rs2, idPrev.readsRs2, idPrev.op2Val, exPrev, memPrev);

            pipelineInfo.exInst = simulator->simEX(idPrev);
        }

        // ID + branches (predict always not taken)
        branchTaken = false;

        if (idPrevIsBranch) {
            // forwarding for branch in ID
            forwarding(idPrev.rs1, idPrev.readsRs1, idPrev.op1Val, exPrev, memPrev);
            forwarding(idPrev.rs2, idPrev.readsRs2, idPrev.op2Val, exPrev, memPrev);

            // recompute nextPC using forwarded operands
            idPrev = simulator->simNextPCResolution(idPrev);

            // only resolve when ID is not stalled
            if (!StallID && !iStall) {
                branchTaken = (idPrev.nextPC != idPrev.PC + 4);
            }
        }

        // ID update (from IF)
        illegalInID = false;

        if (StallID || iStall) {
            // some stall, hold ID
            newIdInst = idPrev;
        } else {
            // normal IF -> ID
            newIdInst = simulator->simID(ifPrev);
        }

        // illegal instruction exception in ID
        if (!StallID && !iStall && !newIdInst.isLegal) {
            illegalInID        = true;
            pipelineInfo.idInst = nop(SQUASHED);
            pipelineInfo.ifInst = nop(SQUASHED);
            PC                  = 0x8000;
            iCacheStallCycles   = 0;
            status              = ERROR;
        }

        // PC + IF control
        if (!illegalInID && !StallID && !iStall) {
            if (idPrevIsBranch) {
                if (branchTaken) {
                    // mispredict under always-not-taken
                    newIdInst           = nop(SQUASHED);
                    pipelineInfo.ifInst = nop(SQUASHED);
                    squashIF            = true;

                    PC = idPrev.nextPC;
                    iCacheStallCycles = 0;
                } else {
                    // branch not taken
                    PC = idPrev.PC + 4;
                }
            } else {
                // non-branch, follow decoded nextPC
                PC = newIdInst.nextPC;
            }
        }

        // update ID (only if not illegal, otherwise already set)
        if (!illegalInID) {
            pipelineInfo.idInst = newIdInst;
        }

        // IF
        if (illegalInID) {
            // IF already squashed above
        } else if (StallID) {
            // hold IF during data stall
            pipelineInfo.ifInst = ifPrev;
            if (iStall && iCacheStallCycles > 0) {
                iCacheStallCycles--;
            }
        } else if (!squashIF) {
            if (iStall) {
                // I-cache miss in progress, hold IF
                pipelineInfo.ifInst = ifPrev;
                if (iCacheStallCycles > 0) {
                    iCacheStallCycles--;
                }
            } else {
                // normal fetch with I-cache access
                if (PC >= MEMORY_SIZE) {
                    pipelineInfo.ifInst = simulator->simIF(0x8000);
                    status = ERROR;
                } else {
                    bool hit = iCache->access(PC, CACHE_READ);
                    pipelineInfo.ifInst = simulator->simIF(PC);
                    pipelineInfo.ifInst.status = SPECULATIVE;
                    if (!hit) {
                        iCacheStallCycles = iCache->config.missLatency;
                    }
                }
            }
        }
        // else squashIF already set IF to NOP (squashed)

        // WB check for halt
        if (pipelineInfo.wbInst.isHalt) {
            status = HALT;
            goto dump_state;
        }

dump_state:
        // dump pipeline
        pipeState.ifPC      = pipelineInfo.ifInst.PC;
        pipeState.ifStatus  = pipelineInfo.ifInst.status;
        pipeState.idInstr   = pipelineInfo.idInst.instruction;
        pipeState.idStatus  = pipelineInfo.idInst.status;
        pipeState.exInstr   = pipelineInfo.exInst.instruction;
        pipeState.exStatus  = pipelineInfo.exInst.status;
        pipeState.memInstr  = pipelineInfo.memInst.instruction;
        pipeState.memStatus = pipelineInfo.memInst.status;
        pipeState.wbInstr   = pipelineInfo.wbInst.instruction;
        pipeState.wbStatus  = pipelineInfo.wbInst.status;
        dumpPipeState(pipeState, output);

        if (status == HALT || status == ERROR) return status;
    }

    return status;
}

// run till halt (call runCycles() with cycles == 1 each time) until
// status tells you to HALT or ERROR out
Status runTillHalt() {
    Status status;
    while (true) {
        status = static_cast<Status>(runCycles(1));
        if (status == HALT) break;
    }
    return status;
}

// dump the state of the simulator
Status finalizeSimulator() {
    simulator->dumpRegMem(output);
    SimulationStats stats{simulator->getDin(),
                          cycleCount,
                          iCache->getHits(),
                          iCache->getMisses(),
                          dCache->getHits(),
                          dCache->getMisses(),
                          loadStallCount};
    dumpSimStats(stats, output);
    return SUCCESS;
}