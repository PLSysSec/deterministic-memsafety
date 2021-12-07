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
GEPConstantOffset computeGEPOffset(const GetElementPtrInst& gep, const DataLayout& DL) {
  GEPConstantOffset gco;
  gco.offset = zero;
  gco.is_constant = gep.accumulateConstantOffset(DL, gco.offset);
  return gco;
}

/// Returns `true` if the block is well-formed. For this function's purposes,
/// "well-formed" means:
///   - the block has exactly one terminator instruction
///   - the terminator instruction is at the end
///   - all PHI instructions and/or landingpad instructions (if they exist) come first
bool wellFormed(const BasicBlock& bb) {
  const Instruction* terminator = bb.getTerminator();
  if (!terminator) return false;
  for (const Instruction& I : bb) {
    if (I.isTerminator() && &I != terminator) return false;
  }
  bool have_non_phi_or_landingpad = false;
  for (const Instruction& I : bb) {
    if (isa<PHINode>(I) || isa<LandingPadInst>(I)) {
      if (have_non_phi_or_landingpad) return false;
    } else {
      have_non_phi_or_landingpad = true;
    }
  }
  return true;
}

static void verifyGVUserIsWellFormed(const User* user) {
  if (auto* I = dyn_cast<Instruction>(user)) {
    if (!I->getParent()) {
      errs() << "The following instruction doesn't have a parent:\n";
      I->dump();
      llvm_unreachable("verifyGVUserIsWellFormed failed");
    }
  } else if (auto* C = dyn_cast<Constant>(user)) {
    for (const User* C_user : C->users()) {
      verifyGVUserIsWellFormed(C_user);
    }
  }
}

/// This function iterates using the same pattern in `GlobalDCEPass`,
/// to ensure that no GV users are Instructions without a Block parent.
/// (If there were, `GlobalDCEPass` will crash later.)
void verifyGVUsersAreWellFormed(const Function& F) {
  const Module* mod = F.getParent();
  for (const GlobalValue& gv : mod->global_values()) {
    for (const User* user : gv.users()) {
      verifyGVUserIsWellFormed(user);
    }
  }
}

/// Mangled name of the get_bounds function
static const char* get_bounds_func = "_ZN5__dms16__dms_get_boundsEPv";
/// Mangled name of the store_bounds function
static const char* store_bounds_func = "_ZN5__dms18__dms_store_boundsEPvS0_S0_";
/// Mangled name of the store_infinite_bounds function
static const char* store_bounds_inf_func = "_ZN5__dms27__dms_store_infinite_boundsEPv";
/// Mangled name of the boundscheckfail function
static const char* boundscheckfail_func = "_ZN5__dms21__dms_boundscheckfailEPv";

/// Convenience function to create calls to our runtime support function
/// `__dms_store_bounds()`.
///
/// The arguments `ptr`, `base`, and `max` can be any pointer type (not
/// necessarily `void*`). They should be UNENCODED values, ie with all upper
/// bits clear.
CallInst* call_dms_store_bounds(Value* ptr, Value* base, Value* max, DMSIRBuilder& Builder) {
  Module* mod = Builder.GetInsertBlock()->getModule();
  static Type* CharStarTy = Builder.getInt8PtrTy();
  static FunctionType* StoreBoundsTy = FunctionType::get(Builder.getVoidTy(), {CharStarTy, CharStarTy, CharStarTy}, /* IsVarArgs = */ false);
  FunctionCallee StoreBounds = mod->getOrInsertFunction(store_bounds_func, StoreBoundsTy);
  return Builder.CreateCall(StoreBounds, {Builder.castToCharStar(ptr), Builder.castToCharStar(base), Builder.castToCharStar(max)});
}

/// Convenience function to create calls to our runtime support function
/// `__dms_store_infinite_bounds()`.
///
/// The `ptr` argument can be any pointer type (not necessarily `void*`),
/// and should be an UNENCODED value, ie with all upper bits clear.
CallInst* call_dms_store_infinite_bounds(Value* ptr, DMSIRBuilder& Builder) {
  Module* mod = Builder.GetInsertBlock()->getModule();
  static Type* CharStarTy = Builder.getInt8PtrTy();
  static FunctionType* StoreBoundsInfTy = FunctionType::get(Builder.getVoidTy(), {CharStarTy}, /* IsVarArgs = */ false);
  FunctionCallee StoreBoundsInf = mod->getOrInsertFunction(store_bounds_inf_func, StoreBoundsInfTy);
  return Builder.CreateCall(StoreBoundsInf, {Builder.castToCharStar(ptr)});
}

/// Convenience function to create calls to our runtime support function
/// `__dms_get_bounds()`.
///
/// The `ptr` argument can be any pointer type (not necessarily `void*`),
/// and should be an UNENCODED value, ie with all upper bits clear.
CallInst* call_dms_get_bounds(Value* ptr, DMSIRBuilder& Builder) {
  Module* mod = Builder.GetInsertBlock()->getModule();
  static Type* CharStarTy = Builder.getInt8PtrTy();
  static Type* Int64Ty = Builder.getInt64Ty();
  static Type* GetBoundsRetTy = StructType::get(mod->getContext(), {CharStarTy, CharStarTy, Int64Ty});
  FunctionType* GetBoundsTy = FunctionType::get(GetBoundsRetTy, CharStarTy, /* IsVarArgs = */ false);
  FunctionCallee GetBounds = mod->getOrInsertFunction(get_bounds_func, GetBoundsTy);
  return Builder.CreateCall(GetBounds, {Builder.castToCharStar(ptr)});
}

/// Convenience function to create calls to our runtime support function
/// `__dms_boundscheckfail()`.
///
/// The `ptr` argument can be any pointer type (not necessarily `void*`),
/// and should be an UNENCODED value, ie with all upper bits clear.
CallInst* call_dms_boundscheckfail(Value* ptr, DMSIRBuilder& Builder) {
  Module* mod = Builder.GetInsertBlock()->getModule();
  static Type* CharStarTy = Builder.getInt8PtrTy();
  FunctionType* BoundsCheckFailTy = FunctionType::get(Builder.getVoidTy(), {CharStarTy}, /* IsVarArgs = */ false);
  FunctionCallee BoundsCheckFail = mod->getOrInsertFunction(boundscheckfail_func, BoundsCheckFailTy);
  return Builder.CreateCall(BoundsCheckFail, {Builder.castToCharStar(ptr)});
}
