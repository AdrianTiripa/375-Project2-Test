#include "cycle.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

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
    Simulator::Instruction ifInst = nop(IDLE);
    Simulator::Instruction idInst = nop(IDLE);
    Simulator::Instruction exInst = nop(IDLE);
    Simulator::Instruction memInst = nop(IDLE);
    Simulator::Instruction wbInst = nop(IDLE);
} pipelineInfo;


// keep track of the number of cycles stall is applied
static uint64_t loadStallCount = 0;
static uint64_t iCacheStallCycles = 0;
static uint64_t dCacheStallCycles = 0;


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
    loadStallCount = 0;
    iCacheStallCycles = 0;
    dCacheStallCycles = 0;

    // start with an empty pipeline (all IDLE NOPs)
    pipelineInfo.ifInst = nop(IDLE);
    pipelineInfo.idInst = nop(IDLE);
    pipelineInfo.exInst = nop(IDLE);
    pipelineInfo.memInst = nop(IDLE);
    pipelineInfo.wbInst = nop(IDLE);

    // NOTE: we no longer do an "initial fetch" here.
    // The first call to runCycles(..) will perform the first simIF(PC)
    // so that cycle 0 shows an idle pipeline, matching the reference output.

    return SUCCESS;
}

// run the simulator for a certain number of cycles
// return SUCCESS if reaching desired cycles.
// return HALT if the simulator halts on 0xfeedfeed

