#include "cycle.h"

#include <iostream>
#include <memory>
#include <string>

#include "Utilities.h"
#include "cache.h"
#include "simulator.h"

// simulator state
static Simulator* simulator = nullptr;
static Cache* iCache        = nullptr;
static Cache* dCache        = nullptr;
static std::string output;

// global cycle counter
static uint64_t cycleCount = 0;

// PC of the next instruction to fetch
static uint64_t PC = 0;

// stall / hazard stats
static uint64_t loadStallCount          = 0;  // includes load-use and load-branch
static uint64_t loadBranchExtraCycles   = 0;  // second cycle of a 2-cycle load-branch stall

// simple NOP constructor
static Simulator::Instruction nop(StageStatus status) {
    Simulator::Instruction n;
    n.instruction = 0x00000013;  // addi x0, x0, 0
    n.isLegal     = true;
    n.isNop       = true;
    n.status      = status;
    return n;
}

// pipeline registers
static struct PipelineInfo {
    Simulator::Instruction ifInst  = nop(IDLE);
    Simulator::Instruction idInst  = nop(IDLE);
    Simulator::Instruction exInst  = nop(IDLE);
    Simulator::Instruction memInst = nop(IDLE);
    Simulator::Instruction wbInst  = nop(IDLE);
} pipelineInfo;


// initialize simulation
Status initSimulator(CacheConfig& iCacheConfig,
                     CacheConfig& dCacheConfig,
                     MemoryStore* mem,
                     const std::string& output_name) {
    output    = output_name;
    simulator = new Simulator();
    simulator->setMemory(mem);

    iCache = new Cache(iCacheConfig, I_CACHE);
    dCache = new Cache(dCacheConfig, D_CACHE);

    cycleCount            = 0;
    PC                    = 0;
    loadStallCount        = 0;
    loadBranchExtraCycles = 0;

    pipelineInfo.ifInst  = nop(IDLE);
    pipelineInfo.idInst  = nop(IDLE);
    pipelineInfo.exInst  = nop(IDLE);
    pipelineInfo.memInst = nop(IDLE);
    pipelineInfo.wbInst  = nop(IDLE);

    return SUCCESS;
}


