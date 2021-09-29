#include "llvm/Transforms/Utils/DMS_common.h"

using namespace llvm;

/// The given `Builder` will now be ready to insert instructions _after_ the
/// given `inst`
void setInsertPointToAfterInst(IRBuilder<>& Builder, Instruction* inst) {
  Builder.SetInsertPoint(inst);
  BasicBlock* bb = Builder.GetInsertBlock();
  auto ip = Builder.GetInsertPoint();
  ip++;
  Builder.SetInsertPoint(bb, ip);
}
