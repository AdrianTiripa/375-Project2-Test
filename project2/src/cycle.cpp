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
    
    // MEM
        
    // D-Cache Stall

        // Checks for a dCache Miss
        // QUESTION: Do we have to implement zero register nullification?
        bool dCacheStall = exPrev.readsMem && !dCache->access(exPrev.memAddress, CACHE_READ) ||
                            exPrev.writesMem && !dCache->access(exPrev.memAddress, CACHE_WRITE);

        // Case: Currently stalling for dCache miss latency
        if(dCacheStallCycles != 0){
            pipelineInfo.wbInst = nop(BUBBLE);
            dCacheStallCycles--;
        }
        
        // Case: dCache miss on current cycle
        else if (dCacheStall){
            bool memError = (pipelineInfo.memInst.readsMem || pipelineInfo.memInst.writeMem
                        && pipelineInfo.memInst.memAddress >= MEMORY_SIZE);

             // Case: Just cache miss, memory in bounds
            if(!memError){
                pipelineInfo.wbInst = nop(BUBBLE);
                pipelineInfo.memInst = simulator->simMEM(exPrev);
                dCacheStallCycles = dCache->config->missLatency;
            } 
            // Case: Memory out of bounds
            else {
                pipelineInfo.wbInst = simulator->simWB(memPrev);
                pipelineInfo.memInst = NOP(SQUASHED);
                pipelineInfo.exInst = NOP(SQUASHED);
                pipelineInfo.idInst = NOP(SQUASHED);
                pipelineInfo.ifInst = simulator->simIF(0x8000);
                status = ERROR;
            }
        }
        // Case: dCache hit OR memory not accessed
        else {
            // WB
            pipelineInfo.wbInst = simulator->simWB(memPrev);
            // MEM
            pipelineInfo.memInst = simulator->simMEM(exPrev);

            // Hazard Detection
            
            bool isBranch = (idPrev.opcode == OP_BRANCH) || 
                            (idPrev.opcode == OP_JAL) ||
                            (idPrev.opcode == OP_JALR);

            // Handle load-use Stalls
            bool loadUse = (idPrev.rs1 == exPrev.rd || idPrev.rs2 == exPrev.rd) && 
                        exPrev.opcode == OP_LOAD && exPrev.rd != 0;
            
            // Handle Arith-Branch Stalls
            bool arithBranch = isBranch && exPrev.doesArithLogic && exPrev.writesRd && exPrev.rd != 0 &&
                         (idPrev.rs1 == exPrev.rd || idPrev.rs2 == exPrev.rd);
            
            // Handle LoadBranch
            // NOTE: the only time a nop is inserted between a load and a branch
            // is when there is a LoadBranch Stall. True?
            bool loadBranch = isBranch && (memPrev.opcode == OP_LOAD) && exPrev.isNop;

            // Ex

            // Case: Load-Use
            if(loadUse){
                // Handle forwarding from MEM stage to the ID stage
                pipelineInfo.idInst.op1Val = (pipelineInfo.memInst.writesRd && (pipelineInfo.memInst.rd == idPrev.rs1)) 
                                                ? pipelineInfo.memInst.memResult:
                                                pipelineInfo.idInst.op1Val;
                pipelineInfo.idInst.op2Val = (pipelineInfo.memInst.writesRd && (pipelineInfo.memInst.rd == idPrev.rs2)) 
                                                ? pipelineInfo.memInst.memResult:
                                                pipelineInfo.idInst.op2Val;
                iCacheStallCycles = max(0, iCacheStallCycles - 1);
                // insert bubble
                pipelineInfo.exInst = nop(BUBBLE);
            } 

            // Case: Arith-Branch Stall
            else if(arithBranch){
                // Handle forwarding from EX stage to the ID stage
                pipelineInfo.idInst.op1Val = (exPrev.writesRd && (exPrev.rd == idPrev.rs1)) 
                                                ? exPrev.arithResult:
                                                pipelineInfo.idInst.op1Val;
                pipelineInfo.idInst.op2Val = (exPrev.writesRd && (exPrev.rd == idPrev.rs2)) 
                                                ? exPrev.arithResult:
                                                pipelineInfo.idInst.op2Val;
                iCacheStallCycles = max(0, iCacheStallCycles - 1);
                // insert bubble
                pipelineInfo.exInst = nop(BUBBLE);
                // Update PC Resolution
                pipelineInfo.idInst = simulator->simNextPCResolution(idPrev);
            }

            // Case: Load-Branch Stall
            else if(loadBranch){
                // Handle forwarding from the memory register to the ID stage
                pipelineInfo.idInst.op1Val = (pipelineInfo.wbInst.writesRd && (pipelineInfo.wbInst.rd == idPrev.rs1)) 
                                                ? pipelineInfo.mwbInst.memResult:
                                                pipelineInfo.idInst.op1Val;
                pipelineInfo.idInst.op2Val = (pipelineInfo.memInst.writesRd && (pipelineInfo.memInst.rd == idPrev.rs2)) 
                                                ? pipelineInfo.memInst.memResult:
                                                pipelineInfo.idInst.op2Val;
                iCacheStallCycles = max(0, iCacheStallCycles - 1);
                // insert bubble
                pipelineInfo.exInst = nop(BUBBLE);
                // update PC resolution if needed
                pipelineInfo.idInst = simulator->simNextPCResolution(idPrev);
            }

            // Case: No Stalls
            else {
                // normal case
                // Do FORWARDING between prevMem and Ex and/or prevEx and Ex
                idPrev.op1Val = (exPrev.writesRd && exPrev.rd == idPrev.rs1 && exPrev.doesArithLogic) ? exPrev.arithResult:
                                (memPrev.writesRd && memPrev.rd == idPrev.rs1 && memPrev.readsMem) ? memPrev.memResult:
                                (memPrev.writesRd && memPrev.rd == idPrev.rs1 && memPrev.arithResult) ? memPrev.arithResult:
                                idPrev.op1Val;

                idPrev.op2Val = (exPrev.writesRd && exPrev.rd == idPrev.rs2 && exPrev.doesArithLogic) ? exPrev.arithResult:
                                (memPrev.writesRd && memPrev.rd == idPrev.rs2 && memPrev.readsMem) ? memPrev.memResult:
                                (memPrev.writesRd && memPrev.rd == idPrev.rs2 && memPrev.arithResult) ? memPrev.arithResult:
                                idPrev.op2Val;

                pipelineInfo.exInst = simulator->simEX(idPrev);
                
                //  ID + IF

                // Handle always not-taken branch handling    
                bool idSquash = isBranch && (idPrev.nextPC != idPrev.PC + 4);

                // Case: Branch is taken
                if(idSquash){
                    loadStallCount++;
                    pipelineInfo.idInst = nop(SQUASHED);
                    /*
                    NOTE
                    I am assuming here that if there is a branch which will change the
                    PC then we are to abort the stall cycles for the 
                    */
                    if(iCacheStallCycles!=0){
                        invalidate(PC);
                        iCacheStallCycles = 0; 
                    }
                    PC = idPrev.nextPC;
                    pipelineInfo.ifInst = simulator->simIF(PC);
                    // Case: New PC misses in iCache
                    if(!access(PC, CACHE_READ)) 
                        iCacheStallCycles = iCache->config->missLatency;
                    
                }

                // Case: Branch is not taken OR no branch
                else {
                    pipelineInfo.idInst = simulator->simID(ifPrev);
                    // 0x8000 is the error handling address
                    PC = (!pipelineInfo.idInst.isLegal) ? 0x8000:
                    pipelineInfo.idInst.nextPC;
                    // Case: Illegal Instruction in ID
                    if(!pipelineInfo.idInst.isLegal){
                        pipelineInfo.idInst = nop(SQUASHED);
                        status = ERROR;
                    }
                    // Case: Still Stalling from previous iCache miss
                    else if (iCacheStallCycles > 0) {
                        pipelineInfo.idInst = nop(BUBBLE);
                        iCacheStallCycles--;
                    }
                    // Case: iCache miss
                    else if(!iCache->access(PC, CACHE_READ)) {
                        pipelineInfo.idInst = nop(BUBBLE);
                        iCacheStallCycles = iCache->config->missLatency;
                    } 
                    pipelineInfo.ifInst = simulator->simIF(PC);
                }

            }
        }

        // IF check for illegal PC
        if(pipelineInfo.ifInst.PC >= MEMORY_SIZE){
            pipelineInfo.ifInst = simulator->simIF(0x8000);
            status = ERROR;
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
