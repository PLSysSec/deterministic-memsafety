#include "llvm/Transforms/Utils/DMS_common.h"
#include "llvm/Transforms/Utils/DMS_IRBuilder.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"

using namespace llvm;

extern const APInt zero;

CallNameInfo getCallNameInfo(const CallBase& call) {
  Function* callee = call.getCalledFunction();
  if (!callee) {
    return CallNameInfo(CallNameInfo::INDIRECTCALL);
  } else if (!callee->hasName()) {
    return CallNameInfo(CallNameInfo::ANONCALL);
  } else {
    return CallNameInfo(CallNameInfo::NAMEDCALL, callee->getName());
  }
}

/// If computing the allocation size requires inserting dynamic instructions,
/// use `Builder`
IsAllocatingCall isAllocatingCall(const CallBase &call, DMSIRBuilder& Builder) {
  CallNameInfo CNI = getCallNameInfo(call);
  switch (CNI.kind) {
    case CallNameInfo::INDIRECTCALL:
      // we assume indirect calls aren't allocating
      return IsAllocatingCall::not_allocating(CNI);
    case CallNameInfo::ANONCALL:
      // we assume anonymous functions aren't allocating
      return IsAllocatingCall::not_allocating(CNI);
    case CallNameInfo::NAMEDCALL:
      if (CNI.name == "malloc") {
        return IsAllocatingCall::allocating(call.getArgOperand(0), CNI);
      } else if (CNI.name == "realloc") {
        return IsAllocatingCall::allocating(call.getArgOperand(1), CNI);
      } else if (CNI.name == "calloc") {
        return IsAllocatingCall::allocating(Builder.CreateMul(
          call.getArgOperand(0),
          call.getArgOperand(1)
        ), CNI);
      } else if (CNI.name == "aligned_alloc") {
        return IsAllocatingCall::allocating(call.getArgOperand(1), CNI);
      }
      return IsAllocatingCall::not_allocating(CNI);
    default:
      llvm_unreachable("Missing CNI.kind case");
  }
}

