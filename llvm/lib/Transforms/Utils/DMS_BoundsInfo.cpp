#include "llvm/Transforms/Utils/DMS_BoundsInfo.h"
#include "llvm/Transforms/Utils/DMS_PointerStatus.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <variant>

using namespace llvm;

#define DEBUG_TYPE "DMS-bounds-info"

static Value* get_min_ptr_value(DMSIRBuilder& Builder) {
  return Builder.Insert(Constant::getNullValue(Builder.getInt8PtrTy()));
}
static Value* get_max_ptr_value(DMSIRBuilder& Builder) {
  return Builder.CreateIntToPtr(Constant::getAllOnesValue(Builder.getInt64Ty()), Builder.getInt8PtrTy());
}

std::string BoundsInfo::pretty() const {
  if (const Static* sinfo = std::get_if<Static>(&data)) {
    std::string out;
    raw_string_ostream ostream(out);
    ostream << pretty_kind() << " [";
    sinfo->low_offset.print(ostream, /* isSigned = */ true);
    ostream << ",";
    sinfo->high_offset.print(ostream, /* isSigned = */ true);
    ostream << "]";
    return ostream.str();
  } else {
    return pretty_kind();
  }
}

/// `cur_ptr`: the pointer value for which these bounds apply.
/// Can be any pointer type.
///
/// `Builder`: the DMSIRBuilder to use to insert dynamic instructions.
///
/// Returns the "base" (minimum inbounds pointer value) of the allocation,
/// as an LLVM `Value` of type `i8*`.
/// Or, if the BoundsInfo is Unknown or Infinite, returns NULL.
/// The BoundsInfo should not be NotDefinedYet.
Value* BoundsInfo::base_as_llvm_value(Value* cur_ptr, DMSIRBuilder& Builder) const {
  if (const NotDefinedYet* ndy = std::get_if<NotDefinedYet>(&data)) {
    (void)ndy; // silence warning about unused variable
    llvm_unreachable("base_as_llvm_value: BoundsInfo should be defined (at least Unknown)");
  } else if (const Unknown* unk = std::get_if<Unknown>(&data)) {
    (void)unk; // silence warning about unused variable
    return NULL;
  } else if (const Infinite* inf = std::get_if<Infinite>(&data)) {
    (void)inf; // silence warning about unused variable
    return get_min_ptr_value(Builder);
  } else if (const Static* sinfo = std::get_if<Static>(&data)) {
    return sinfo->base_as_llvm_value(cur_ptr, Builder);
  } else if (const Dynamic* dinfo = std::get_if<Dynamic>(&data)) {
    return dinfo->getBase().as_llvm_value(Builder);
  } else {
    llvm_unreachable("Missing BoundsInfo case");
  }
}

/// `cur_ptr`: the pointer value for which these bounds apply.
/// Can be any pointer type.
///
/// `Builder`: the DMSIRBuilder to use to insert dynamic instructions.
///
/// Returns the "max" (maximum inbounds pointer value) of the allocation,
/// as an LLVM `Value` of type `i8*`.
/// Or, if the BoundsInfo is Unknown or Infinite, returns NULL.
/// The BoundsInfo should not be NotDefinedYet.
Value* BoundsInfo::max_as_llvm_value(Value* cur_ptr, DMSIRBuilder& Builder) const {
  if (const NotDefinedYet* ndy = std::get_if<NotDefinedYet>(&data)) {
    (void)ndy; // silence warning about unused variable
    llvm_unreachable("max_as_llvm_value: BoundsInfo should be defined (at least Unknown)");
  } else if (const Unknown* unk = std::get_if<Unknown>(&data)) {
    (void)unk; // silence warning about unused variable
    return NULL;
  } else if (const Infinite* inf = std::get_if<Infinite>(&data)) {
    (void)inf; // silence warning about unused variable
    return get_max_ptr_value(Builder);
  } else if (const Static* sinfo = std::get_if<Static>(&data)) {
    return sinfo->max_as_llvm_value(cur_ptr, Builder);
  } else if (const Dynamic* dinfo = std::get_if<Dynamic>(&data)) {
    return dinfo->getMax().as_llvm_value(Builder);
  } else {
    llvm_unreachable("Missing BoundsInfo case");
  }
}

