#include "RISCV.h"
#include "RISCVSubtarget.h"
#include "RISCVXhwachaUtilities.h"
#include "llvm/Analysis/Scalarization.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/ScalarEvolutionExpander.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/Pass.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineScalarization.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/PassSupport.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Target/TargetInstrInfo.h"
#include <algorithm>
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "vfopt"

namespace llvm {
  ModulePass *createRISCVVectorFetchIROpt();
  void initializeRISCVVectorFetchIROptPass(PassRegistry&);
  MachineFunctionPass *createRISCVVectorFetchMachOpt();
  void initializeRISCVVectorFetchMachOptPass(PassRegistry&);
}

namespace {
  struct RISCVVectorFetchIROpt : public ModulePass {
    Scalarization       *MS;
    ScalarEvolution     *SE;
    std::map<Instruction *, const SCEV *> addrs;
  public:
    static char ID;

    explicit RISCVVectorFetchIROpt() : ModulePass(ID) {
      initializeRISCVVectorFetchIROptPass(*PassRegistry::getPassRegistry());
    }

    bool runOnSCC(CallGraphSCC &SCC);
    bool runOnModule(Module &M) override;

    const char *getPassName() const override { return "RISCV Vector Fetch IROpt"; }

    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.addRequired<CallGraphWrapperPass>();
      AU.addRequired<Scalarization>();
      AU.addRequired<ScalarEvolution>();
      ModulePass::getAnalysisUsage(AU);
    }
    virtual CallGraphNode *processOpenCLKernel(Function *F);
    virtual CallGraphNode *VectorFetchOpt(CallGraphNode *CGN);
    virtual const SCEV *attemptToHoistOffset(const SCEV *&expr, const SCEV *&parent,
        bool *found, unsigned bytewidth, const SCEV **veidx);
  };

}

namespace {
  struct RISCVVectorFetchMachOpt : public MachineFunctionPass {
    MachineScalarization       *MS;
    const RISCVInstrInfo     *TII;
  public:
    static char ID;

    RISCVVectorFetchMachOpt() : MachineFunctionPass(ID) {
      initializeRISCVVectorFetchMachOptPass(*PassRegistry::getPassRegistry());
    }

    bool runOnMachineFunction(MachineFunction &MF) override;

    const char *getPassName() const override { return "RISCV Vector Fetch MachOpt"; }

    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.addRequired<MachineScalarization>();
      MachineFunctionPass::getAnalysisUsage(AU);
    }
    virtual void processOpenCLKernel(MachineFunction &MF);
  };

  char RISCVVectorFetchMachOpt::ID = 0;

} // end anonymous namespace

char RISCVVectorFetchIROpt::ID = 0;
INITIALIZE_PASS_BEGIN(RISCVVectorFetchIROpt, "vfiropt",
                      "RISCV Vector Fetch IROpt", false, false)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(Scalarization)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolution)
INITIALIZE_PASS_END(RISCVVectorFetchIROpt, "vfiropt",
                    "RISCV Vector Fetch IROpt", false, false)

ModulePass *llvm::createRISCVVectorFetchIROpt() {
  return new RISCVVectorFetchIROpt();
}
bool RISCVVectorFetchIROpt::runOnModule(Module &M) {
    CallGraph& cg = getAnalysis<CallGraphWrapperPass>().getCallGraph();

    scc_iterator<CallGraph*> cgSccIter = scc_begin(&cg);
    CallGraphSCC curSCC(&cgSccIter);
    while (!cgSccIter.isAtEnd())
    {
        const std::vector<CallGraphNode*>& nodeVec = *cgSccIter;
        curSCC.initialize(nodeVec.data(), nodeVec.data() + nodeVec.size());
        runOnSCC(curSCC);
        ++cgSccIter;
    }

    return false;
}
bool RISCVVectorFetchIROpt::runOnSCC(CallGraphSCC &SCC) {
  bool Changed = false;

  // Attempt to promote arguments from all functions in this SCC.
  for (CallGraphSCC::iterator I = SCC.begin(), E = SCC.end(); I != E; ++I) {
    if (CallGraphNode *CGN = VectorFetchOpt(*I)) {
      Changed = true;
      SCC.ReplaceNode(*I, CGN);
    }
  }
  return Changed;
}

