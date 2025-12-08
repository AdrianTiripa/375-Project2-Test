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

Simulator::Instruction nop(StageStatus status) {
    Simulator::Instruction nop;
    nop.instruction = 0x00000013; // addi x0,x0,0
    nop.isLegal = true;
    nop.isNop   = true;
    nop.status  = status;
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
static int stallCyclesCount   = 0;  // extra cycle for load-branch
static int loadStallCount     = 0;  // number of load stalls (load-use + load-branch)
static int iCacheStallCycles  = 0;
static int dCacheStallCycles  = 0;

// initialize the simulator
Status initSimulator(CacheConfig& iCacheConfig, CacheConfig& dCacheConfig, MemoryStore* mem,
                     const std::string& output_name) {
    output = output_name;
    simulator = new Simulator();
    simulator->setMemory(mem);
    iCache = new Cache(iCacheConfig, I_CACHE);
    dCache = new Cache(dCacheConfig, D_CACHE);

    cycleCount         = 0;
    PC                 = 0;
    stallCyclesCount   = 0;
    loadStallCount     = 0;
    iCacheStallCycles  = 0;
    dCacheStallCycles  = 0;

    pipelineInfo.ifInst  = nop(IDLE);
    pipelineInfo.idInst  = nop(IDLE);
    pipelineInfo.exInst  = nop(IDLE);
    pipelineInfo.memInst = nop(IDLE);
    pipelineInfo.wbInst  = nop(IDLE);

    return SUCCESS;
}

Status runCycles(uint64_t cycles) {
    uint64_t count = 0;
    auto status    = SUCCESS;

    PipeState pipeState = {0};

    while (cycles == 0 || count < cycles) {
        pipeState.cycle = cycleCount;
        count++;
        cycleCount++;

        // snapshot of previous pipeline state
        Simulator::Instruction ifPrev  = pipelineInfo.ifInst;
        Simulator::Instruction idPrev  = pipelineInfo.idInst;
        Simulator::Instruction exPrev  = pipelineInfo.exInst;
        Simulator::Instruction memPrev = pipelineInfo.memInst;
        Simulator::Instruction wbPrev  = pipelineInfo.wbInst;

        bool StallID  = false;
        bool BubbleEx = false;

        bool iStall   = (iCacheStallCycles > 0);
        bool dStall   = (dCacheStallCycles > 0);

        // Default WB is bubble; we'll overwrite as needed
        pipelineInfo.wbInst = nop(BUBBLE);

        /**********************
         *  D-CACHE / MEM / WB
         **********************/

        // If a D-cache miss is already in progress, freeze MEM/EX/ID/IF
        if (dStall) {
            pipelineInfo.memInst = memPrev;
            pipelineInfo.exInst  = exPrev;
            pipelineInfo.idInst  = idPrev;
            pipelineInfo.ifInst  = ifPrev;

            // nothing new retires
            pipelineInfo.wbInst = nop(BUBBLE);

            // pay one cycle of the D-cache miss
            dCacheStallCycles--;

            // I-cache miss (if any) continues to count down in parallel
            if (iCacheStallCycles > 0) {
                iCacheStallCycles--;
            }

            // dump state and proceed to next cycle
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

            if (pipelineInfo.wbInst.isHalt) {
                return HALT;
            }
            continue;
        }

        // No D-cache stall currently in progress:
        // EX -> MEM and MEM -> WB, with potential new miss detection

        // Start from EX value that goes into MEM
        Simulator::Instruction exToMem = exPrev;

        // WB->EX forwarding for load->store data (store rs2)
        if (exToMem.writesMem && !exToMem.readsMem &&
            exToMem.readsRs2 && exToMem.rs2 != 0) {
            if (wbPrev.writesRd && wbPrev.rd == exToMem.rs2 && wbPrev.readsMem) {
                exToMem.op2Val = wbPrev.memResult;
            }
        }

        bool exMemAccess = exToMem.readsMem || exToMem.writesMem;

        // Perform MEM/WB pipeline movement
        pipelineInfo.wbInst  = simulator->simWB(memPrev);
        pipelineInfo.memInst = simulator->simMEM(exToMem);

        // D-cache access (only when there is a real mem access and no miss in progress)
        if (exMemAccess) {
            CacheOperation type = exToMem.readsMem ? CACHE_READ : CACHE_WRITE;
            bool hit = dCache->access(exToMem.memAddress, type);
            if (!hit) {
                // start D-cache miss penalty; next cycles will freeze pipeline around MEM
                dCacheStallCycles = dCache->config.missLatency;
            }
        }

        /**********************
         *  DATA HAZARDS
         **********************/

        bool idPrevIsBranch = (idPrev.opcode == OP_BRANCH) ||
                              (idPrev.opcode == OP_JAL)    ||
                              (idPrev.opcode == OP_JALR);

        bool idPrevIsStore  = idPrev.writesMem && !idPrev.readsMem;

        if (stallCyclesCount > 0) {
            // second cycle of a load-branch stall (2-cycle hazard)
            StallID  = true;
            BubbleEx = true;
            stallCyclesCount--;
        } else {
            // load-use (1-cycle stall)
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

            // arith-branch (1-cycle stall)
            if (exPrev.doesArithLogic && exPrev.writesRd && exPrev.rd != 0 &&
                idPrevIsBranch && !StallID &&
                (idPrev.rs1 == exPrev.rd || idPrev.rs2 == exPrev.rd)) {
                StallID  = true;
                BubbleEx = true;
            }

            // load-branch (2-cycle stall total)
            if (exPrev.readsMem && exPrev.writesRd && exPrev.rd != 0 &&
                idPrevIsBranch &&
                (idPrev.rs1 == exPrev.rd || idPrev.rs2 == exPrev.rd) &&
                !StallID) {
                StallID        = true;
                BubbleEx       = true;
                stallCyclesCount = 1; // one more cycle after this
                loadStallCount++;
            }
        }

        /**********************
         *  EX STAGE
         **********************/

        if (BubbleEx) {
            pipelineInfo.exInst = nop(BUBBLE);
        } else {
            // forwarding into EX operands
            forwarding(idPrev.rs1, idPrev.readsRs1, idPrev.op1Val, exPrev, memPrev);
            forwarding(idPrev.rs2, idPrev.readsRs2, idPrev.op2Val, exPrev, memPrev);
            pipelineInfo.exInst = simulator->simEX(idPrev);
        }

        /**********************
         *  BRANCH RESOLUTION IN ID
         **********************/

        bool branchTaken = false;
        Simulator::Instruction branchInst = idPrev;

        if (idPrevIsBranch) {
            // forwarding for branch operands in ID
            forwarding(branchInst.rs1, branchInst.readsRs1, branchInst.op1Val, exPrev, memPrev);
            forwarding(branchInst.rs2, branchInst.readsRs2, branchInst.op2Val, exPrev, memPrev);

            // recompute nextPC for branch
            branchInst = simulator->simNextPCResolution(branchInst);

            // always-not-taken predictor:
            // detect mispredict when not data-stalled or I-cache stalled
            if (!StallID && !iStall) {
                branchTaken = (branchInst.nextPC != branchInst.PC + 4);
            }
        }

        /**********************
         *  ID STAGE UPDATE
         **********************/

        Simulator::Instruction newIdInst = idPrev; // default: hold

        if (!StallID && !iStall) {
            if (idPrevIsBranch && branchTaken) {
                // squash the speculatively fetched instruction from IF
                newIdInst = nop(SQUASHED);
            } else {
                // normal IF->ID decode
                newIdInst = simulator->simID(ifPrev);
            }
        }

        // write back ID stage
        pipelineInfo.idInst = newIdInst;

        /**********************
         *  PC UPDATE
         **********************/

        if (!StallID && !iStall) {
            if (idPrevIsBranch) {
                if (branchTaken) {
                    // mispredict: redirect to branch target
                    PC = branchInst.nextPC;
                } else {
                    // predicted not taken; sequential
                    PC = branchInst.PC + 4;
                }
            } else {
                // non-branch: follow decoded nextPC of newIdInst
                PC = newIdInst.nextPC;
            }
        }

        /**********************
         *  IF STAGE + I-CACHE
         **********************/

        if (StallID) {
            // hold IF during data hazard stall
            pipelineInfo.ifInst = ifPrev;
        } else if (iStall) {
            // I-cache miss in progress: hold IF, let stall counters tick down
            pipelineInfo.ifInst = ifPrev;
            iCacheStallCycles--;
        } else {
            // normal fetch with I-cache access at PC
            bool hit = iCache->access(PC, CACHE_READ);
            pipelineInfo.ifInst = simulator->simIF(PC);
            pipelineInfo.ifInst.status = SPECULATIVE; // IF is always speculative

            if (!hit) {
                // miss penalty cycles; instruction will sit in IF until done
                iCacheStallCycles = iCache->config.missLatency;
            }
        }

        /**********************
         *  HALT CHECK + DUMP
         **********************/

        if (pipelineInfo.wbInst.isHalt) {
            status = HALT;
        }

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

        if (status == HALT) return status;
    }

    return status;
}

// run till halt (call runCycles() with cycles == 1 each time)
Status runTillHalt() {
    Status status;
    while (true) {
        status = static_cast<Status>(runCycles(1));
        if (status == HALT) break;
    }
    return status;
}

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