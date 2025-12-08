#include "cycle.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "Utilities.h"
#include "cache.h"
#include "simulator.h"

/*
 * Global simulator state
 */

static Simulator* simulator = nullptr;
static Cache* iCache = nullptr;
static Cache* dCache = nullptr;
static std::string output;
static uint64_t cycleCount = 0;
static uint64_t PC = 0;

/*
 * NOP constructor helper
 */
Simulator::Instruction nop(StageStatus status) {
    Simulator::Instruction n;
    n.instruction = 0x00000013;
    n.isLegal = true;
    n.isNop = true;
    n.status = status;
    return n;
}

/*
 * Pipeline registers
 */
static struct PipelineInfo {
    Simulator::Instruction ifInst = nop(IDLE);
    Simulator::Instruction idInst = nop(IDLE);
    Simulator::Instruction exInst = nop(IDLE);
    Simulator::Instruction memInst = nop(IDLE);
    Simulator::Instruction wbInst = nop(IDLE);
} pipelineInfo;

/*
 * Stall counters
 */
static uint64_t loadStallCount = 0;
static uint64_t iCacheStallCycles = 0;
static uint64_t dCacheStallCycles = 0;

/*
 * initSimulator:
 * Performs initial fetch THROUGH the I-cache, so that the first instruction
 * appears multiple times in IF if the I-cache misses. (Required by spec.)
 */
Status initSimulator(CacheConfig& iCfg, CacheConfig& dCfg, MemoryStore* mem,
                     const std::string& out) {
    output = out;

    simulator = new Simulator();
    simulator->setMemory(mem);
    iCache = new Cache(iCfg, I_CACHE);
    dCache = new Cache(dCfg, D_CACHE);

    cycleCount = 0;
    PC = 0;
    loadStallCount = 0;
    iCacheStallCycles = 0;
    dCacheStallCycles = 0;

    pipelineInfo.ifInst = nop(IDLE);
    pipelineInfo.idInst = nop(IDLE);
    pipelineInfo.exInst = nop(IDLE);
    pipelineInfo.memInst = nop(IDLE);
    pipelineInfo.wbInst = nop(IDLE);

    /* Perform initial fetch THROUGH the I-cache */
    bool hit = iCache->access(PC, CACHE_READ);
    pipelineInfo.ifInst = simulator->simIF(PC);
    if (!hit) {
        iCacheStallCycles = iCache->config.missLatency;
    }

    return SUCCESS;
}

/*
 * runCycles:
 * The heart of the cycle-accurate simulator
 */
