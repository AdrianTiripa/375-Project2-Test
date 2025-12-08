#include "cycle.h"

#include <iostream>
#include <memory>
#include <string>

#include "Utilities.h"
#include "cache.h"
#include "simulator.h"

// simulator state
static Simulator* simulator = nullptr;
static Cache* iCache = nullptr;
static Cache* dCache = nullptr;
static std::string output;
static uint64_t cycleCount = 0;

static uint64_t PC = 0;

/** TODO: Implement pipeline simulation for the RISCV machine in this file.
 *  This version includes full forwarding, stalls, branch prediction,
 *  exceptions, and cache timing behavior.
 */

Simulator::Instruction nop(StageStatus status) {
    Simulator::Instruction nop;
    nop.instruction = 0x00000013; // ADDI x0,x0,0
    nop.isLegal = true;
    nop.isNop = true;
    nop.status = status;
    return nop;
}

// pipeline registers
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
                       const Simulator::Instruction &memPrev)
{
    if (!readsRs || rs == 0) return;

    // EX forwarding (arith only)
    if (exPrev.writesRd && exPrev.rd == rs && exPrev.doesArithLogic) {
        opVal = exPrev.arithResult;
        return;
    }

    // MEM forwarding (load or arith)
    if (memPrev.writesRd && memPrev.rd == rs) {
        if (memPrev.readsMem)        opVal = memPrev.memResult;
        else if (memPrev.doesArithLogic) opVal = memPrev.arithResult;
    }
}

// stall counters
static uint64_t loadStallCount    = 0;
static uint64_t stallCyclesCount  = 0;
static uint64_t iCacheStallCycles = 0;
static uint64_t dCacheStallCycles = 0;


// initialize simulation
Status initSimulator(CacheConfig& iCacheConfig,
                     CacheConfig& dCacheConfig,
                     MemoryStore* mem,
                     const std::string& output_name)
{
    output = output_name;

    simulator = new Simulator();
    simulator->setMemory(mem);

    iCache = new Cache(iCacheConfig, I_CACHE);
    dCache = new Cache(dCacheConfig, D_CACHE);

    cycleCount = 0;
    PC = 0;
    stallCyclesCount  = 0;
    loadStallCount    = 0;
    iCacheStallCycles = 0;
    dCacheStallCycles = 0;

    pipelineInfo.ifInst  = nop(IDLE);
    pipelineInfo.idInst  = nop(IDLE);
    pipelineInfo.exInst  = nop(IDLE);
    pipelineInfo.memInst = nop(IDLE);
    pipelineInfo.wbInst  = nop(IDLE);

    // initial fetch through I-cache
    bool hit = iCache->access(PC, CACHE_READ);
    pipelineInfo.ifInst = simulator->simIF(PC);
    pipelineInfo.ifInst.status = SPECULATIVE;
    if (!hit) {
        iCacheStallCycles = iCache->config.missLatency;
    }

    return SUCCESS;
}


