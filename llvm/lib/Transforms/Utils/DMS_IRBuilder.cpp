#include "llvm/Transforms/Utils/DMS_IRBuilder.h"

using namespace llvm;

/// Make this `Builder` ready to insert instructions _after_ the given `inst`
void DMSIRBuilder::SetInsertPointToAfterInst(Instruction* inst) {
	SetInsertPoint(inst);
	BasicBlock* bb = GetInsertBlock();
	auto ip = GetInsertPoint();
	ip++;
	SetInsertPoint(bb, ip);
}
