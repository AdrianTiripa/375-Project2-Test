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
    Simulator::Instruction ifInst = nop(IDLE);
    Simulator::Instruction idInst = nop(IDLE);
    Simulator::Instruction exInst = nop(IDLE);
    Simulator::Instruction memInst = nop(IDLE);
    Simulator::Instruction wbInst = nop(IDLE);
} pipelineInfo;


// initialize the simulator
Status initSimulator(CacheConfig& iCacheConfig, CacheConfig& dCacheConfig, MemoryStore* mem,
                     const std::string& output_name) {
    output = output_name;
    simulator = new Simulator();
    simulator->setMemory(mem);
    iCache = new Cache(iCacheConfig, I_CACHE);
    dCache = new Cache(dCacheConfig, D_CACHE);
    return SUCCESS;
}

// run the simulator for a certain number of cycles
// return SUCCESS if reaching desired cycles.
// return HALT if the simulator halts on 0xfeedfeed

// cache

// keep track of the number of cycles stall is applied
static int loadStallCount = 0;
static int iCacheStallCycles = 0;
static int dCacheStallCycles = 0;

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
        pipelineInfo.wbInst = simulator->simWB(memPrev);
    
    // MEM
        
    // D-Cache Stall

        // Checks for a dCache Miss
        // QUESTION: Do we have to implement zero register nullification?
        bool dCacheStall = exPrev.readsMem && !dCache->access(exPrev.memAddress, CACHE_READ) ||
                            exPrev.writesMem && !dCache->access(exPrev.memAddress, CACHE_WRITE);

        // Case: Currently stalling for dCache miss latency
        if(dCacheStallCycles != 0){
            pipelineInfo.memInst = nop(BUBBLE);
            dCacheStallCycles--;
        }
        // Case: dCache miss on current cycle
        else if (dCacheStall){
            pipelineInfo.memInst = nop(BUBBLE);
            dCacheStallCycles = dCache->config->missLatency;
        }
        // Case: dCache hit OR memory not accessed
        else {
            
            bool StallID    = false;
            bool BubbleEx   = false;

            // Hazard Detection
            
            bool isBranch = (idPrev.opcode == OP_BRANCH) || 
                            (idPrev.opcode == OP_JAL) ||
                            (idPrev.opcode == OP_JALR);

            // Handle load-use stalls
            BubbleEx = (idPrev.rs1 == exPrev.rd || idPrev.rs2 == exPrev.rd) && exPrev.readsMem && exPrev.writesRd
                        && exPrev.rd != 0 && !idPrevisBranch;

            // Ex

            // Case: Load-Use Stall
            if(BubbleEx) {

                // Handle forwarding from the memory register to the ID stage
                pipelineInfo.idInst.op1Val = (pipelineInfo.memInst.writesRd && (pipelineInfo.memInst.rd 
                                    == idPrev.rs1) && memPrev) ? pipelineInfo.memInst.memResult;
                pipelineInfo.idInst.op2Val = (pipelineInfo.memInst.writesRd && (pipelineInfo.memInst.rd 
                                    == idPrev.rs2) && memPrev.readsMem) ? pipelineInfo.memInst.memResult;
                iCacheStallCycles = max(0, iCacheStallCycles - 1);
                // insert bubble
                pipelineInfo.exInst = nop(BUBBLE);
            } 
            // Case: No Load-Use Stall
            else {
                // normal case
                // Do FORWARDING between prevMem and Ex and/or prevEx and Ex
                idPrev.op1Val = (exPrev.writesRd && exPrev.rd == idPrev.rs1 && exPrev.doesArithLogic) ? exPrev.arithResult;
                idPrev.op2Val = (exPrev.writesRd && exPrev.rd == idPrev.rs2 && exPrev.doesArithLogic) ? exPrev.arithResult;
                
                /*
                Making slight changes to forwarding to prevent mem from writing over ex
                OLD CODE

                // rs1 forwarding - these can be put in helper functions!!
                if (idPrev.readsRs1 && idPrev.rs1 != 0) {

                    // From EX/MEM (exPrev) - ALU result is ready here
                    if (exPrev.writesRd && exPrev.rd == idPrev.rs1 &&
                        exPrev.doesArithLogic) {
                        idPrev.op1Val = exPrev.arithResult;
                    }
                    // From MEM  (memPrev) - load result is ready here
                    if (memPrev.writesRd && memPrev.rd == idPrev.rs1) {
                        if (memPrev.readsMem) {
                            idPrev.op1Val = memPrev.memResult;
                        } else if (memPrev.doesArithLogic) {
                            idPrev.op1Val = memPrev.arithResult;
                        }
                    }
                    // From WB stage (just wrote back this cycle)- SORRY,
                    // I deleted it, we dont need it
                
                }
                // rs2 forwarding
                if (idPrev.readsRs2 && idPrev.rs2 != 0) {
                    // From EX/MEM (exPrev)
                    if (exPrev.writesRd && exPrev.rd == idPrev.rs2 &&
                        exPrev.doesArithLogic) {
                        idPrev.op2Val = exPrev.arithResult;
                    }
                    // From MEM (memPrev)
                    if (memPrev.writesRd && memPrev.rd == idPrev.rs2) {
                        if (memPrev.readsMem) {
                            idPrev.op2Val = memPrev.memResult;
                        } else if (memPrev.doesArithLogic) {
                            idPrev.op2Val = memPrev.arithResult;
                        }
                    }        
                }
                */
                pipelineInfo.exInst = simulator->simEX(idPrev);
                
                // ID
            
                //  ID + IF

                // Handle arith-branch
                // Question: Should we change to check for BEQ/BNE/ Arith branches?      
                StallID = isBranch && (idPrev.nextPC != idPrev.PC + 4);

                // Case: Branch is taken
                if(StallID){
                    loadStallCount++;
                    pipelineInfo.idInst = nop(SQUASHED);
                    PC = idPrev.nextPC;
                    /*
                    NOTE
                    I am assuming here that if there is a branch which will change the
                    PC then we are to abort the stall cycles for the 
                    */
                    iCacheStallCycles = 0;
                    pipelineInfo.ifInst = simulator->simIF(PC);
                }
                // Case: Branch is not taken OR no branch
                else {
                    pipelineInfo.idInst = simulator->simID(ifPrev);
                    PC = (!pipelineInfo.idInst.isLegal) 0x8000:
                    pipelineInfo.idInst.nextPC;
                    // Case: Illegal Instruction in ID
                    if(!pipelineInfo.idInst.isLegal){
                        pipelineInfo.idInst = nop(SQUASHED);
                        // 0x8000 is the error handling address
                        pipelineInfo.ifInst = simulator->simIF(0x8000);
                    }
                    // Case: Still Stalling from previous iCache miss
                    else if (iCacheStallCycles > 0) {
                        // not taken, then just move on with PC + 4
                        pipelineInfo.ifInst = nop(IDLE);
                        iCacheStallCycles--;
                    }
                    // Case: iCache miss
                    else if(!iCache->access(PC, CACHE_READ)) {
                        pipelineInfo.ifInst = nop(IDLE);
                        iCacheStallCycles = iCache->config->missLatency;
                    } 
                    // Case: iCache hit and legal instruction
                    else {
                        pipelineInfo.ifInst = simulator->simIF(PC);
                    }
                }

            }
        }

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
        if (status == HALT) break;
    }
    return status;
}

// dump the state of the simulator
Status finalizeSimulator() {
    simulator->dumpRegMem(output);
    SimulationStats stats{simulator->getDin(),  cycleCount, iCache->getHits(), 
                iCache->getMisses(), dCache->getHits(), dCache->getMisses(), loadStallCount}; 
    dumpSimStats(stats, output);
    return SUCCESS;
}
