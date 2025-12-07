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

// initialize the simulator
Status initSimulator(CacheConfig& iCacheConfig, CacheConfig& dCacheConfig, MemoryStore* mem,
                     const std::string& output_name) {
    output = output_name;
    simulator = new Simulator();
    simulator->setMemory(mem);
    iCache = new Cache(iCacheConfig, I_CACHE);
    dCache = new Cache(dCacheConfig, D_CACHE);
    cycleCount = 0;
    PC = 0;
    pipelineInfo.ifInst  = nop(IDLE);
    pipelineInfo.idInst  = nop(IDLE);
    pipelineInfo.exInst  = nop(IDLE);
    pipelineInfo.memInst = nop(IDLE);
    pipelineInfo.wbInst  = nop(IDLE);
    return SUCCESS;
}

// keep track of stall cycles
static int stallCyclesCount   = 0;  // extra cycle for load-branch
static int loadStallCount     = 0;  // number of load stalls (load-use + load-branch)
static int iCacheStallCycles  = 0;
static int dCacheStallCycles  = 0;

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

        bool StallID  = false;
        bool BubbleEx = false;
        bool squashIF = false;

        bool iStall = (iCacheStallCycles > 0);
        bool dStall = (dCacheStallCycles > 0);

        // default WB is bubble
        pipelineInfo.wbInst = nop(BUBBLE);

        // MEM + WB + D-cache
        if (dStall) {
            // D-cache stall in progress: hold MEM, do not advance the miss
            pipelineInfo.memInst = memPrev;
            pipelineInfo.exInst  = exPrev;   // hold EX
            pipelineInfo.idInst  = idPrev;   // hold ID
            pipelineInfo.ifInst  = ifPrev;   // hold IF
            // older instruction in WB still advances
            pipelineInfo.wbInst = simulator->simWB(memPrev);
            dCacheStallCycles--;

            // WB check for halt instruction
            if (pipelineInfo.wbInst.isHalt) {
                status = HALT;
            }

            // dump pipeline state for this cycle
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

            if (status == HALT) break;
            continue;
        } else {
            bool needData = exPrev.readsMem || exPrev.writesMem;

            // memory exception (out of range)
            bool memError = needData && (exPrev.memAddress >= MEMORY_SIZE);

            if (memError) {
                // older MEM -> WB still happens
                pipelineInfo.wbInst = simulator->simWB(memPrev);

                // squash younger stages
                pipelineInfo.memInst = nop(SQUASHED);  // excepting instruction
                pipelineInfo.exInst  = nop(SQUASHED);
                pipelineInfo.idInst  = nop(SQUASHED);
                pipelineInfo.ifInst  = nop(SQUASHED);

                // jump to exception handler
                PC     = 0x8000;
                status = ERROR;

                // clear cache stalls
                iCacheStallCycles = 0;
                dCacheStallCycles = 0;

                // dump state for this cycle and return
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
                return status;
            }

            if (needData) {
                CacheOperation type = exPrev.readsMem ? CACHE_READ : CACHE_WRITE;
                bool hit = dCache->access(exPrev.memAddress, type);

                if (!hit) {
                    // D-cache miss: start stall for this load/store
                    dCacheStallCycles = dCache->config.missLatency;
                    pipelineInfo.memInst = simulator->simMEM(exPrev);
                    // older MEM instruction should still retire this cycle
                    if (!iStall) {
                        pipelineInfo.wbInst = simulator->simWB(memPrev);
                    }
                } else {
                    // D-cache hit: normal MEM
                    pipelineInfo.memInst = simulator->simMEM(exPrev);

                    // WB can proceed unless I-cache is stalling previous fetch
                    if (!iStall) {
                        pipelineInfo.wbInst = simulator->simWB(memPrev);
                    }
                }
            } else {
                // no memory access: normal MEM
                pipelineInfo.memInst = simulator->simMEM(exPrev);

                if (!iStall) {
                    pipelineInfo.wbInst = simulator->simWB(memPrev);
                }
            }
        }

        // Hazard detection (only when D-cache not stalling)
        bool idPrevIsBranch = (idPrev.opcode == OP_BRANCH) ||
                              (idPrev.opcode == OP_JAL)    ||
                              (idPrev.opcode == OP_JALR);

        bool idPrevIsStore  = idPrev.writesMem && !idPrev.readsMem;

        if (stallCyclesCount > 0) {
            // extra stall cycle for load-branch
            StallID  = true;
            BubbleEx = true;
            stallCyclesCount--;
        } else {
            // load-use stall (1 cycle), but do not stall store-data (rs2) because of WB->MEM forwarding
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

            // arithmetic-branch stall (1 cycle)
            if (exPrev.doesArithLogic && exPrev.writesRd && exPrev.rd != 0 &&
                idPrevIsBranch && !StallID &&
                (idPrev.rs1 == exPrev.rd || idPrev.rs2 == exPrev.rd)) {
                StallID  = true;
                BubbleEx = true;
            }

            // load-branch stall (2 cycles total)
            if (exPrev.readsMem && exPrev.writesRd && exPrev.rd != 0 &&
                idPrevIsBranch &&
                (idPrev.rs1 == exPrev.rd || idPrev.rs2 == exPrev.rd) &&
                !StallID) {
                StallID        = true;
                BubbleEx       = true;
                stallCyclesCount = 1;   // one more stall next cycle
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

        // ID and branches (predict always not taken)
        bool branchTaken = false;

        if (idPrevIsBranch) {
            // forwarding for branch in ID
            forwarding(idPrev.rs1, idPrev.readsRs1, idPrev.op1Val, exPrev, memPrev);
            forwarding(idPrev.rs2, idPrev.readsRs2, idPrev.op2Val, exPrev, memPrev);

            // recompute nextPC using forwarded operands
            idPrev = simulator->simNextPCResolution(idPrev);

            // only resolve when ID is not stalled
            if (!StallID) {
                branchTaken = (idPrev.nextPC != idPrev.PC + 4);
            }
        }

        // ID update (from IF)
        Simulator::Instruction newIdInst;  // what will be in ID
        if (StallID || iStall) {
            // some stall, hold ID
            newIdInst = idPrev;
        } else {
            // normal IF -> ID
            newIdInst = simulator->simID(ifPrev);
        }

        // if branch is taken, squash the speculative IF instruction before it enters ID
        if (!StallID && idPrevIsBranch && branchTaken) {
            newIdInst = nop(SQUASHED);
        }

        // PC and IF control
        if (!StallID && !iStall) {
            if (idPrevIsBranch) {
                if (branchTaken) {
                    // always-not-taken mispredict
                    squashIF = true;
                    PC = idPrev.nextPC;
                    iCacheStallCycles = 0;
                } else {
                    // branch not taken
                    PC = idPrev.PC + 4;
                }
            } else {
                // non-branch instruction
                PC = newIdInst.nextPC;
            }
        }

        // update ID
        pipelineInfo.idInst = newIdInst;

        // WB->MEM forwarding for load->store (store data)
        if (exPrev.writesMem && !exPrev.readsMem && exPrev.readsRs2 && exPrev.rs2 != 0) {
            const Simulator::Instruction &wbNow = pipelineInfo.wbInst;
            if (wbNow.writesRd && wbNow.rd == exPrev.rs2 && wbNow.readsMem) {
                // forward from WB load to store data
                Simulator::Instruction &exRef = pipelineInfo.exInst;
                exRef.op2Val = wbNow.memResult;
            }
        }

        // IF
        if (StallID) {
            // hold IF while ID is stalled
            pipelineInfo.ifInst = ifPrev;
        } else if (!squashIF) {
            if (iStall) {
                // I-cache miss in progress: hold IF
                pipelineInfo.ifInst = ifPrev;
                iCacheStallCycles--;
            } else {
                // normal speculative fetch with I-cache access
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

        // WB check for halt instruction
        if (pipelineInfo.wbInst.isHalt) {
            status = HALT;
        }

        // dump pipeline state for this cycle
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

        // if HALT reached WB, stop early
        if (status == HALT) break;
    }

    return status;
}

// run till halt (call runCycles() with cycles == 1 each time) until
// status tells you to HALT or ERROR out
Status runTillHalt() {
    Status status;
    while (true) {
        status = static_cast<Status>(runCycles(1));
        if (status == HALT || status == ERROR) break;
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