/// `cur_ptr` is the pointer which these bounds are for.
/// Can be any pointer type.
///
/// `Builder` is the DMSIRBuilder to use to insert dynamic instructions, if
/// that is necessary.
BoundsInfo BoundsInfo::merge(
  const BoundsInfo& A,
  const BoundsInfo& B,
  Value* cur_ptr,
  DMSIRBuilder& Builder
) {
  if (A.is_notdefinedyet()) return B;
  if (B.is_notdefinedyet()) return A;
  if (A.is_unknown()) return A;
  if (B.is_unknown()) return B;
  if (A.is_infinite()) return B;
  if (B.is_infinite()) return A;

  if (A.is_static() && B.is_static()) {
    const Static& a_info = std::get<Static>(A.data);
    const Static& b_info = std::get<Static>(B.data);
    return BoundsInfo::static_bounds(
      a_info.low_offset.sgt(b_info.low_offset) ? a_info.low_offset : b_info.low_offset,
      a_info.high_offset.slt(b_info.high_offset) ? a_info.high_offset : b_info.high_offset
    );
  }

  if (A.is_dynamic() && B.is_dynamic()) {
    return merge_dynamic_dynamic(
      std::get<Dynamic>(A.data), std::get<Dynamic>(B.data), Builder
    );
  }

  if (A.is_static() && B.is_dynamic()) {
    return merge_static_dynamic(
      std::get<Static>(A.data), std::get<Dynamic>(B.data), cur_ptr, Builder
    );
  }
  if (A.is_dynamic() && B.is_static()) {
    return merge_static_dynamic(
      std::get<Static>(B.data), std::get<Dynamic>(A.data), cur_ptr, Builder
    );
  }

  llvm_unreachable("Missing case in BoundsInfo::merge");
}

/// `cur_ptr` is the pointer which these bounds are for.
/// Can be any pointer type.
///
/// `Builder` is the DMSIRBuilder to use to insert dynamic instructions.
BoundsInfo BoundsInfo::merge_static_dynamic(
  const Static& static_info,
  const Dynamic& dynamic_info,
  Value* cur_ptr,
  DMSIRBuilder& Builder
) {
  // these are the base and max from the dynamic side
  Value* incoming_base = dynamic_info.getBase().as_llvm_value(Builder);
  Value* incoming_max = dynamic_info.getMax().as_llvm_value(Builder);
  // these are the base and max from the static side
  Value* static_base = static_info.base_as_llvm_value(cur_ptr, Builder);
  Value* static_max = static_info.max_as_llvm_value(cur_ptr, Builder);

  return merge_dynamic_dynamic(
    Dynamic(incoming_base, incoming_max),
    Dynamic(static_base, static_max),
    Builder
  );
}

/// `Builder` is the DMSIRBuilder to use to insert dynamic instructions.
BoundsInfo BoundsInfo::merge_dynamic_dynamic(
  const Dynamic& a_info,
  const Dynamic& b_info,
  DMSIRBuilder& Builder
) {
  if (a_info == b_info) return BoundsInfo(a_info); // this also avoids forcing, if both a_info and b_info are still lazy but equivalent
  PointerWithOffset base;
  PointerWithOffset max;
  const PointerWithOffset& a_base = a_info.getBase();
  const PointerWithOffset& b_base = b_info.getBase();
  const PointerWithOffset& a_max = a_info.getMax();
  const PointerWithOffset& b_max = b_info.getMax();
  if (a_base.ptr == b_base.ptr) {
    base = PointerWithOffset(
      a_base.ptr,
      a_base.offset.slt(b_base.offset) ? b_base.offset : a_base.offset
    );
  } else {
    Value* a_base_val = a_base.as_llvm_value(Builder);
    Value* b_base_val = b_base.as_llvm_value(Builder);
    Value* merged_base = Builder.CreateSelect(
      Builder.CreateICmpULT(a_base_val, b_base_val),
      b_base_val,
      a_base_val,
      "merged_base"
    );
    base = PointerWithOffset(merged_base);
  }
  if (a_max.ptr == b_max.ptr) {
    max = PointerWithOffset(
      a_max.ptr,
      a_max.offset.slt(b_max.offset) ? a_max.offset : b_max.offset
    );
  } else {
    Value* a_max_val = a_max.as_llvm_value(Builder);
    Value* b_max_val = b_max.as_llvm_value(Builder);
    Value* merged_max = Builder.CreateSelect(
      Builder.CreateICmpULT(a_max_val, b_max_val),
      a_max_val,
      b_max_val,
      "merged_max"
    );
    max = PointerWithOffset(merged_max);
  }
  Dynamic merged(base, max);
  merged.merge_inputs.clear();
  merged.merge_inputs.push_back(new BoundsInfo(a_info));
  merged.merge_inputs.push_back(new BoundsInfo(b_info));
  return BoundsInfo(merged);
}