Status runCycles(uint64_t cycles) {
    uint64_t count = 0;
    auto status = SUCCESS;

    PipeState pipeState = {
        0,
    };


    while (cycles == 0 || count < cycles) {

        pipeState.cycle = cycleCount;
        count++;
        cycleCount++;
    
        // to keep track of the previous instructions in each 
        // stage before the cycle is executed
        Simulator::Instruction ifPrev = pipelineInfo.ifInst;
        Simulator::Instruction idPrev = pipelineInfo.idInst;
        Simulator::Instruction exPrev = pipelineInfo.exInst;
        Simulator::Instruction memPrev = pipelineInfo.memInst;

        // WB
        pipelineInfo.wbInst = nop(BUBBLE);
        
        // MEM
        
        // D-Cache Stall

        // first check for a memory exception on the address that will access D-Cache
        bool memError = (exPrev.readsMem || exPrev.writesMem) &&
                        (exPrev.memAddress >= MEMORY_SIZE);

        // Checks for a dCache Miss
        // QUESTION: Do we have to implement zero register nullification?
        // IMPLEMENT I-CHACHE STALL CYCLES HERE
        bool dCacheStall = !memError && (
            (exPrev.readsMem && !dCache->access(exPrev.memAddress, CACHE_READ)) ||
            (exPrev.writesMem && !dCache->access(exPrev.memAddress, CACHE_WRITE)));
        

        // Case: Currently stalling for dCache miss latency
        if (dCacheStallCycles != 0) {
            /*
             * We are in the "middle" of a D-cache miss.
             * The miss-causing instruction should remain in MEM and
             * younger instructions should be stalled behind it.
             * Older instructions have already committed in the cycle
             * when the miss was first detected, so we DO NOT re-execute WB here.
             */
            pipelineInfo.wbInst = nop(BUBBLE);
            dCacheStallCycles--;
        }
        
        // Case: memory out of bounds (memory exception)
        else if (memError) {
            pipelineInfo.wbInst = simulator->simWB(memPrev);
            pipelineInfo.memInst = nop(SQUASHED);
            pipelineInfo.exInst = nop(SQUASHED);
            pipelineInfo.idInst = nop(SQUASHED);
            PC = 0x8000;
            pipelineInfo.ifInst = simulator->simIF(PC);
        }

        // Case: dCache miss on current cycle
        else if (dCacheStall) {
            /*
             * On the cycle we detect a miss:
             *  - The older instruction in MEM (if any) commits.
             *  - The load/store moves EX -> MEM.
             *  - We start counting miss latency.
             * On subsequent stall cycles we will not WB again.
             */
            pipelineInfo.wbInst = simulator->simWB(memPrev);
            pipelineInfo.memInst = simulator->simMEM(exPrev);
            dCacheStallCycles = dCache->config.missLatency;
        }

        // Case: dCache hit OR memory not accessed
        else {
            // WB
            pipelineInfo.wbInst = simulator->simWB(memPrev);
            // MEM
            pipelineInfo.memInst = simulator->simMEM(exPrev);

            // Hazard Detection
            
            bool isBranch = !idPrev.isNop && ((idPrev.opcode == OP_BRANCH) || 
                            (idPrev.opcode == OP_JAL) ||
                            (idPrev.opcode == OP_JALR));

            // Handle load-use Stalls (including load followed by branch)
            bool loadUse = !idPrev.isNop && !exPrev.isNop &&
                        (idPrev.rs1 == exPrev.rd || idPrev.rs2 == exPrev.rd) && 
                        exPrev.opcode == OP_LOAD && exPrev.rd != 0;

            // separate out "plain" load-use vs load-branch first stall
            bool loadUseBranch = loadUse && isBranch;
            bool pureLoadUse   = loadUse && !isBranch;
            
            // Handle Arith-Branch Stalls
            bool arithBranch = isBranch && exPrev.doesArithLogic && exPrev.writesRd && exPrev.rd != 0 &&
                         (idPrev.rs1 == exPrev.rd || idPrev.rs2 == exPrev.rd);
            
            // Handle LoadBranch
            // NOTE: the only time a nop is inserted between a load and a branch
            // is when there is a LoadBranch Stall. True?
            // Here, this corresponds to the SECOND stall cycle for a load-branch
            // dependency (the load is now in MEM and EX is a bubble).
            bool loadBranch = isBranch && (memPrev.opcode == OP_LOAD) && exPrev.isNop;

            // Ex

            // Case: Load-Use (including the first cycle of a load-branch stall)
            if (pureLoadUse || loadUseBranch) {
                // Handle forwarding from MEM stage to the ID stage
                // Dead code commented out
                pipelineInfo.idInst.op1Val = (/*pipelineInfo.memInst.writesRd &&*/ (pipelineInfo.memInst.rd == idPrev.rs1)) 
                                                ? pipelineInfo.memInst.memResult:
                                                pipelineInfo.idInst.op1Val;
                pipelineInfo.idInst.op2Val = (/*pipelineInfo.memInst.writesRd &&*/ (pipelineInfo.memInst.rd == idPrev.rs2)) 
                                                ? pipelineInfo.memInst.memResult:
                                                pipelineInfo.idInst.op2Val;
                if (iCacheStallCycles != 0)
                    iCacheStallCycles -= 1;
                // insert bubble
                pipelineInfo.exInst = nop(BUBBLE);
                // count this load stall only for "plain" load-use
                // (load-branch will be counted once in the loadBranch case below)
                if (pureLoadUse)
                    loadStallCount++;
            } 

            // Case: Arith-Branch Stall
            else if (arithBranch) {
                // Handle forwarding from EX stage to the ID stage
                pipelineInfo.idInst.op1Val = (/*exPrev.writesRd &&*/ (exPrev.rd == idPrev.rs1)) 
                                                ? exPrev.arithResult:
                                                pipelineInfo.idInst.op1Val;
                pipelineInfo.idInst.op2Val = (/*exPrev.writesRd &&*/ (exPrev.rd == idPrev.rs2)) 
                                                ? exPrev.arithResult:
                                                pipelineInfo.idInst.op2Val;
                if (iCacheStallCycles != 0)
                    iCacheStallCycles -= 1;
                // insert bubble
                pipelineInfo.exInst = nop(BUBBLE);
                // Update PC Resolution
                pipelineInfo.idInst = simulator->simNextPCResolution(idPrev);
            }

            // Case: Load-Branch Stall (second cycle)
            else if (loadBranch) {
                // Handle forwarding from the memory register to the ID stage
                pipelineInfo.idInst.op1Val = (pipelineInfo.wbInst.writesRd && (pipelineInfo.wbInst.rd == idPrev.rs1)) 
                                                ? pipelineInfo.wbInst.memResult:
                                                pipelineInfo.idInst.op1Val;
                pipelineInfo.idInst.op2Val = (pipelineInfo.memInst.writesRd && (pipelineInfo.memInst.rd == idPrev.rs2)) 
                                                ? pipelineInfo.memInst.memResult:
                                                pipelineInfo.idInst.op2Val;
                if (iCacheStallCycles != 0)
                    iCacheStallCycles -= 1;
                // insert bubble
                pipelineInfo.exInst = nop(BUBBLE);
                // update PC resolution if needed
                pipelineInfo.idInst = simulator->simNextPCResolution(idPrev);
                // count this load stall (load-branch counts ONCE even though there are 2 stall cycles)
                loadStallCount++;
            }

            // Case: No Stalls
            else {
                // normal case
                // Do FORWARDING between prevMem and Ex and/or prevEx and Ex
                idPrev.op1Val = (exPrev.writesRd && (exPrev.rd == idPrev.rs1) && exPrev.doesArithLogic) ? exPrev.arithResult:
                                (memPrev.writesRd && (memPrev.rd == idPrev.rs1) && memPrev.readsMem) ? memPrev.memResult:
                                (memPrev.writesRd && (memPrev.rd == idPrev.rs1) && memPrev.doesArithLogic) ? memPrev.arithResult:
                                idPrev.op1Val;

                idPrev.op2Val = (exPrev.writesRd && (exPrev.rd == idPrev.rs2) && exPrev.doesArithLogic) ? exPrev.arithResult:
                                (memPrev.writesRd && (memPrev.rd == idPrev.rs2) && memPrev.readsMem) ? memPrev.memResult:
                                (memPrev.writesRd && (memPrev.rd == idPrev.rs2) && memPrev.doesArithLogic) ? memPrev.arithResult:
                                idPrev.op2Val;

                pipelineInfo.exInst = simulator->simEX(idPrev);
                
                //  ID + IF

                // Handle always not-taken branch handling    
                bool idSquash = isBranch && (idPrev.nextPC != idPrev.PC + 4);

                // Case: Branch is taken
                if (idSquash) {
                    pipelineInfo.idInst = nop(SQUASHED);
                    /*
                    NOTE
                    I am assuming here that if there is a branch which will change the
                    PC then we are to abort the stall cycles for the 
                    */
                    if (iCacheStallCycles != 0) {
                        iCache->invalidate(PC);
                        iCacheStallCycles = 0; 
                    }
                    PC = idPrev.nextPC;
                    pipelineInfo.ifInst = simulator->simIF(PC);
                    // Case: New PC misses in iCache
                    if (!iCache->access(PC, CACHE_READ)) 
                        iCacheStallCycles = iCache->config.missLatency;
                    
                }

                // Case: Branch is not taken OR no branch
                else {
                    pipelineInfo.idInst = simulator->simID(ifPrev);
                    // 0x8000 is the error handling address
                    PC = (!pipelineInfo.idInst.isLegal) ? 0x8000 :
                        pipelineInfo.idInst.nextPC;
                    // Case: Illegal Instruction in ID
                    if (!pipelineInfo.idInst.isLegal) {
                        pipelineInfo.idInst = nop(SQUASHED);
                        pipelineInfo.ifInst = simulator->simIF(PC);
                    }
                    // Case: Still Stalling from previous iCache miss
                    else if (iCacheStallCycles > 0) {
                        pipelineInfo.idInst = nop(BUBBLE);
                        iCacheStallCycles--;
                    }
                    // Case: iCache miss
                    else if (!iCache->access(PC, CACHE_READ)) {
                        pipelineInfo.ifInst = simulator->simIF(PC);
                        iCacheStallCycles = iCache->config.missLatency;
                    } else {
                        pipelineInfo.ifInst = simulator->simIF(PC);
                    }
                }
            }
        }


        // (No ERROR status on IF here; if PC is bad and they want a memory exception
        // they will encode that via the program / tests; otherwise we just keep running.)

        // WB Check for halt instruction
        if (pipelineInfo.wbInst.isHalt) {
            status = HALT;
            break;
        }
        
        // We'll need to add the stats
    }

    // Finally, update the pipeline
    pipeState.ifPC = pipelineInfo.ifInst.PC;
    pipeState.ifStatus = pipelineInfo.ifInst.status;
    pipeState.idInstr = pipelineInfo.idInst.instruction;
    pipeState.idStatus = pipelineInfo.idInst.status;
    pipeState.exInstr = pipelineInfo.exInst.instruction;
    pipeState.exStatus = pipelineInfo.exInst.status;
    pipeState.memInstr = pipelineInfo.memInst.instruction;
    pipeState.memStatus = pipelineInfo.memInst.status;
    pipeState.wbInstr = pipelineInfo.wbInst.instruction;
    pipeState.wbStatus = pipelineInfo.wbInst.status;
    dumpPipeState(pipeState, output);
    return status;
}

// run till halt (call runCycles() with cycles == 1 each time) until
// status tells you to HALT or ERROR out
Status runTillHalt() {
    Status status;
    while (true) {
        status = static_cast<Status>(runCycles(1));
        if (status == HALT || status == ERROR) break; // CHECK ON THIS LATER
    }
    return status;
}

// dump the state of the simulator
Status finalizeSimulator() {
    simulator->dumpRegMem(output);
    SimulationStats stats{simulator->getDin(),  cycleCount, iCache->getHits(), 
                iCache->getMisses(), dCache->getHits(), dCache->getMisses(), (loadStallCount)}; 
    dumpSimStats(stats, output);
    return SUCCESS;
}