// run for N cycles
Status runCycles(uint64_t cycles)
{
    uint64_t count = 0;
    Status status  = SUCCESS;

    PipeState pipeState = {0};

    while (cycles == 0 || count < cycles) {

        pipeState.cycle = cycleCount;
        count++;
        cycleCount++;

        // snapshot previous pipeline state
        Simulator::Instruction ifPrev  = pipelineInfo.ifInst;
        Simulator::Instruction idPrev  = pipelineInfo.idInst;
        Simulator::Instruction exPrev  = pipelineInfo.exInst;
        Simulator::Instruction memPrev = pipelineInfo.memInst;
        Simulator::Instruction wbPrev  = pipelineInfo.wbInst;

        bool StallID   = false;
        bool BubbleEx  = false;
        bool squashIF  = false;
        bool skipRest  = false;

        bool idPrevIsBranch;
        bool idPrevIsStore;
        bool branchTaken;
        bool illegalInID;
        bool iStall    = (iCacheStallCycles > 0);
        bool dStall    = (dCacheStallCycles > 0);

        Simulator::Instruction newIdInst;

        // default WB
        pipelineInfo.wbInst = nop(BUBBLE);

        /** ============================
         *  D-CACHE MISS STALL HANDLING
         *  ============================ */
        if (dStall) {

            // freeze IF, ID, EX, MEM pipeline registers
            pipelineInfo.ifInst  = ifPrev;
            pipelineInfo.idInst  = idPrev;
            pipelineInfo.exInst  = exPrev;
            pipelineInfo.memInst = memPrev;

            // WB still processes older instruction
            pipelineInfo.wbInst = simulator->simWB(memPrev);

            dCacheStallCycles--;

            // check illegal PC fetch
            if (pipelineInfo.ifInst.PC >= MEMORY_SIZE) {
                pipelineInfo.ifInst = simulator->simIF(0x8000);
                status = ERROR;
            }

            if (pipelineInfo.wbInst.isHalt) {
                status = HALT;
            }

            goto dump_state;
        }

        /** ================
         *  MEM + WB STAGE
         *  ================ */

        // begin forming EX->MEM
        Simulator::Instruction exToMem = exPrev;

        // WB->MEM forwarding for load->store (store data)
        if (exToMem.writesMem && !exToMem.readsMem &&
            exToMem.readsRs2 && exToMem.rs2 != 0)
        {
            if (wbPrev.writesRd && wbPrev.rd == exToMem.rs2 && wbPrev.readsMem) {
                exToMem.op2Val = wbPrev.memResult;
            }
        }

        bool needData = (exToMem.readsMem || exToMem.writesMem);

        // memory exception check
        bool memError = needData && (exToMem.memAddress >= MEMORY_SIZE);

        if (memError) {

            pipelineInfo.wbInst  = simulator->simWB(memPrev);

            // squash EX, ID, IF
            pipelineInfo.memInst = nop(SQUASHED);
            pipelineInfo.exInst  = nop(SQUASHED);
            pipelineInfo.idInst  = nop(SQUASHED);
            pipelineInfo.ifInst  = nop(SQUASHED);

            PC = 0x8000;
            status = ERROR;

            iCacheStallCycles = 0;
            dCacheStallCycles = 0;

            skipRest = true;
        }
        else if (needData) {

            CacheOperation type = exToMem.readsMem ? CACHE_READ : CACHE_WRITE;
            bool hit = dCache->access(exToMem.memAddress, type);

            if (!hit) {
                // miss: EX enters MEM, old MEM enters WB
                pipelineInfo.memInst = simulator->simMEM(exToMem);
                pipelineInfo.wbInst  = simulator->simWB(memPrev);

                dCacheStallCycles = dCache->config.missLatency;
            } else {
                // hit: normal path
                pipelineInfo.memInst = simulator->simMEM(exToMem);
                pipelineInfo.wbInst  = simulator->simWB(memPrev);
            }
        }
        else {
            // no memory access
            pipelineInfo.memInst = simulator->simMEM(exToMem);
            pipelineInfo.wbInst  = simulator->simWB(memPrev);
        }

        if (skipRest) {
            goto dump_state;
        }


        /** =================
         *  HAZARD DETECTION
         *  ================= */

        idPrevIsBranch = (idPrev.opcode == OP_BRANCH) ||
                         (idPrev.opcode == OP_JAL)    ||
                         (idPrev.opcode == OP_JALR);

        idPrevIsStore  = idPrev.writesMem && !idPrev.readsMem;

        // load-branch second cycle
        if (stallCyclesCount > 0) {
            StallID  = true;
            BubbleEx = true;
            stallCyclesCount--;
        } else {

            bool hazardRs1 = idPrev.readsRs1 && (idPrev.rs1 == exPrev.rd);
            bool hazardRs2 = idPrev.readsRs2 && (idPrev.rs2 == exPrev.rd);

            bool loadUse = exPrev.readsMem && exPrev.writesRd &&
                           exPrev.rd != 0 &&
                           !idPrevIsBranch &&
                           (hazardRs1 ||
                           (hazardRs2 && !idPrevIsStore));

            // load-use stall
            if (loadUse) {
                StallID  = true;
                BubbleEx = true;
                loadStallCount++;
            }

            // arith-branch stall
            if (exPrev.doesArithLogic && exPrev.writesRd && exPrev.rd != 0 &&
                idPrevIsBranch && !StallID &&
                ((idPrev.readsRs1 && idPrev.rs1 == exPrev.rd) ||
                 (idPrev.readsRs2 && idPrev.rs2 == exPrev.rd))) {
                StallID  = true;
                BubbleEx = true;
            }

            // load-branch stall (2 cycles)
            if (exPrev.readsMem && exPrev.writesRd && exPrev.rd != 0 &&
                idPrevIsBranch &&
                ((idPrev.readsRs1 && idPrev.rs1 == exPrev.rd) ||
                 (idPrev.readsRs2 && idPrev.rs2 == exPrev.rd)) &&
                !StallID) {
                StallID        = true;
                BubbleEx       = true;
                stallCyclesCount = 1;
                loadStallCount++;
            }
        }


        /** === EX STAGE === */

        if (BubbleEx) {
            pipelineInfo.exInst = nop(BUBBLE);
        } else {
            // forwarding into EX
            forwarding(idPrev.rs1, idPrev.readsRs1, idPrev.op1Val, exPrev, memPrev);
            forwarding(idPrev.rs2, idPrev.readsRs2, idPrev.op2Val, exPrev, memPrev);

            pipelineInfo.exInst = simulator->simEX(idPrev);
        }


        /** === ID STAGE + BRANCH HANDLING === */

        branchTaken = false;
        illegalInID = false;

        if (idPrevIsBranch) {

            // forwarding for branch operands
            forwarding(idPrev.rs1, idPrev.readsRs1, idPrev.op1Val, exPrev, memPrev);
            forwarding(idPrev.rs2, idPrev.readsRs2, idPrev.op2Val, exPrev, memPrev);

            // resolve branch nextPC with forwarded operands
            idPrev = simulator->simNextPCResolution(idPrev);

            if (!StallID && !iStall) {
                branchTaken = (idPrev.nextPC != idPrev.PC + 4);
            }
        }

        if (StallID || iStall) {
            newIdInst = idPrev;
        } else {
            newIdInst = simulator->simID(ifPrev);
        }

        // illegal instruction detected in ID
        if (!StallID && !iStall && !newIdInst.isLegal) {

            illegalInID = true;
            pipelineInfo.idInst = nop(SQUASHED);
            pipelineInfo.ifInst = nop(SQUASHED);

            PC = 0x8000;          // exception handler
            iCacheStallCycles = 0;

            status = ERROR;
        }


        /** === PC + IF CONTROL === */

        if (!illegalInID && !StallID && !iStall) {

            if (idPrevIsBranch) {

                if (branchTaken) {
                    // mispredicted (always-not-taken)
                    newIdInst           = nop(SQUASHED);
                    pipelineInfo.ifInst = nop(SQUASHED);
                    squashIF            = true;

                    PC = idPrev.nextPC;
                    iCacheStallCycles = 0;
                } else {
                    PC = idPrev.PC + 4;
                }

            } else {
                PC = newIdInst.nextPC;
            }
        }

        // update ID stage unless illegal
        if (!illegalInID) {
            pipelineInfo.idInst = newIdInst;
        }


        /** === IF STAGE === */

        if (illegalInID) {
            // IF already squashed above
        }
        else if (StallID) {
            // hold IF
            pipelineInfo.ifInst = ifPrev;
        }
        else if (!squashIF) {

            if (iStall) {
                // I-cache miss continues
                pipelineInfo.ifInst = ifPrev;
                iCacheStallCycles--;
            } else {
                // fetch with I-cache access
                bool hit = iCache->access(PC, CACHE_READ);
                pipelineInfo.ifInst = simulator->simIF(PC);
                pipelineInfo.ifInst.status = SPECULATIVE;

                if (!hit) {
                    iCacheStallCycles = iCache->config.missLatency;
                }
            }
        }

        // PC sanity check
        if (pipelineInfo.ifInst.PC >= MEMORY_SIZE) {
            pipelineInfo.ifInst = simulator->simIF(0x8000);
            status = ERROR;
        }


        // halt at WB
        if (pipelineInfo.wbInst.isHalt) {
            status = HALT;
        }


dump_state:

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

        if (status == HALT) return HALT;
    }

    return status;
}


// run until halt
Status runTillHalt() {
    Status status;
    while (true) {
        status = runCycles(1);
        if (status == HALT || status == ERROR) break;
    }
    return status;
}


// finalize simulation, dump stats + reg/mem
Status finalizeSimulator() {
    simulator->dumpRegMem(output);
    SimulationStats stats{ simulator->getDin(),
                           cycleCount,
                           iCache->getHits(),
                           iCache->getMisses(),
                           dCache->getHits(),
                           dCache->getMisses(),
                           loadStallCount };
    dumpSimStats(stats, output);
    return SUCCESS;
}