CallGraphNode *RISCVVectorFetchIROpt::VectorFetchOpt(CallGraphNode *CGN) {
  Function *F = CGN->getFunction();

  if(!F) return nullptr;
  if(F->isIntrinsic()) return nullptr;
  MS = &getAnalysis<Scalarization>(*F);
  SE = &getAnalysis<ScalarEvolution>(*F);

  if(isOpenCLKernelFunction(*F)) {
    if (CallGraphNode *CGN = processOpenCLKernel(F)) {
      return CGN;
    }
    return nullptr;
  }
  return nullptr;
}

const SCEV *RISCVVectorFetchIROpt::attemptToHoistOffset(const SCEV *&expr, const SCEV *&parent, bool *found, unsigned bytewidth, const SCEV **veidx) {
  //psuedo code
  // recursively descend looking for eidx
  // once found, while we are coming up the tree
  // + ignore the + nodes since hoisting veidx above them doesn't matter
  // + record the other input to the first * we encounter
  // + any more than one * and we fail
  // + any less than one * and we fail
  // + if the recorded input is not equal to the bytewidth we fail
  // Once we arrive at the root of the tree which needs to be an add node
  // + append the mul of eidx and recorded input as another input
  if(const SCEVAddExpr *add = dyn_cast<SCEVAddExpr>(expr)) {
    bool lfound = *found;
    SmallVector<const SCEV *, 8> newops;
    for( const SCEV *op : add->operands() ) {
      *found = false;
      const SCEV *subexp = attemptToHoistOffset(op, expr, found, bytewidth, veidx);
      if(subexp == SE->getCouldNotCompute()) return subexp;
      if(lfound && *found) {
        printf("two uses of veidx: can't hoist\n");
        return SE->getCouldNotCompute();
      }
      newops.push_back(subexp);
      if(*found) {
        if(parent == expr) { // root node
          // add the veidx and the op
          const SCEV *eidxExpr = SE->getMulExpr(*veidx, 
              SE->getConstant(expr->getType(), bytewidth));
          //newops.push_back(eidxExpr);
        }
      }
      lfound = *found;
    }
    return SE->getAddExpr(newops);
  } else if(const SCEVMulExpr *mul = dyn_cast<SCEVMulExpr>(expr)) {
    bool lfound = *found;
    SmallVector<const SCEV *, 8> newops;
    for( const SCEV *op : mul->operands() ) {
      *found = false;
      const SCEV *subexp = attemptToHoistOffset(op, expr, found, bytewidth, veidx);
      if(subexp == SE->getCouldNotCompute()) return subexp;
      if(lfound && found) {
        printf("two uses of veidx: can't hoist\n");
        return SE->getCouldNotCompute();
      }
      newops.push_back(subexp);
      if(*found) {
        if(parent == expr) { // root node
          printf("require a non-zero base: can't hoist\n");
          return SE->getCouldNotCompute(); // root node can't be *
        }
        // check constant 
        if(const SCEVConstant *num = dyn_cast<SCEVConstant>(mul->getOperand(0))){
          if(num != SE->getConstant(num->getType(), bytewidth)) {
            printf("require bytewidth multipler on eidx: can't hoist\n");
            return SE->getCouldNotCompute();
          }
        } else {
          printf("require constant as bytewidth: can't hoist\n");
          return SE->getCouldNotCompute();
        }
      }
      lfound = *found;
    }
    return SE->getMulExpr(newops);
  } else if(const SCEVUnknown *eidx = dyn_cast<SCEVUnknown>(expr)) {
    // Note that we found it
    if(isa<IntrinsicInst>(eidx->getValue())) {
      if(cast<IntrinsicInst>(eidx->getValue())->getIntrinsicID() == Intrinsic::hwacha_veidx) {
        *found = true;
        *veidx = eidx;
        // Replace  with identity constant based on parent
        if(parent->getSCEVType() == scAddExpr) {
          return SE->getConstant(expr->getType(),0);
        } else if(parent->getSCEVType() == scMulExpr) {
          return SE->getConstant(expr->getType(),1);
        }
      }
    }
    return expr; // just some random value TODO: maybe check that its an argument?
  } else if(isa<SCEVConstant>(expr)) {
    return expr;
  } else
    return SE->getCouldNotCompute();
}

