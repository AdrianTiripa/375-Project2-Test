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
static int stallCyclesCount = 0;
static int iCacheStallCycles = 0;


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
        pipelineInfo.memInst = simulator->simMEM(exPrev);
        
        bool StallID    = false;
        bool BubbleEx   = false;

        // Hazard Detection
        
        bool idPrevisBranch = (idPrev.opcode == OP_BRANCH) || 
                        (idPrev.opcode == OP_JAL) ||
                        (idPrev.opcode == OP_JALR);
        
        if(stallCyclesCount > 0){
            StallID = true;
            BubbleEx = true;
            stallCyclesCount --;
        }
        else{
        // Handle load-use stalls
            if((idPrev.rs1 == exPrev.rd ||
                idPrev.rs2 == exPrev.rd) && 
                exPrev.readsMem && exPrev.writesRd
                && exPrev.rd != 0 && !idPrevisBranch)
            {
                BubbleEx = true;
            }

            // Handle arith-branch
            // CHANGE TO CHECK FOR BEQ/BNE, etc
            if(exPrev.doesArithLogic && exPrev.writesRd && exPrev.rd != 0
            && idPrevisBranch && 
            (idPrev.rs1 == exPrev.rd || idPrev.rs2 == exPrev.rd))
            {            
                StallID = true;
            }

            // Handle load-branch
            if(exPrev.readsMem && exPrev.writesRd && exPrev.rd != 0
                && idPrevisBranch && (idPrev.rs1 == exPrev.rd ||
                idPrev.rs2 == exPrev.rd))
            {
                StallID = true;
                BubbleEx = true;
                stallCyclesCount = 1;
            }
        }

        if(BubbleEx) {
            // insert bubble
            pipelineInfo.exInst = nop(BUBBLE);
        } else {

            // normal case
            // Do FORWARDING between prevMem and Ex and/or prevEx and Ex

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

            pipelineInfo.exInst = simulator->simEX(idPrev);
        }

        // Do FORWARDING between prevMem and Ex and/or prevEx and Ex (done)
        
    // ID
    
        Simulator::Instruction newIdInst;

        if(StallID){
            // do not advance IF inst to ID
            newIdInst = idPrev;
        } else {
            // normal
            newIdInst = simulator->simID(ifPrev);
        }

        // DO Forwarding to ID for braches
        bool newIsBranch = (newIdInst.opcode == OP_BRANCH) || 
                           (newIdInst.opcode == OP_JAL)    ||
                           (newIdInst.opcode == OP_JALR);

        if (newIsBranch) {
            // rs1 forwarding for branch
            if (newIdInst.readsRs1 && newIdInst.rs1 != 0) {
                // From EX/MEM  (exPrev)
                if (exPrev.writesRd && exPrev.rd == newIdInst.rs1 &&
                    exPrev.doesArithLogic) {
                    newIdInst.op1Val = exPrev.arithResult;
                }
                // From MEM  (memPrev)
                if (memPrev.writesRd && memPrev.rd == newIdInst.rs1) {
                    if (memPrev.readsMem) {
                        newIdInst.op1Val = memPrev.memResult;
                    } else if (memPrev.doesArithLogic) {
                        newIdInst.op1Val = memPrev.arithResult;
                    }
                }
            }

            // rs2 forwarding for branch
            if (newIdInst.readsRs2 && newIdInst.rs2 != 0) {
                // From EX/MEM  (exPrev)
                if (exPrev.writesRd && exPrev.rd == newIdInst.rs2 &&
                    exPrev.doesArithLogic) {
                    newIdInst.op2Val = exPrev.arithResult;
                }
                // From MEM (memPrev)
                if (memPrev.writesRd && memPrev.rd == newIdInst.rs2) {
                    if (memPrev.readsMem) {
                        newIdInst.op2Val = memPrev.memResult;
                    } else if (memPrev.doesArithLogic) {
                        newIdInst.op2Val = memPrev.arithResult;
                    }
                }

            }
        }

        bool taken = isBranch && (newIdInst.nextPC != newIdInst.PC + 4);

    //  ID + IF
        if(!StallID) {
            // if branch taken, then flush the if inst and move PC to 
            // the address given by the branch
            if(taken){
                pipelineInfo.idInst = nop(SQUASHED);
                PC = newIdInst.nextPC;
                /*
                NOTE
                I am assuming here that if there is a branch which will change the
                PC then we are to abort the stall cycles for the 
                */
                iCacheStallCycles = 0;
                pipelineInfo.ifInst = simulator->simIF(PC);
            }
            else if (iCacheStallCycles >0) {
                // not taken, then just move on with PC + 4
                pipelineInfo.idInst = newIdInst;
                pipelineInfo.ifInst = nop(IDLE);
                iCacheStallCycles--;
            }
            else if(!iCache->access()) {
                pipelineInfo.idInst = newIdInst;
                pipelineInfo.ifInst = nop(IDLE);
                iCacheStallCycles = iCache->config->missLatency;
            } else {
                pipelineInfo.idInst = newIdInst;
                pipelineInfo.ifInst = simulator->simIF(PC);
            }

        }
        else {

        }
        // update pipeline's ID inst
        pipelineInfo.idInst = newIdInst; 

        // DO exceptions
        

// Previous Code
        /* // Handle load-use stalls
        if((pipelineInfo.idInst.rs1 == pipelineInfo.memInst.rd ||
            pipelineInfo.idInst.rs2 == pipelineInfo.memInst.rd) && 
            pipelineInfo.memInst.readsMem){
            pipelineInfo.exInst = nop(BUBBLE);

            // handle memory stage forwarding (reduce load-use stall to 1 from 2)
            if(pipelineInfo.idInst.rs1 == pipelineInfo.memInst.rd)
                pipelineInfo.idInst.regData[pipelineInfo.idInst.rs1] = pipelineInfo.memInst.regData[pipelineInfo.memIsnt.rd];
            else if(pipelineInfo.idInst.rs2 == pipelineInfo.memInst.rd)
                pipelineInfo.idInst.regData[pipelineInfo.idInst.rs2] = pipelineInfo.memInst.regData[pipelineInfo.memIsnt.rd];

        } else {

            // handle execute stage forwarding (no stall at all)
            if(pipelineInfo.idInst.rs1 == pipelineInfo.exInst.rd)
                pipelineInfo.idInst.regData[pipelineInfo.idInst.rs1] = pipelineInfo.exInst.regData[pipelineInfo.exInst.rd];
            else if(pipelineInfo.idInst.rs2 == pipelineInfo.memInst.rd)
                pipelineInfo.idInst.regData[pipelineInfo.idInst.rs2] = pipelineInfo.exInst.regData[pipelineInfo.exInst.rd];

            pipelineInfo.exInst = simulator->simEX(pipelineInfo.idInst);
            // handle branch stalls
            if(pipelineInfo.exInst.nextPC != pipelineInfo.exInst.PC+4){
                pipelineInfo.idInst = nop(SQUASHED);

                // handle branch forwarding
                pipelineInfo.ifInst.PC = pipelineInfo.exInst.nextPC;
            }
            else 
                pipelineInfo.idInst = simulator->simID(pipelineInfo.ifInst);

            pipelineInfo.ifInst = simulator->simIF(PC);
        }*/

        // Stall Checking Conditions
        // To be implemented
        // Load-Use: 1 cycle
        // Arithmatic Branch: 1 cycle
        // Load-branch: 2 cycle

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
    SimulationStats stats{simulator->getDin(),  cycleCount, 0, 0, 0, 0, 0};  // TODO incomplete implementation
    dumpSimStats(stats, output);
    return SUCCESS;
}
