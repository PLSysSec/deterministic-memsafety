#include "llvm/Transforms/Utils/DMS_common.h"
#include "llvm/Transforms/Utils/DMS_IRBuilder.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"

using namespace llvm;

extern const APInt zero;

/// If computing the allocation size requires inserting dynamic instructions,
/// use `Builder`
IsAllocatingCall isAllocatingCall(const CallBase &call, DMSIRBuilder& Builder) {
  Function* callee = call.getCalledFunction();
  if (!callee) {
    // we assume indirect calls aren't allocating
    return IsAllocatingCall::not_allocating();
  }
  if (!callee->hasName()) {
    // we assume anonymous functions aren't allocating
    return IsAllocatingCall::not_allocating();
  }
  StringRef name = callee->getName();
  if (name == "malloc") {
    return IsAllocatingCall::allocating(call.getArgOperand(0));
  } else if (name == "realloc") {
    return IsAllocatingCall::allocating(call.getArgOperand(1));
  } else if (name == "calloc") {
    return IsAllocatingCall::allocating(Builder.CreateMul(
      call.getArgOperand(0),
      call.getArgOperand(1)
    ));
  } else if (name == "aligned_alloc") {
    return IsAllocatingCall::allocating(call.getArgOperand(1));
  }
  return IsAllocatingCall::not_allocating();
}

/// Determine whether the GEP's total offset is a compile-time constant, and if
/// so, what constant
GEPConstantOffset computeGEPOffset(const llvm::GetElementPtrInst& gep, const DataLayout& DL) {
  GEPConstantOffset gco;
  gco.offset = zero;
  gco.is_constant = gep.accumulateConstantOffset(DL, gco.offset);
  return gco;
}