Status runCycles(uint64_t cycles) {
    uint64_t count = 0;
    Status status = SUCCESS;

    PipeState pipeState = {0};

    while (cycles == 0 || count < cycles) {

        pipeState.cycle = cycleCount;
        cycleCount++;
        count++;

        Simulator::Instruction ifPrev  = pipelineInfo.ifInst;
        Simulator::Instruction idPrev  = pipelineInfo.idInst;
        Simulator::Instruction exPrev  = pipelineInfo.exInst;
        Simulator::Instruction memPrev = pipelineInfo.memInst;

        /*
         * WRITEBACK STAGE
         */
        pipelineInfo.wbInst = nop(BUBBLE);

        /*
         * MEMORY STAGE — D-cache miss detection
         */

        bool memError = (exPrev.readsMem || exPrev.writesMem) &&
                        (exPrev.memAddress >= MEMORY_SIZE);

        bool dMiss = !memError &&
                     ((exPrev.readsMem  && !dCache->access(exPrev.memAddress, CACHE_READ)) ||
                      (exPrev.writesMem && !dCache->access(exPrev.memAddress, CACHE_WRITE)));

        /*
         * D-cache MISS — follow the project spec carefully
         */
        if (dCacheStallCycles > 0) {
            // Stalled due to a previous D-cache miss
            pipelineInfo.wbInst = nop(BUBBLE);
            dCacheStallCycles--;
        }
        else if (memError) {
            // Memory exception — squash younger instructions
            pipelineInfo.wbInst  = simulator->simWB(memPrev);
            pipelineInfo.memInst = nop(SQUASHED);
            pipelineInfo.exInst  = nop(SQUASHED);
            pipelineInfo.idInst  = nop(SQUASHED);

            PC = 0x8000;
            pipelineInfo.ifInst = simulator->simIF(PC);

            // No fetch stall from exception
        }
        else if (dMiss) {
            // Detect a new D-cache miss
            pipelineInfo.wbInst = simulator->simWB(memPrev);
            pipelineInfo.memInst = simulator->simMEM(exPrev);

            dCacheStallCycles = dCache->config.missLatency;
        }
        else {
            /*
             * Normal D-cache hit or no memory access
             */
            pipelineInfo.wbInst = simulator->simWB(memPrev);
            pipelineInfo.memInst = simulator->simMEM(exPrev);

            /*
             * HAZARD DETECTION
             */

            bool isBranch = !idPrev.isNop &&
                            (idPrev.opcode == OP_BRANCH ||
                             idPrev.opcode == OP_JAL ||
                             idPrev.opcode == OP_JALR);

            bool loadUse = !idPrev.isNop &&
                           !exPrev.isNop &&
                           (idPrev.rs1 == exPrev.rd || idPrev.rs2 == exPrev.rd) &&
                           exPrev.opcode == OP_LOAD &&
                           exPrev.rd != 0;

            bool loadUseBranch = loadUse && isBranch;
            bool pureLoadUse   = loadUse && !isBranch;

            bool arithBranch = isBranch &&
                               exPrev.doesArithLogic &&
                               exPrev.writesRd &&
                               exPrev.rd != 0 &&
                               (idPrev.rs1 == exPrev.rd || idPrev.rs2 == exPrev.rd);

            bool loadBranch = isBranch &&
                              (memPrev.opcode == OP_LOAD) &&
                              exPrev.isNop;

            /*
             * EXECUTE STAGE
             */

            if (pureLoadUse || loadUseBranch) {

                /* Forward from MEM → ID if needed */
                if (pipelineInfo.memInst.rd == idPrev.rs1)
                    pipelineInfo.idInst.op1Val = pipelineInfo.memInst.memResult;

                if (pipelineInfo.memInst.rd == idPrev.rs2)
                    pipelineInfo.idInst.op2Val = pipelineInfo.memInst.memResult;

                // Insert a bubble into EX
                pipelineInfo.exInst = nop(BUBBLE);

                // Count pure load-use stalls only once
                if (pureLoadUse) loadStallCount++;

                // If I-cache was already stalling, continue decrementing
                if (iCacheStallCycles > 0)
                    iCacheStallCycles--;
            }
            else if (arithBranch) {

                /* Forward from EX → ID */
                if (exPrev.rd == idPrev.rs1)
                    pipelineInfo.idInst.op1Val = exPrev.arithResult;

                if (exPrev.rd == idPrev.rs2)
                    pipelineInfo.idInst.op2Val = exPrev.arithResult;

                pipelineInfo.exInst = nop(BUBBLE);
                pipelineInfo.idInst = simulator->simNextPCResolution(idPrev);

                if (iCacheStallCycles > 0)
                    iCacheStallCycles--;
            }
            else if (loadBranch) {

                /* Second stall of load-branch */
                if (pipelineInfo.wbInst.rd == idPrev.rs1)
                    pipelineInfo.idInst.op1Val = pipelineInfo.wbInst.memResult;

                if (pipelineInfo.memInst.rd == idPrev.rs2)
                    pipelineInfo.idInst.op2Val = pipelineInfo.memInst.memResult;

                pipelineInfo.exInst = nop(BUBBLE);
                pipelineInfo.idInst = simulator->simNextPCResolution(idPrev);

                // Count load-branch stall ONCE
                loadStallCount++;

                if (iCacheStallCycles > 0)
                    iCacheStallCycles--;
            }
            else {

                /*
                 * No hazards: normal EX
                 */

                /* Forwarding logic */
                if (exPrev.writesRd && exPrev.rd == idPrev.rs1 && exPrev.doesArithLogic)
                    idPrev.op1Val = exPrev.arithResult;
                else if (memPrev.writesRd && memPrev.rd == idPrev.rs1 && memPrev.readsMem)
                    idPrev.op1Val = memPrev.memResult;
                else if (memPrev.writesRd && memPrev.rd == idPrev.rs1 && memPrev.doesArithLogic)
                    idPrev.op1Val = memPrev.arithResult;

                if (exPrev.writesRd && exPrev.rd == idPrev.rs2 && exPrev.doesArithLogic)
                    idPrev.op2Val = exPrev.arithResult;
                else if (memPrev.writesRd && memPrev.rd == idPrev.rs2 && memPrev.readsMem)
                    idPrev.op2Val = memPrev.memResult;
                else if (memPrev.writesRd && memPrev.rd == idPrev.rs2 && memPrev.doesArithLogic)
                    idPrev.op2Val = memPrev.arithResult;

                pipelineInfo.exInst = simulator->simEX(idPrev);

                /*
                 * ID + IF stage logic
                 */

                bool idSquash = isBranch && (idPrev.nextPC != idPrev.PC + 4);

                if (idSquash) {

                    pipelineInfo.idInst = nop(SQUASHED);

                    if (iCacheStallCycles > 0) {
                        iCache->invalidate(PC);
                        iCacheStallCycles = 0;
                    }

                    PC = idPrev.nextPC;
                    pipelineInfo.ifInst = simulator->simIF(PC);

                    if (!iCache->access(PC, CACHE_READ))
                        iCacheStallCycles = iCache->config.missLatency;
                }
                else {

                    pipelineInfo.idInst = simulator->simID(ifPrev);

                    PC = pipelineInfo.idInst.isLegal ? pipelineInfo.idInst.nextPC : 0x8000;

                    if (!pipelineInfo.idInst.isLegal) {
                        pipelineInfo.idInst = nop(SQUASHED);
                        pipelineInfo.ifInst = simulator->simIF(PC);
                    }
                    else if (iCacheStallCycles > 0) {
                        pipelineInfo.idInst = nop(BUBBLE);
                        iCacheStallCycles--;
                    }
                    else if (!iCache->access(PC, CACHE_READ)) {
                        pipelineInfo.ifInst = simulator->simIF(PC);
                        iCacheStallCycles = iCache->config.missLatency;
                    }
                    else {
                        pipelineInfo.ifInst = simulator->simIF(PC);
                    }
                }
            }
        }

        /*
         * HALT detection — halt completes in WB
         */
        if (pipelineInfo.wbInst.isHalt) {
            status = HALT;
            break;
        }
    }

    /*
     * Dump pipeline state
     */
    pipeState.ifPC     = pipelineInfo.ifInst.PC;
    pipeState.ifStatus = pipelineInfo.ifInst.status;
    pipeState.idInstr  = pipelineInfo.idInst.instruction;
    pipeState.idStatus = pipelineInfo.idInst.status;
    pipeState.exInstr  = pipelineInfo.exInst.instruction;
    pipeState.exStatus = pipelineInfo.exInst.status;
    pipeState.memInstr = pipelineInfo.memInst.instruction;
    pipeState.memStatus= pipelineInfo.memInst.status;
    pipeState.wbInstr  = pipelineInfo.wbInst.instruction;
    pipeState.wbStatus = pipelineInfo.wbInst.status;

    dumpPipeState(pipeState, output);

    return status;
}

/*
 * runTillHalt wrapper
 */
Status runTillHalt() {
    Status s;
    while (true) {
        s = runCycles(1);
        if (s == HALT || s == ERROR)
            break;
    }
    return s;
}

/*
 * finalizeSimulator:
 * Dump final register/memory state + statistics
 */
Status finalizeSimulator() {

    simulator->dumpRegMem(output);

    SimulationStats stats {
        simulator->getDin(),
        cycleCount,
        iCache->getHits(),
        iCache->getMisses(),
        dCache->getHits(),
        dCache->getMisses(),
        loadStallCount
    };

    dumpSimStats(stats, output);

    return SUCCESS;
}