/// Insert dynamic instructions to store this bounds info.
///
/// `ptr` is the pointer this bounds info applies to; call it P. `addr` is &P.
/// Both `ptr` and `addr` need to be _decoded_ pointer values.
///
/// `Builder` is the DMSIRBuilder to use to insert dynamic instructions.
///
/// Returns the Call instruction if one was inserted, or else NULL
CallInst* BoundsInfo::store_dynamic(Value* addr, Value* ptr, DMSIRBuilder& Builder) const {
  assert(ptr);
  assert(addr);
  assert(ptr->getType()->isPointerTy());
  assert(addr->getType()->isPointerTy());
  assert(cast<PointerType>(addr->getType())->getElementType()->isPointerTy());
  //assert(cast<PointerType>(addr->getType())->getElementType() == ptr->getType());
  LLVM_DEBUG(dbgs() << "Inserting a call to store dynamic bounds info for the pointer " << ptr->getNameOrAsOperand() << " stored at " << addr->getNameOrAsOperand() << "\n");
  if (is_notdefinedyet()) {
    errs() << "error during store_dynamic with ptr=" << ptr->getNameOrAsOperand() << ", addr=" << addr->getNameOrAsOperand() << "\n";
    llvm_unreachable("store_dynamic: bounds info should be defined (at least Unknown)");
  } else if (is_unknown()) {
    errs() << "warning: bounds info unknown for the pointer " << ptr->getNameOrAsOperand() << " stored at " << addr->getNameOrAsOperand() << "; considering it as infinite bounds\n";
    return call_dms_store_infinite_bounds(addr, Builder);
  } else if (is_infinite()) {
    return call_dms_store_infinite_bounds(addr, Builder);
  } else {
    Value* base = base_as_llvm_value(ptr, Builder);
    Value* max = max_as_llvm_value(ptr, Builder);
    if (base && max) {
      return call_dms_store_bounds(addr, base, max, Builder);
    } else {
      llvm_unreachable("base and/or max are NULL, but boundsinfo is not Infinite or Unknown or NotDefinedYet");
    }
  }
}

BoundsInfo::Dynamic::Info BoundsInfo::Dynamic::LazyInfo::force() {
  assert(addr);
  assert(loaded_ptr);
  assert(addr->getType()->isPointerTy());
  assert(loaded_ptr->getType()->isPointerTy());
  assert(cast<PointerType>(addr->getType())->getElementType()->isPointerTy());
  //assert(cast<PointerType>(addr->getType())->getElementType() == loaded_ptr->getType());
  LLVM_DEBUG(dbgs() << "Inserting a call to load dynamic bounds info for the pointer " << loaded_ptr->getNameOrAsOperand() << " loaded from " << addr->getNameOrAsOperand() << "\n");
  std::string loaded_ptr_name = isa<ConstantExpr>(loaded_ptr) ? "constexpr" : loaded_ptr->getNameOrAsOperand();
  DMSIRBuilder Builder(loaded_ptr, DMSIRBuilder::AFTER, added_insts);
  static Type* CharStarTy = Builder.getInt8PtrTy();
  Value* output_base = Builder.CreateAlloca(CharStarTy, nullptr, Twine(loaded_ptr_name, "_output_base"));
  Value* output_max = Builder.CreateAlloca(CharStarTy, nullptr, Twine(loaded_ptr_name, "_output_max"));
  call_dms_get_bounds(addr, output_base, output_max, Builder);
  Value* base = Builder.CreateLoad(CharStarTy, output_base, Twine(loaded_ptr_name, "_base"));
  Value* max = Builder.CreateLoad(CharStarTy, output_max, Twine(loaded_ptr_name, "_max"));
  return Dynamic::Info(base, max);
}

BoundsInfo::Dynamic::Info BoundsInfo::Dynamic::LazyGlobalArraySize::force() {
  assert(arr);
  assert(arr->getType()->isPointerTy());
  assert(arr->hasName());
  LLVM_DEBUG(dbgs() << "Inserting a call to load dynamic global array size for " << arr->getNameOrAsOperand() << "\n");
  // inserting at the top of `insertion_func` is not necessarily the most efficient, but it's easiest for now
  DMSIRBuilder Builder(&insertion_func->getEntryBlock(), DMSIRBuilder::BEGINNING, added_insts);
  static Type* CharStarTy = Builder.getInt8PtrTy();
  Value* output_max = Builder.CreateAlloca(CharStarTy, nullptr, Twine(arr->getName(), "_output_dynamic_max"));
  call_dms_get_globalarraysize(arr, output_max, Builder);
  Value* max = Builder.CreateLoad(CharStarTy, output_max, Twine(arr->getName(), "_dynamic_max"));
  return Dynamic::Info(arr, max);
}