CallGraphNode *RISCVVectorFetchIROpt::processOpenCLKernel(Function *F) {
  for(Function::iterator BBI = F->begin(), MBBE = F->end(); BBI != MBBE; ++BBI) {
    for(BasicBlock::iterator MII = BBI->begin(), MIE = BBI->end(); MII != MIE; ++MII) {
      if(isa<StoreInst>(MII)) {
        const SCEV *store = SE->getSCEV(cast<StoreInst>(MII)->getPointerOperand());
        printf("found store inst in opencl kernel, trying to hoist\n");
        MII->dump();
        store->dump();
        fflush(stdout);fflush(stderr);
        const SCEV *ptrBase = SE->getPointerBase(store);
        // We need a base addr to start with
        if(!isa<SCEVUnknown>(ptrBase)) {
          break;
        }
        // Descend through NAry ops building up a global addexpr
        // Goal is something like AddExpr(base, offset, MulExpr(eidx,bytewidth))
        // where offset is another potentially deep scev tree as long as it doesn't
        // have base or eidx
        bool found = false;
        const SCEV *veidx;
        const SCEV *newSCEV = attemptToHoistOffset(store, store, &found, MII->getOperand(0)->getType()->getPrimitiveSizeInBits()/8, &veidx);
        if(newSCEV != SE->getCouldNotCompute()){
          // TODO: setup data structure so caller can promote value to va reg
          addrs.insert(std::make_pair(MII, newSCEV));
        }
      }
      if(isa<LoadInst>(MII)) {
        const SCEV *load = SE->getSCEV(cast<LoadInst>(MII)->getPointerOperand());
        printf("found load inst in opencl kernel, trying to hoist\n");
        MII->dump();
        load->dump();
        fflush(stdout);fflush(stderr);
        const SCEV *ptrBase = SE->getPointerBase(load);
        // We need a base addr to start with
        if(!isa<SCEVUnknown>(ptrBase)) {
          break;
        }
        // Descend through NAry ops building up a global addexpr
        // Goal is something like AddExpr(base, offset, MulExpr(eidx,bytewidth))
        // where offset is another potentially deep scev tree as long as it doesn't
        // have base or eidx
        bool found = false;
        const SCEV *veidx;
        const SCEV *newSCEV = attemptToHoistOffset(load, load, &found, MII->getType()->getPrimitiveSizeInBits()/8, &veidx);
        if(newSCEV != SE->getCouldNotCompute()){
          // TODO: setup data structure so caller can promote value to va reg
          addrs.insert(std::make_pair(MII, newSCEV));
        }
      }
    }
  }
  // Update function type based on new arguments
  FunctionType *FTy = F->getFunctionType();
  std::vector<Type*> Params;
  Params.insert(Params.end(), FTy->param_begin(), FTy->param_end());
  for(std::map<Instruction *, const SCEV *>::iterator it = addrs.begin(); it != addrs.end(); ++it) {
    if(isa<LoadInst>(it->first))
      Params.push_back(it->first->getType()->getPointerTo());
    if(isa<StoreInst>(it->first))
      Params.push_back(it->first->getOperand(0)->getType()->getPointerTo());
  }
  Type *RetTy = FTy->getReturnType();

  // Create new function with additional args to replace old one
  FunctionType *NFTy = FunctionType::get(RetTy, Params, false);
  Function *NF = Function::Create(NFTy, F->getLinkage(), F->getName());
  NF->copyAttributesFrom(F);
  for(unsigned i = 1; i <= addrs.size(); i++) {
    NF->addAttribute(FTy->getNumParams()+i, Attribute::ByVal);
  }

  F->getParent()->getFunctionList().insert(F, NF);
  NF->takeName(F);
  // Get the callgraph information that we need to update to reflect our
  // changes.
  CallGraph &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();

  // Get a new callgraph node for NF.
  CallGraphNode *NF_CGN = CG.getOrInsertFunction(NF);

  // Loop over all of the callers of the function, transforming the call sites
  // to pass in the loaded pointers.
  //
  SmallVector<Value*, 16> Args;
  SmallVector<AttributeSet, 8> AttributesVec;
  while (!F->use_empty()) {
    CallSite CS(F->user_back());
    assert(CS.getCalledFunction() == F);
    Instruction *Call = CS.getInstruction();
    const AttributeSet &CallPAL = CS.getAttributes();

    // Add any return attributes.
    if (CallPAL.hasAttributes(AttributeSet::ReturnIndex))
      AttributesVec.push_back(AttributeSet::get(F->getContext(),
                                                CallPAL.getRetAttributes()));

    // Create callee.args => callsite.args map for parameter rewriter
    ValueToValueMap argMap;
    // Loop over the operands, inserting GEP and loads in the caller as
    // appropriate.
    CallSite::arg_iterator AI = CS.arg_begin();
    unsigned ArgIndex = 1;
    for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end();
         I != E; ++I, ++AI, ++ArgIndex) {
      if(ArgIndex <= FTy->getNumParams()) {
        Args.push_back(*AI); // Old unchanged args
        argMap[I] = *AI;
      }
    }
    AttributeSet aregAttrs();
    // Create code to generate SCEV in map
    SCEVExpander Expander(*SE, F->getParent()->getDataLayout(), "vfoptexp");
    for(std::map<Instruction *, const SCEV *>::iterator it = addrs.begin(); it != addrs.end(); ++it) {
      const SCEV *newSCEV = SCEVParameterRewriter::rewrite(it->second, *SE, argMap);
      Value *base;
      if(isa<LoadInst>(it->first))
        base = Expander.expandCodeFor(newSCEV, it->first->getType()->getPointerTo(),Call);
      if(isa<StoreInst>(it->first))
        base = Expander.expandCodeFor(newSCEV, it->first->getOperand(0)->getType()->getPointerTo(),Call);
      Args.push_back(base);
    }

    Instruction *New = CallInst::Create(NF, Args, "", Call);
    cast<CallInst>(New)->setCallingConv(CS.getCallingConv());
    cast<CallInst>(New)->setAttributes(AttributeSet::get(New->getContext(),
                                                        AttributesVec));
    for(unsigned i = 0;i < addrs.size(); i++) {
      cast<CallInst>(New)->addAttribute(ArgIndex+i, Attribute::ByVal);
    }
    if (cast<CallInst>(Call)->isTailCall())
      cast<CallInst>(New)->setTailCall();

    New->setDebugLoc(Call->getDebugLoc());
    Args.clear();
    AttributesVec.clear();
    // Update the callgraph to know that the callsite has been transformed.
    CallGraphNode *CalleeNode = CG[Call->getParent()->getParent()];
    CalleeNode->replaceCallEdge(CS, CallSite(New), NF_CGN);

    //migrate all named metadata
    llvm::NamedMDNode *nmd = F->getParent()->getNamedMetadata("opencl.kernels");
    for (unsigned i = 0, e = nmd->getNumOperands(); i != e; ++i) {
      MDNode *kernel_iter = nmd->getOperand(i);
      Function *k =
        cast<Function>(
          dyn_cast<ValueAsMetadata>(kernel_iter->getOperand(0))->getValue());
      if (k->getName() == F->getName()) {
        kernel_iter->replaceOperandWith(0, llvm::ValueAsMetadata::get(NF));
      }
    }

    if (!Call->use_empty()) {
      Call->replaceAllUsesWith(New);
      New->takeName(Call);
    }

    // Finally, remove the old call from the program, reducing the use-count of
    // F.
    Call->eraseFromParent();
  }
  // Since we have now created the new function, splice the body of the old
  // function right into the new function, leaving the old rotting hulk of the
  // function empty.
  NF->getBasicBlockList().splice(NF->begin(), F->getBasicBlockList());

  // Loop over the argument list, transferring uses of the old arguments over to
  // the new arguments, also transferring over the names as well.
  Function::arg_iterator I2 = NF->arg_begin();
  for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end(); I != E; ++I) {
    // This is an unmodified argument, move the name and users over to the
    // new version.
    I->replaceAllUsesWith(I2);
    I2->takeName(I);
    ++I2;
    continue;
  }
  // Loop over the remaining args createing new loads to use the
  for(std::map<Instruction *, const SCEV *>::iterator it = addrs.begin(); it != addrs.end(); ++it) {

    Instruction *newMemOp;
    if(isa<LoadInst>(it->first)) {
      newMemOp = new LoadInst(I2, "vec_addr_base", it->first);
    }
    if(isa<StoreInst>(it->first)) {
      newMemOp = new StoreInst(it->first->getOperand(0), I2, "vec_addr_base", it->first);
    }
    it->first->replaceAllUsesWith(newMemOp);
    newMemOp->takeName(it->first);
    it->first->eraseFromParent();
    ++I2;
  }
  // iterate of instruction in addr doing a few things
  // 1) add another argument for the address to be passed
  // 2) replace the memop with one that uses this new argument
  // 3) TODO: figure out how to ensure the new args are in va regs

  NF_CGN->stealCalledFunctionsFrom(CG[F]);
  
  // Now that the old function is dead, delete it.  If there is a dangling
  // reference to the CallgraphNode, just leave the dead function around for
  // someone else to nuke.
  CallGraphNode *CGN = CG[F];
  if (CGN->getNumReferences() == 0)
    delete CG.removeFunctionFromModule(CGN);
  else
    F->setLinkage(Function::ExternalLinkage);

  return NF_CGN;
}

