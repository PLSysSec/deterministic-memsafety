#include "llvm/Transforms/Utils/DMS_IRBuilder.h"
#include "llvm/Transforms/Utils/DMS_RuntimeStackSlots.h"

using namespace llvm;

/// Get the output_base, initializing it first if necessary.
///
/// This never returns NULL.
AllocaInst* RuntimeStackSlots::getOutputBase() {
  if (output_base) return output_base;
  DMSIRBuilder Builder(&F.getEntryBlock(), DMSIRBuilder::BEGINNING, &added_insts);
  Type* CharStarTy = Builder.getInt8PtrTy();
  output_base = Builder.CreateAlloca(CharStarTy, nullptr, "__dms_output_base");
  return output_base;
}

/// Get the output_max, initializing it first if necessary.
///
/// This never returns NULL.
AllocaInst* RuntimeStackSlots::getOutputMax() {
  if (output_max) return output_max;
  DMSIRBuilder Builder(&F.getEntryBlock(), DMSIRBuilder::BEGINNING, &added_insts);
  Type* CharStarTy = Builder.getInt8PtrTy();
  output_max = Builder.CreateAlloca(CharStarTy, nullptr, "__dms_output_max");
  return output_max;
}