/// Determine whether the GEP's total offset is a compile-time constant, and if
/// so, what constant
std::optional<APInt> computeGEPOffset(const GetElementPtrInst& gep, const DataLayout& DL) {
  APInt offset = zero;
  if (gep.accumulateConstantOffset(DL, offset)) {
    return offset;
  } else {
    return std::nullopt;
  }
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
static const char* get_bounds_func = "_ZN5__dms16__dms_get_boundsEPvPS0_S1_";
/// Mangled name of the store_bounds function
static const char* store_bounds_func = "_ZN5__dms18__dms_store_boundsEPvS0_S0_";
/// Mangled name of the store_infinite_bounds function
static const char* store_bounds_inf_func = "_ZN5__dms27__dms_store_infinite_boundsEPv";
/// Mangled name of the copy_single_bounds function
static const char* copy_single_bounds_func = "_ZN5__dms24__dms_copy_single_boundsEPvS0_";
/// Mangled name of the copy_bounds_in_interval function
static const char* copy_bounds_in_interval_func = "_ZN5__dms29__dms_copy_bounds_in_intervalEPvS0_mm";
/// Mangled name of the store_globalarraysize function
static const char* store_global_array_size_func = "_ZN5__dms27__dms_store_globalarraysizeEPvS0_";
/// Mangled name of the get_globalarraysize function
static const char* get_global_array_size_func = "_ZN5__dms25__dms_get_globalarraysizeEPvPS0_";
/// Mangled name of the boundscheckfail function
static const char* boundscheckfail_func = "_ZN5__dms21__dms_boundscheckfailEPv";

/// Convenience function to create calls to our runtime support function
/// `__dms_store_bounds()`. See docs in dms_interface.h.
///
/// The arguments `addr`, `base`, and `max` can be any pointer type (not
/// necessarily `void*`). They should be UNENCODED values, ie with all upper
/// bits clear.
CallInst* call_dms_store_bounds(const Value* addr, const Value* base, const Value* max, DMSIRBuilder& Builder) {
  Module* mod = Builder.GetInsertBlock()->getModule();
  static Type* CharStarTy = Builder.getInt8PtrTy();
  static FunctionType* StoreBoundsTy = FunctionType::get(
    Builder.getVoidTy(),
    {CharStarTy, CharStarTy, CharStarTy},
    /* IsVarArgs = */ false
  );
  FunctionCallee StoreBounds = mod->getOrInsertFunction(store_bounds_func, StoreBoundsTy);
  return Builder.CreateCall(StoreBounds, {
    Builder.castToCharStar(addr),
    Builder.castToCharStar(base),
    Builder.castToCharStar(max)
  });
}

/// Convenience function to create calls to our runtime support function
/// `__dms_store_infinite_bounds()`. See docs in dms_interface.h.
///
/// The `addr` argument can be any pointer type (not necessarily `void*`),
/// and should be an UNENCODED value, ie with all upper bits clear.
CallInst* call_dms_store_infinite_bounds(const Value* addr, DMSIRBuilder& Builder) {
  Module* mod = Builder.GetInsertBlock()->getModule();
  static Type* CharStarTy = Builder.getInt8PtrTy();
  static FunctionType* StoreBoundsInfTy = FunctionType::get(
    Builder.getVoidTy(),
    {CharStarTy},
    /* IsVarArgs = */ false
  );
  FunctionCallee StoreBoundsInf = mod->getOrInsertFunction(store_bounds_inf_func, StoreBoundsInfTy);
  return Builder.CreateCall(StoreBoundsInf, {
    Builder.castToCharStar(addr)
  });
}

/// Convenience function to create calls to our runtime support function
/// `__dms_get_bounds()`. See docs in dms_interface.h.
///
/// The `addr` argument can be any pointer type (not necessarily `void*`),
/// and should be an UNENCODED value, ie with all upper bits clear.
///
/// `output_base` and `output_max` are output parameters and should have LLVM
/// type i8**.
CallInst* call_dms_get_bounds(const Value* addr, Value* output_base, Value* output_max, DMSIRBuilder& Builder) {
  Module* mod = Builder.GetInsertBlock()->getModule();
  static Type* CharStarTy = Builder.getInt8PtrTy();
  static Type* CharStarStarTy = CharStarTy->getPointerTo();
  FunctionType* GetBoundsTy = FunctionType::get(
    Builder.getVoidTy(),
    {CharStarTy, CharStarStarTy, CharStarStarTy},
    /* IsVarArgs = */ false
  );
  FunctionCallee GetBounds = mod->getOrInsertFunction(get_bounds_func, GetBoundsTy);
  assert(output_base->getType() == CharStarStarTy);
  assert(output_max->getType() == CharStarStarTy);
  return Builder.CreateCall(GetBounds, {
    Builder.castToCharStar(addr),
    output_base,
    output_max
  });
}

/// Convenience function to create calls to our runtime support function
/// `__dms_copy_single_bounds()`. See docs in dms_interface.h.
///
/// The arguments `src` and `dst` can be any pointer type (not necessarily
/// `void*`). They should be UNENCODED values, ie with all upper bits clear.
llvm::CallInst* call_dms_copy_single_bounds(const llvm::Value* src, const llvm::Value* dst, llvm::DMSIRBuilder& Builder) {
  Module* mod = Builder.GetInsertBlock()->getModule();
  static Type* CharStarTy = Builder.getInt8PtrTy();
  FunctionType* CopySingleBoundsTy = FunctionType::get(
    Builder.getVoidTy(),
    {CharStarTy, CharStarTy},
    /* IsVarArgs = */ false
  );
  FunctionCallee CopySingleBounds = mod->getOrInsertFunction(copy_single_bounds_func, CopySingleBoundsTy);
  return Builder.CreateCall(CopySingleBounds, {
    Builder.castToCharStar(src),
    Builder.castToCharStar(dst)
  });
}

/// Convenience function to create calls to our runtime support function
/// `__dms_copy_bounds_in_interval()`. See docs in dms_interface.h.
///
/// The arguments `src` and `dst` can be any pointer type (not necessarily
/// `void*`). They should be UNENCODED values, ie with all upper bits clear.
llvm::CallInst* call_dms_copy_bounds_in_interval(const llvm::Value* src, const llvm::Value* dst, llvm::Value* len_bytes, llvm::Value* stride, llvm::DMSIRBuilder& Builder) {
  Module* mod = Builder.GetInsertBlock()->getModule();
  static Type* CharStarTy = Builder.getInt8PtrTy();
  static Type* SizeTTy = Builder.getInt64Ty();
  FunctionType* CopyBoundsInIntervalTy = FunctionType::get(
    Builder.getVoidTy(),
    {CharStarTy, CharStarTy, SizeTTy, SizeTTy},
    /* IsVarArgs = */ false
  );
  FunctionCallee CopyBoundsInInterval = mod->getOrInsertFunction(copy_bounds_in_interval_func, CopyBoundsInIntervalTy);
  assert(!len_bytes->getType()->isPointerTy());
  assert(!stride->getType()->isPointerTy());
  return Builder.CreateCall(CopyBoundsInInterval, {
    Builder.castToCharStar(src),
    Builder.castToCharStar(dst),
    len_bytes,
    stride
  });
}

/// Convenience function to create calls to our runtime support function
/// `__dms_store_globalarraysize()`. See docs in dms_interface.h.
///
/// The arguments `arr` and `max` can be any pointer type (not necessarily
/// `void*`). They should be UNENCODED values, ie with all upper bits clear.
CallInst* call_dms_store_globalarraysize(const GlobalValue* arr, const Value* max, DMSIRBuilder& Builder) {
  Module* mod = Builder.GetInsertBlock()->getModule();
  static Type* CharStarTy = Builder.getInt8PtrTy();
  static FunctionType* StoreGlobalArraySizeTy = FunctionType::get(
    Builder.getVoidTy(),
    {CharStarTy, CharStarTy},
    /* IsVarArgs = */ false
  );
  FunctionCallee StoreGlobalArraySize = mod->getOrInsertFunction(store_global_array_size_func, StoreGlobalArraySizeTy);
  return Builder.CreateCall(StoreGlobalArraySize, {
    Builder.castToCharStar(arr),
    Builder.castToCharStar(max)
  });
}

/// Convenience function to create calls to our runtime support function
/// `__dms_get_globalarraysize()`. See docs in dms_interface.h.
///
/// The `arr` argument can be any pointer type (not necessarily `void*`),
/// and should be an UNENCODED value, ie with all upper bits clear.
///
/// `output_max` is an output parameter and should have LLVM type i8**.
CallInst* call_dms_get_globalarraysize(const GlobalValue* arr, Value* output_max, DMSIRBuilder& Builder) {
  Module* mod = Builder.GetInsertBlock()->getModule();
  static Type* CharStarTy = Builder.getInt8PtrTy();
  static Type* CharStarStarTy = CharStarTy->getPointerTo();
  FunctionType* GetGlobalArraySizeTy = FunctionType::get(
    Builder.getVoidTy(),
    {CharStarTy, CharStarStarTy},
    /* IsVarArgs = */ false
  );
  FunctionCallee GetGlobalArraySize = mod->getOrInsertFunction(get_global_array_size_func, GetGlobalArraySizeTy);
  assert(output_max->getType() == CharStarStarTy);
  return Builder.CreateCall(GetGlobalArraySize, {
    Builder.castToCharStar(arr),
    output_max
  });
}

/// Convenience function to create calls to our runtime support function
/// `__dms_boundscheckfail()`. See docs in dms_interface.h.
///
/// The `ptr` argument can be any pointer type (not necessarily `void*`),
/// and should be an UNENCODED value, ie with all upper bits clear.
CallInst* call_dms_boundscheckfail(const Value* ptr, DMSIRBuilder& Builder) {
  Module* mod = Builder.GetInsertBlock()->getModule();
  static Type* CharStarTy = Builder.getInt8PtrTy();
  FunctionType* BoundsCheckFailTy = FunctionType::get(
    Builder.getVoidTy(),
    {CharStarTy},
    /* IsVarArgs = */ false
  );
  FunctionCallee BoundsCheckFail = mod->getOrInsertFunction(boundscheckfail_func, BoundsCheckFailTy);
  return Builder.CreateCall(BoundsCheckFail, {
    Builder.castToCharStar(ptr)
  });
}