// run for a certain number of cycles (0 = run until halt)
Status runCycles(uint64_t cycles) {
    uint64_t count = 0;
    Status status  = SUCCESS;

    PipeState pipeState = {0};

    while (cycles == 0 || count < cycles) {
        // track which cycle we are about to simulate
        pipeState.cycle = cycleCount;

        // snapshot old pipeline state
        Simulator::Instruction oldIF  = pipelineInfo.ifInst;
        Simulator::Instruction oldID  = pipelineInfo.idInst;
        Simulator::Instruction oldEX  = pipelineInfo.exInst;
        Simulator::Instruction oldMEM = pipelineInfo.memInst;
        Simulator::Instruction oldWB  = pipelineInfo.wbInst;

        // next pipeline values
        Simulator::Instruction nextIF  = nop(IDLE);
        Simulator::Instruction nextID  = nop(IDLE);
        Simulator::Instruction nextEX  = nop(BUBBLE);
        Simulator::Instruction nextMEM = nop(BUBBLE);
        Simulator::Instruction nextWB  = nop(BUBBLE);

        bool stallIF   = false;
        bool stallID   = false;
        bool bubbleEX  = false;

        // ========== EXCEPTIONS FIRST ==========

        // Memory exception detected in MEM: squash this instruction and all after it, jump to 0x8000.
        if (!oldMEM.isNop && oldMEM.memException) {
            // older instructions still complete
            nextWB  = simulator->simWB(oldWB);
            nextMEM = nop(SQUASHED);
            nextEX  = nop(SQUASHED);
            nextID  = nop(SQUASHED);
            nextIF  = nop(SQUASHED);

            PC                    = 0x8000;
            loadBranchExtraCycles = 0;  // clear any pending load-branch stall
        }
        // Illegal instruction in ID: squash it and younger, jump to 0x8000.
        else if (!oldID.isNop && !oldID.isLegal) {
            nextWB  = simulator->simWB(oldMEM);
            nextMEM = simulator->simMEM(oldEX);
            nextEX  = nop(SQUASHED);
            nextID  = nop(SQUASHED);
            nextIF  = nop(SQUASHED);

            PC                    = 0x8000;
            loadBranchExtraCycles = 0;
        }
        else {
            // ========== HAZARD DETECTION (LOAD/BRANCH) ==========

            bool exWritesRd = oldEX.writesRd && (oldEX.rd != 0);
            bool exIsLoad   = oldEX.readsMem && !oldEX.writesMem && exWritesRd;
            bool exIsArith  = oldEX.doesArithLogic && exWritesRd;

            bool idIsNop    = oldID.isNop;
            bool idIsBranch = (oldID.opcode == OP_BRANCH);
            bool idIsStore  = oldID.writesMem && !oldID.readsMem;

            bool hazardRs1 = false;
            bool hazardRs2 = false;

            if (!idIsNop && exWritesRd) {
                if (oldID.readsRs1 && oldID.rs1 == oldEX.rd) hazardRs1 = true;
                if (oldID.readsRs2 && oldID.rs2 == oldEX.rd) hazardRs2 = true;
            }

            bool loadBranchHazard =
                exIsLoad && idIsBranch && (hazardRs1 || hazardRs2);

            bool loadUseHazard =
                exIsLoad && !idIsBranch &&
                (hazardRs1 || (hazardRs2 && !idIsStore));  // allow load -> store with forwarding

            bool arithBranchHazard =
                exIsArith && idIsBranch && (hazardRs1 || hazardRs2);

            // second cycle of a load-branch stall
            if (loadBranchExtraCycles > 0) {
                stallIF   = true;
                stallID   = true;
                bubbleEX  = true;
                loadBranchExtraCycles--;
            } else {
                if (loadBranchHazard) {
                    // two total stall cycles, counted as one load stall
                    stallIF   = true;
                    stallID   = true;
                    bubbleEX  = true;
                    loadBranchExtraCycles = 1;
                    loadStallCount++;
                } else if (loadUseHazard) {
                    // one stall cycle
                    stallIF   = true;
                    stallID   = true;
                    bubbleEX  = true;
                    loadStallCount++;
                } else if (arithBranchHazard) {
                    // one stall cycle (no load stall counter increment)
                    stallIF   = true;
                    stallID   = true;
                    bubbleEX  = true;
                }
            }

            // ========== STAGE UPDATES BACK-TO-FRONT ==========

            // WB: commit result of old MEM
            nextWB = simulator->simWB(oldMEM);

            // MEM: handle memory accesses + D-cache stats, including WB->MEM forward for load->store
            Simulator::Instruction exForMem = oldEX;

            // WB -> MEM forwarding for load -> store data (rs2)
            if (exForMem.writesMem && !exForMem.readsMem &&
                exForMem.readsRs2 && exForMem.rs2 != 0) {
                if (oldWB.writesRd && oldWB.rd == exForMem.rs2 && oldWB.readsMem) {
                    exForMem.op2Val = oldWB.memResult;
                }
            }

            if (exForMem.readsMem || exForMem.writesMem) {
                CacheOperation op = exForMem.readsMem ? CACHE_READ : CACHE_WRITE;
                dCache->access(exForMem.memAddress, op);
            }
            nextMEM = simulator->simMEM(exForMem);

            // EX: either bubble or real execute with EX/MEM forwarding
            if (bubbleEX) {
                nextEX = nop(BUBBLE);
            } else {
                Simulator::Instruction idForEX = oldID;

                auto forwardOperand = [&](uint64_t rs, bool reads, uint64_t& val) {
                    if (!reads || rs == 0) return;

                    // EX stage forwarding (ALU result)
                    if (oldEX.writesRd && oldEX.doesArithLogic && oldEX.rd == rs) {
                        val = oldEX.arithResult;
                        return;
                    }

                    // MEM stage forwarding (ALU or load result)
                    if (oldMEM.writesRd && oldMEM.rd == rs) {
                        if (oldMEM.readsMem) {
                            val = oldMEM.memResult;
                        } else if (oldMEM.doesArithLogic) {
                            val = oldMEM.arithResult;
                        }
                    }
                };

                forwardOperand(idForEX.rs1, idForEX.readsRs1, idForEX.op1Val);
                forwardOperand(idForEX.rs2, idForEX.readsRs2, idForEX.op2Val);

                nextEX = simulator->simEX(idForEX);
            }

            // ID: either hold (stall) or decode IF
            if (stallID) {
                nextID = oldID;
            } else {
                nextID = simulator->simID(oldIF);
            }

            // ========== BRANCH PREDICTION (ALWAYS NOT TAKEN) ==========

            uint64_t newPC = PC + 4;  // default next fetch

            bool nextIsCtrl =
                (nextID.opcode == OP_BRANCH ||
                 nextID.opcode == OP_JAL    ||
                 nextID.opcode == OP_JALR);

            bool branchTaken = false;
            if (!nextID.isNop && nextID.isLegal && nextIsCtrl) {
                // simID should have filled nextPC for control instructions
                if (nextID.nextPC != nextID.PC + 4) {
                    branchTaken = true;
                    newPC       = nextID.nextPC;
                }
            }

            // IF: fetch if not stalled
            if (stallIF) {
                nextIF = oldIF;
            } else {
                // instruction fetch goes through I-cache
                iCache->access(PC, CACHE_READ);
                nextIF = simulator->simIF(PC);

                if (nextIsCtrl) {
                    nextIF.status = SPECULATIVE;
                } else {
                    nextIF.status = NORMAL;
                }
            }

            // squash speculative IF instruction on a taken branch
            if (branchTaken) {
                nextIF = nop(SQUASHED);
            }

            // update PC for next cycle
            PC = newPC;

            // halt detection: when halt reaches WB
            if (nextWB.isHalt) {
                status = HALT;
            }
        }

        // commit new pipeline state
        pipelineInfo.ifInst  = nextIF;
        pipelineInfo.idInst  = nextID;
        pipelineInfo.exInst  = nextEX;
        pipelineInfo.memInst = nextMEM;
        pipelineInfo.wbInst  = nextWB;

        // update cycle counters
        count++;
        cycleCount++;

        // done with cycles?
        if (status == HALT) break;
    }

    // dump pipe state for the last simulated cycle
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


// run until halt, one cycle at a time (so pipe state is dumped each cycle)
Status runTillHalt() {
    Status status;
    while (true) {
        status = runCycles(1);
        if (status == HALT || status == ERROR) break;
    }
    return status;
}


// finalize simulation: dump regs, memory, and statistics
Status finalizeSimulator() {
    simulator->dumpRegMem(output);

    SimulationStats stats;
    stats.dynamicInstructions = simulator->getDin();
    stats.totalCycles         = cycleCount;
    stats.icHits              = iCache->getHits();
    stats.icMisses            = iCache->getMisses();
    stats.dcHits              = dCache->getHits();
    stats.dcMisses            = dCache->getMisses();
    stats.loadUseStalls       = loadStallCount;

    dumpSimStats(stats, output);
    return SUCCESS;
}