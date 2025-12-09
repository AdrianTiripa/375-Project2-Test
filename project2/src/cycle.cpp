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

        // PREVIOUS STATE OF PIPELINE
        Simulator::Instruction ifPrev = pipelineInfo.ifInst;
        Simulator::Instruction idPrev = pipelineInfo.idInst;
        Simulator::Instruction exPrev = pipelineInfo.exInst;
        Simulator::Instruction memPrev = pipelineInfo.memInst;
        Simulator::Instruction wbPrev = pipelineInfo.wbInst;


        // BRANCH CATHCER
        bool idIsBranch = (idPrev.opcode == OP_BRANCH) || (idPrev.opcode == OP_JALR) ||
                          (idPrev.opcode == OP_JAL);

        // BRANCH TAKEN OR NOT
        bool taken = false;

        // HAZARD DETECTION (LOAD-USE, ARITHMETIC-BRANCH, LOAD-BRANCH)

        // Catches load use stall (vanilla) and first stall of load branch
        bool catchLoadUse = ((idPrev.rs1 == exPrev.rd) || (idPrev.rs2 == exPrev.rd))
                            && exPrev.readsMem && (exPrev.rd!=0);

        bool catchArithBranch = ((idPrev.rs1 == exPrev.rd) || (idPrev.rs2 == exPrev.rd))
                                && (exPrev.rd!=0) && idIsBranch && exPrev.doesArithLogic
                                && exPrev.writesRd;

        // Catches second cycle of load branch
        bool catchLoadBranch = idIsBranch && exPrev.isNop && memPrev.readsMem
                                && ((idPrev.rs1 == memPrev.rd) || (idPrev.rs2 == memPrev.rd))
                                && (memPrev.rd != 0);

        bool bubbleEXstallID = (catchLoadUse || catchArithBranch) || catchLoadBranch;

        // WB SEQUENCE
        // WB Check for halt instruction 
        pipelineInfo.wbInst = nop(BUBBLE);
        pipelineInfo.wbInst = simulator->simWB(memPrev);
        if (pipelineInfo.wbInst.isHalt) {
            status = HALT;
            break;
        }

        // MEM SEQUENCE
        // special forwarding load-store data dependency
        if(exPrev.writesMem && exPrev.readsRs2 && exPrev.rs2 != 0){
                if ((wbPrev.rd == exPrev.rs2) && wbPrev.readsMem)
                exPrev.op2Val = wbPrev.memResult;
        }
        pipelineInfo.memInst = simulator->simMEM(exPrev);

        // EX SEQUENCE
        if(bubbleEXstallID){
            pipelineInfo.exInst = nop(BUBBLE);
        }
        else{
            forwarding(idPrev.rs1, idPrev.readsRs1, idPrev.op1Val, exPrev, memPrev);
            forwarding(idPrev.rs2, idPrev.readsRs2, idPrev.op2Val, exPrev, memPrev);
            pipelineInfo.exInst = simulator->simEX(idPrev);
        }

        // ID SEQUENCE
        forwarding(idPrev.rs1, idPrev.readsRs1, idPrev.op1Val, exPrev, memPrev);
        forwarding(idPrev.rs2, idPrev.readsRs2, idPrev.op2Val, exPrev, memPrev);
        uint64_t nextPC = PC;
        if(bubbleEXstallID){
           pipelineInfo.idInst = idPrev;
        }
        else{
            nextPC = PC + 4;
            if(idIsBranch){
                idPrev = simulator->simNextPCResolution(idPrev);
                taken = ((idPrev.PC+4) != (idPrev.nextPC));
            }
            
            if(taken){
                pipelineInfo.idInst = nop(SQUASHED);
                nextPC = idPrev.nextPC;
            }
            else{
                pipelineInfo.idInst = simulator->simID(ifPrev);
            }

        }

        // IF SEQUENCE
        if(bubbleEXstallID){
            pipelineInfo.ifInst = ifPrev; 
        }
        else{
            pipelineInfo.ifInst = simulator->simIF(PC);
        }

        // UPDATE STATUS FOR IF
        if(idIsBranch){
            pipelineInfo.ifInst.status = SPECULATIVE;
        }

        // MOVE ON
        PC = nextPC;
    }

    
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