INITIALIZE_PASS_BEGIN(RISCVVectorFetchMachOpt, "vfmachopt",
                      "RISCV Vector Fetch MachOpt", false, false)
INITIALIZE_PASS_DEPENDENCY(MachineScalarization)
INITIALIZE_PASS_END(RISCVVectorFetchMachOpt, "vfmachopt",
                    "RISCV Vector Fetch MachOpt", false, false)

MachineFunctionPass *llvm::createRISCVVectorFetchMachOpt() {
  return new RISCVVectorFetchMachOpt();
}

bool RISCVVectorFetchMachOpt::runOnMachineFunction(MachineFunction &MF) {
  bool Changed = false;

  MS = &getAnalysis<MachineScalarization>();
  TII = MF.getSubtarget<RISCVSubtarget>().getInstrInfo();

  if(isOpenCLKernelFunction(*(MF.getFunction())))
    processOpenCLKernel(MF);
  
  return Changed;
}

void RISCVVectorFetchMachOpt::processOpenCLKernel(MachineFunction &MF) {
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
  const TargetRegisterInfo &TRI = *MF.getSubtarget().getRegisterInfo();

  for (MachineFunction::iterator MFI = MF.begin(), MFE = MF.end(); MFI != MFE;
       ++MFI) {
    // In each BB change each instruction
    for (MachineBasicBlock::iterator I = MFI->begin(); I != MFI->end(); ++I) {
      printf("Inst:");I->dump();
      printf("invar?%d\n",MS->invar[I]);fflush(stdout);fflush(stderr);
      // All inputs are vs registers and outputs are vv registers
      switch(I->getOpcode()) {
        case TargetOpcode::COPY :
          // If this is physical to virt copy do nothing
          if(TRI.isPhysicalRegister(I->getOperand(1).getReg())) {
            if(RISCV::VARBitRegClass.contains(I->getOperand(1).getReg()))
              MRI.setRegClass(I->getOperand(0).getReg(), &RISCV::VARBitRegClass);
            break;
          }
          MRI.setRegClass(I->getOperand(0).getReg(),
            MRI.getRegClass(I->getOperand(1).getReg()));
          break;
        case RISCV::ADD64 :
          //FIXME: if we can have phys regs here check for that first
          if(MS->invar[I]) {
            I->setDesc(TII.get(RISCV::VADD_SSS));
            MRI.setRegClass(I->getOperand(0).getReg(), &RISCV::VSRBitRegClass);
          } else {
            if(MRI.getRegClass(I->getOperand(1).getReg()) == &RISCV::VSRBitRegClass) {
              if(MRI.getRegClass(I->getOperand(2).getReg()) == &RISCV::VSRBitRegClass) {
                I->setDesc(TII.get(RISCV::VADD_VSS));
              } else {
                I->setDesc(TII.get(RISCV::VADD_VSV));
              }
            } else {
              if(MRI.getRegClass(I->getOperand(2).getReg()) == &RISCV::VSRBitRegClass) {
                I->setDesc(TII.get(RISCV::VADD_VVS));
              } else {
                I->setDesc(TII.get(RISCV::VADD_VVV));
              }
            }
            // Destination is always vector
            MRI.setRegClass(I->getOperand(0).getReg(), &RISCV::VVRBitRegClass);
          }
          break;
        case RISCV::SLLI64 :
        {
          if(MS->invar[I]) {
            // Generate one instruction
            // vslli vsdest, vssrc, imm
            I->setDesc(TII.get(RISCV::VSLLI));
            MRI.setRegClass(I->getOperand(0).getReg(), &RISCV::VSRBitRegClass);
          } else {
            // Generate two instructions
            // 1. vaddi vstemp, vs0, imm
            unsigned vstemp = MRI.createVirtualRegister(&RISCV::VSRBitRegClass);
            MachineOperand &imm = I->getOperand(2);
            BuildMI(*MFI, I, I->getDebugLoc(), TII.get(RISCV::VADDI), vstemp).addReg(RISCV::vs0).addImm(imm.getImm());
            // 1. vsll vvdest, vssrc, vstemp
            I->setDesc(TII.get(RISCV::VSLL_VSS));
            // Destination is always vector
            MRI.setRegClass(I->getOperand(0).getReg(), &RISCV::VVRBitRegClass);
            I->getOperand(2).ChangeToRegister(vstemp, false);
          }
          break;
        }
        case RISCV::FLW64 :
          //TODO: support invariant memops becoming scalar memops
          if(MRI.getRegClass(I->getOperand(1).getReg()) == &RISCV::VARBitRegClass) {
            I->setDesc(TII.get(RISCV::VLW_F));
            MRI.setRegClass(I->getOperand(0).getReg(), &RISCV::VVRBitRegClass);
          } else {
            I->setDesc(TII.get(RISCV::VLXW_F));
            // Destination is always vector
            MRI.setRegClass(I->getOperand(0).getReg(), &RISCV::VVRBitRegClass);
            // Shift vector portion to second src
            I->getOperand(2).ChangeToRegister(I->getOperand(1).getReg(), false);
            I->getOperand(1).ChangeToRegister(RISCV::vs0, false);
          }
          break;
        case RISCV::LW64 :
        //TODO: support invariant memops becoming scalar memops
          I->setDesc(TII.get(RISCV::VLXW));
          // Destination is always vector
          MRI.setRegClass(I->getOperand(0).getReg(), &RISCV::VVRBitRegClass);
          // Shift vector portion to second src
          I->getOperand(2).ChangeToRegister(I->getOperand(1).getReg(), false);
          I->getOperand(1).ChangeToRegister(RISCV::vs0, false);
          break;
        case RISCV::FSW64 :
          //TODO: support invariant memops becoming scalar memops
          if(MRI.getRegClass(I->getOperand(1).getReg()) == &RISCV::VARBitRegClass) {
            I->setDesc(TII.get(RISCV::VSW_F));
            I->RemoveOperand(2);
          } else {
            I->setDesc(TII.get(RISCV::VSXW_F));
            // Shift vector portion to second src
            I->getOperand(2).ChangeToRegister(I->getOperand(1).getReg(), false);
            I->getOperand(1).ChangeToRegister(RISCV::vs0, false);
          }
          break;
        case RISCV::SW64 :
        //TODO: support invariant memops becoming scalar memops
          I->setDesc(TII.get(RISCV::VSXW));
          // Shift vector portion to second src
          I->getOperand(2).ChangeToRegister(I->getOperand(1).getReg(), false);
          I->getOperand(1).ChangeToRegister(RISCV::vs0, false);
          break;
        case RISCV::FADD_S_RDY :
          {
          const TargetRegisterClass *destClass = &RISCV::VVRBitRegClass;
          if(MS->invar[I]) {
            //if we were invariant but have a vector src it means there was a vector load
            if(MRI.getRegClass(I->getOperand(1).getReg()) == &RISCV::VVRBitRegClass ||
               MRI.getRegClass(I->getOperand(2).getReg()) == &RISCV::VVRBitRegClass) {
              destClass = &RISCV::VVRBitRegClass;
            } else 
              destClass = &RISCV::VSRBitRegClass;
          } 
          if(destClass == &RISCV::VVRBitRegClass) {
            if(MRI.getRegClass(I->getOperand(1).getReg()) == &RISCV::VSRBitRegClass) {
              if(MRI.getRegClass(I->getOperand(2).getReg()) == &RISCV::VSRBitRegClass) {
                I->setDesc(TII.get(RISCV::VFADD_S_RDY_VSS));
              } else {
                I->setDesc(TII.get(RISCV::VFADD_S_RDY_VSV));
              }
            } else {
              if(MRI.getRegClass(I->getOperand(2).getReg()) == &RISCV::VSRBitRegClass) {
                I->setDesc(TII.get(RISCV::VFADD_S_RDY_VVS));
              } else {
                I->setDesc(TII.get(RISCV::VFADD_S_RDY_VVV));
              }
            }
            // Destination is always vector
          } else {
            I->setDesc(TII.get(RISCV::VFADD_S_RDY_SSS));
          }
          MRI.setRegClass(I->getOperand(0).getReg(), destClass);
          break;
          }
        case RISCV::FMUL_S_RDY :
          {
          const TargetRegisterClass *destClass = &RISCV::VVRBitRegClass;
          if(MS->invar[I]) {
            //if we were invariant but have a vector src it means there was a vector load
            if(MRI.getRegClass(I->getOperand(1).getReg()) == &RISCV::VVRBitRegClass ||
               MRI.getRegClass(I->getOperand(2).getReg()) == &RISCV::VVRBitRegClass) {
              destClass = &RISCV::VVRBitRegClass;
            } else 
              destClass = &RISCV::VSRBitRegClass;
          } 
          if(destClass == &RISCV::VVRBitRegClass) {
            if(MRI.getRegClass(I->getOperand(1).getReg()) == &RISCV::VSRBitRegClass) {
              if(MRI.getRegClass(I->getOperand(2).getReg()) == &RISCV::VSRBitRegClass) {
                I->setDesc(TII.get(RISCV::VFMUL_S_RDY_VSS));
              } else {
                I->setDesc(TII.get(RISCV::VFMUL_S_RDY_VSV));
              }
            } else {
              if(MRI.getRegClass(I->getOperand(2).getReg()) == &RISCV::VSRBitRegClass) {
                I->setDesc(TII.get(RISCV::VFMUL_S_RDY_VVS));
              } else {
                I->setDesc(TII.get(RISCV::VFMUL_S_RDY_VVV));
              }
            }
            // Destination is always vector
          } else {
            I->setDesc(TII.get(RISCV::VFMUL_S_RDY_SSS));
          }
          MRI.setRegClass(I->getOperand(0).getReg(), destClass);
          break;
          }
        case RISCV::RET :
          I->setDesc(TII.get(RISCV::VSTOP));
          I->RemoveOperand(1);
          I->RemoveOperand(0);
          break;
        default:
          printf("Unable to handle Opcode:%u in OpenCL kernel\n", I->getOpcode());
          I->dump();
      }
    }
  }
}
