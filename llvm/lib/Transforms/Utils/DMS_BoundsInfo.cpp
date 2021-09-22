#include "llvm/Transforms/Utils/DMS_BoundsInfo.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

/// Mangled name of the get_bounds function
static const char* get_bounds_func = "_ZN5__dms16__dms_get_boundsEPv";
/// Mangled name of the store_bounds function
static const char* store_bounds_func = "_ZN5__dms18__dms_store_boundsEPvS0_S0_";

/// `cur_ptr`: the pointer value for which these bounds apply.
/// Can be any pointer type.
///
/// `Builder`: the IRBuilder to use to insert dynamic instructions.
///
/// `bounds_insts`: If we insert any instructions into the program, we'll
/// also add them to `bounds_insts`, see notes there
///
/// Returns the "base" (minimum inbounds pointer value) of the allocation,
/// as an LLVM `Value` of type `i8*`.
/// Or, if the `kind` is UNKNOWN or INFINITE, returns NULL.
/// The `kind` should not be NOTDEFINEDYET.
Value* BoundsInfo::base_as_llvm_value(
	Value* cur_ptr,
	IRBuilder<>& Builder,
	DenseSet<const Instruction*>& bounds_insts
) const {
	switch (kind) {
		case NOTDEFINEDYET:
			llvm_unreachable("base_as_llvm_value: BoundsInfo should be defined (at least UNKNOWN)");
		case UNKNOWN:
		case INFINITE:
			return NULL;
		case STATIC:
			return info.static_info.base_as_llvm_value(cur_ptr, Builder, bounds_insts);
		case DYNAMIC:
		case DYNAMIC_MERGED:
			return info.dynamic_info.base.as_llvm_value(Builder, bounds_insts);
		default:
			llvm_unreachable("Missing BoundsInfo.kind case");
	}
}

/// `cur_ptr`: the pointer value for which these bounds apply.
/// Can be any pointer type.
///
/// `Builder`: the IRBuilder to use to insert dynamic instructions.
///
/// `bounds_insts`: If we insert any instructions into the program, we'll
/// also add them to `bounds_insts`, see notes there
///
/// Returns the "max" (maximum inbounds pointer value) of the allocation,
/// as an LLVM `Value` of type `i8*`.
/// Or, if the `kind` is UNKNOWN or INFINITE, returns NULL.
/// The `kind` should not be NOTDEFINEDYET.
Value* BoundsInfo::max_as_llvm_value(
	Value* cur_ptr,
	IRBuilder<>& Builder,
	DenseSet<const Instruction*>& bounds_insts
) const {
	switch (kind) {
		case NOTDEFINEDYET:
			llvm_unreachable("max_as_llvm_value: BoundsInfo should be defined (at least UNKNOWN)");
		case UNKNOWN:
		case INFINITE:
			return NULL;
		case STATIC:
			return info.static_info.max_as_llvm_value(cur_ptr, Builder, bounds_insts);
		case DYNAMIC:
		case DYNAMIC_MERGED:
			return info.dynamic_info.max.as_llvm_value(Builder, bounds_insts);
		default:
			llvm_unreachable("Missing BoundsInfo.kind case");
	}
}

/// `cur_ptr` is the pointer which these bounds are for.
/// Can be any pointer type.
///
/// `Builder` is the IRBuilder to use to insert dynamic instructions, if
/// that is necessary.
///
/// `bounds_insts`: If we insert any instructions into the program, we'll
/// also add them to `bounds_insts`, see notes there
BoundsInfo BoundsInfo::merge(
	const BoundsInfo& A,
	const BoundsInfo& B,
	Value* cur_ptr,
	IRBuilder<>& Builder,
	DenseSet<const Instruction*>& bounds_insts
) {
	if (A.kind == NOTDEFINEDYET) return B;
	if (B.kind == NOTDEFINEDYET) return A;
	if (A.kind == UNKNOWN) return A;
	if (B.kind == UNKNOWN) return B;
	if (A.kind == INFINITE) return B;
	if (B.kind == INFINITE) return A;

	if (A.kind == STATIC && B.kind == STATIC) {
		const StaticBoundsInfo &a_info = A.info.static_info;
		const StaticBoundsInfo &b_info = B.info.static_info;
		return BoundsInfo::static_bounds(
			a_info.low_offset.slt(b_info.low_offset) ? a_info.low_offset : b_info.low_offset,
			a_info.high_offset.slt(b_info.high_offset) ? a_info.high_offset : b_info.high_offset
		);
	}

	if (A.is_dynamic() && B.is_dynamic()) {
		return merge_dynamic_dynamic(
			A.info.dynamic_info, B.info.dynamic_info, Builder, bounds_insts
		);
	}

	if (A.kind == STATIC && B.is_dynamic()) {
		return merge_static_dynamic(
			A.info.static_info, B.info.dynamic_info, cur_ptr, Builder, bounds_insts
		);
	}
	if (A.is_dynamic() && B.kind == STATIC) {
		return merge_static_dynamic(
			B.info.static_info, A.info.dynamic_info, cur_ptr, Builder, bounds_insts
		);
	}

	llvm_unreachable("Missing case in BoundsInfo::merge");
}

/// Casts the given input pointer `ptr` to the LLVM type `i8*`.
/// The input pointer can be any pointer type, including `i8*` (in which case
/// this will return the pointer unchanged).
///
/// `Builder`: the IRBuilder to use to insert dynamic instructions.
///
/// `bounds_insts`: If we insert any instructions into the program, we'll
/// also add them to `bounds_insts`, see notes there
static Value* castToCharStar(
	Value* ptr,
	IRBuilder<>& Builder,
	DenseSet<const Instruction*>& bounds_insts
) {
	Value* casted = Builder.CreatePointerCast(ptr, Builder.getInt8PtrTy());
	if (casted != ptr) {
		bounds_insts.insert(cast<Instruction>(casted));
	}
	return casted;
}

/// Adds the given `offset` (in _bytes_) to the given `ptr`, and returns
/// the resulting pointer.
/// The input pointer can be any pointer type, the output pointer will
/// have type `i8*`.
///
/// `Builder`: the IRBuilder to use to insert dynamic instructions.
///
/// `bounds_insts`: If we insert any instructions into the program, we'll
/// also add them to `bounds_insts`, see notes there
Value* BoundsInfo::add_offset_to_ptr(
	Value* ptr,
	APInt offset,
	IRBuilder<>& Builder,
	DenseSet<const Instruction*>& bounds_insts)
{
	Value* casted = castToCharStar(ptr, Builder, bounds_insts);
	if (offset == 0) {
		return casted;
	} else {
		Value* GEP = Builder.CreateGEP(Builder.getInt8Ty(), casted, Builder.getInt(offset));
		if (GEP != casted) {
			bounds_insts.insert(cast<Instruction>(GEP));
		}
		return GEP;
	}
}

/// `cur_ptr` is the pointer which these bounds are for.
/// Can be any pointer type.
///
/// `Builder` is the IRBuilder to use to insert dynamic instructions.
///
/// `bounds_insts`: If we insert any instructions into the program, we'll
/// also add them to `bounds_insts`, see notes there
BoundsInfo BoundsInfo::merge_static_dynamic(
	const StaticBoundsInfo& static_info,
	const DynamicBoundsInfo& dynamic_info,
	Value* cur_ptr,
	IRBuilder<>& Builder,
	DenseSet<const Instruction*>& bounds_insts
) {
	// these are the base and max from the dynamic side
	Value* incoming_base = dynamic_info.base.as_llvm_value(Builder, bounds_insts);
	Value* incoming_max = dynamic_info.max.as_llvm_value(Builder, bounds_insts);
	// these are the base and max from the static side
	Value* static_base = static_info.base_as_llvm_value(cur_ptr, Builder, bounds_insts);
	Value* static_max = static_info.max_as_llvm_value(cur_ptr, Builder, bounds_insts);

	return merge_dynamic_dynamic(
		DynamicBoundsInfo(incoming_base, incoming_max),
		DynamicBoundsInfo(static_base, static_max),
		Builder,
		bounds_insts
	);
}

/// `Builder` is the IRBuilder to use to insert dynamic instructions.
///
/// `bounds_insts`: If we insert any instructions into the program, we'll
/// also add them to `bounds_insts`, see notes there
BoundsInfo BoundsInfo::merge_dynamic_dynamic(
	const DynamicBoundsInfo& a_info,
	const DynamicBoundsInfo& b_info,
	IRBuilder<>& Builder,
	DenseSet<const Instruction*>& bounds_insts
) {
	PointerWithOffset base;
	PointerWithOffset max;
	if (a_info.base.ptr == b_info.base.ptr) {
		base = PointerWithOffset(
			a_info.base.ptr,
			a_info.base.offset.slt(b_info.base.offset) ? b_info.base.offset : a_info.base.offset
		);
	} else {
		Value* a_base = a_info.base.as_llvm_value(Builder, bounds_insts);
		Value* b_base = b_info.base.as_llvm_value(Builder, bounds_insts);
		Value* merged_base = Builder.CreateSelect(
			Builder.CreateICmpULT(a_base, b_base),
			b_base,
			a_base
		);
		if (merged_base != a_base && merged_base != b_base) {
			bounds_insts.insert(cast<Instruction>(merged_base));
		}
		base = PointerWithOffset(merged_base);
	}
	if (a_info.max.ptr == b_info.max.ptr) {
		max = PointerWithOffset(
			a_info.max.ptr,
			a_info.max.offset.slt(b_info.max.offset) ? a_info.max.offset : b_info.max.offset
		);
	} else {
		Value* a_max = a_info.max.as_llvm_value(Builder, bounds_insts);
		Value* b_max = b_info.max.as_llvm_value(Builder, bounds_insts);
		Value* merged_max = Builder.CreateSelect(
			Builder.CreateICmpULT(a_max, b_max),
			a_max,
			b_max
		);
		if (merged_max != a_max && merged_max != b_max) {
			bounds_insts.insert(cast<Instruction>(merged_max));
		}
		max = PointerWithOffset(merged_max);
	}
	BoundsInfo merged = BoundsInfo::dynamic_bounds(base, max);
	merged.kind = DYNAMIC_MERGED;
	merged.merge_inputs.clear();
	merged.merge_inputs.push_back(new BoundsInfo(a_info));
	merged.merge_inputs.push_back(new BoundsInfo(b_info));
	return merged;
}


/// Insert dynamic instructions indicating a bounds-check failure, at the
/// program point indicated by the given `Builder`
///
/// `bounds_insts`: As we insert instructions into the program, we'll also add
/// them to `bounds_insts`, see notes there
static void insertBoundsCheckFail(
	IRBuilder<>& Builder,
	DenseSet<const Instruction*>& bounds_insts
) {
	Module* mod = Builder.GetInsertBlock()->getModule();
	FunctionType* AbortTy = FunctionType::get(Builder.getVoidTy(), /* IsVarArgs = */ false);
	FunctionCallee Abort = mod->getOrInsertFunction("abort", AbortTy);
	Value* call = Builder.CreateCall(Abort);
	bounds_insts.insert(cast<Instruction>(call));
	Builder.CreateUnreachable();
}

/// Create a new BasicBlock containing code indicating a bounds-check failure.
/// The BasicBlock will be inserted in the given `Function`.
/// We can dynamically jump to this block to report a bounds-check failure.
/// Returns the BasicBlock.
///
/// `bounds_insts`: As we insert instructions into the program, we'll also add
/// them to `bounds_insts`, see notes there
BasicBlock* llvm::boundsCheckFailBB(
	Function* F,
	DenseSet<const Instruction*>& bounds_insts
) {
	BasicBlock* boundsfail = BasicBlock::Create(F->getContext(), "", F);
	IRBuilder<> BoundsFailBuilder(boundsfail, boundsfail->getFirstInsertionPt());
	insertBoundsCheckFail(BoundsFailBuilder, bounds_insts);
	return boundsfail;
}

/// Insert a SW bounds check of `ptr` against the bounds information in this
/// `BoundsInfo`.
///
/// `Builder` is the IRBuilder to use to insert dynamic instructions, if
/// that is necessary. To be safe, you should assume `Builder` is invalidated
/// when this function returns.
///
/// `bounds_insts`: If we insert any instructions into the program, we'll
/// also add them to `bounds_insts`, see notes there
void BoundsInfo::sw_bounds_check(
	Value* ptr,
	IRBuilder<>& Builder,
	DenseSet<const Instruction*>& bounds_insts
) const {
	switch (kind) {
		case NOTDEFINEDYET:
			llvm_unreachable("Can't sw_bounds_check with NOTDEFINEDYET bounds");
		case UNKNOWN:
			dbgs() << "warning: bounds info unknown for " << ptr->getNameOrAsOperand() << " even though it needs a bounds check. Unsafely omitting the bounds check.\n";
			return;
		case INFINITE:
			// bounds check passes by default
			return;
		case STATIC:
			static_info()->sw_bounds_check(ptr, Builder, bounds_insts);
			break;
		case DYNAMIC:
		case DYNAMIC_MERGED:
			dynamic_info()->sw_bounds_check(ptr, Builder, bounds_insts);
			break;
		default:
			llvm_unreachable("Missing BoundsInfo.kind case");
	}
}

/// Insert a SW bounds check of `ptr` against the bounds information in this
/// `StaticBoundsInfo`.
///
/// `Builder` is the IRBuilder to use to insert dynamic instructions, if
/// that is necessary.
///
/// `bounds_insts`: If we insert any instructions into the program, we'll
/// also add them to `bounds_insts`, see notes there
void BoundsInfo::StaticBoundsInfo::sw_bounds_check(
	Value* ptr,
	IRBuilder<>& Builder,
	DenseSet<const Instruction*>& bounds_insts
) const {
	// implementation doesn't actually need `ptr`
	if (fails()) {
		insertBoundsCheckFail(Builder, bounds_insts);
	}
}

/// Insert a SW bounds check of `ptr` against the bounds information in this
/// `DynamicBoundsInfo`.
///
/// `Builder` is the IRBuilder to use to insert dynamic instructions, if
/// that is necessary. To be safe, you should assume `Builder` is invalidated
/// when this function returns.
///
/// `bounds_insts`: If we insert any instructions into the program, we'll
/// also add them to `bounds_insts`, see notes there
void BoundsInfo::DynamicBoundsInfo::sw_bounds_check(
	Value* ptr,
	IRBuilder<>& Builder,
	DenseSet<const Instruction*>& bounds_insts
) const {
	ptr = castToCharStar(ptr, Builder, bounds_insts);
	Value* LowFail = Builder.CreateICmpULT(ptr, base.as_llvm_value(Builder, bounds_insts));
	bounds_insts.insert(cast<Instruction>(LowFail));
	Value* HighFail = Builder.CreateICmpUGT(ptr, max.as_llvm_value(Builder, bounds_insts));
	bounds_insts.insert(cast<Instruction>(HighFail));
	Value* Fail = Builder.CreateLogicalOr(LowFail, HighFail);
	BasicBlock* bb = Builder.GetInsertBlock();
	BasicBlock::iterator I = Builder.GetInsertPoint();
	BasicBlock* new_bb = bb->splitBasicBlock(I);
	// at this point, `bb` holds everything before the pointer
	// dereference, and an unconditional-br terminator to `new_bb`.
	// `new_bb` holds the dereference and everything following,
	// including the old terminator.
	// To be safe, we assume that `Builder` is invalidated by the above
	// operation (docs say that the iterator `I` is invalidated).
	BasicBlock* boundsfail = boundsCheckFailBB(bb->getParent(), bounds_insts);
	BasicBlock::iterator bbend = bb->getTerminator()->eraseFromParent();
	IRBuilder<> NewBuilder(bb, bbend);
	NewBuilder.CreateCondBr(Fail, boundsfail, new_bb);
}

/// Insert dynamic instructions to store bounds info for the given `ptr`.
/// `ptr` can be of any pointer type.
///
/// Insert dynamic instructions using the given `IRBuilder`.
///
/// `bounds_insts`: If we insert any instructions into the program, we'll
/// also add them to `bounds_insts`, see notes there
void llvm::store_dynamic_boundsinfo(
  Value* ptr,
  const BoundsInfo& binfo,
  IRBuilder<>& Builder,
  DenseSet<const Instruction*>& bounds_insts
) {
	Module* mod = Builder.GetInsertBlock()->getModule();
	Type* CharStarTy = Builder.getInt8PtrTy();
	FunctionType* StoreBoundsTy = FunctionType::get(Builder.getVoidTy(), {CharStarTy, CharStarTy, CharStarTy}, /* IsVarArgs = */ false);
	FunctionCallee StoreBounds = mod->getOrInsertFunction(store_bounds_func, StoreBoundsTy);
	Value* base = binfo.base_as_llvm_value(ptr, Builder, bounds_insts);
	Value* max = binfo.max_as_llvm_value(ptr, Builder, bounds_insts);
	Value* ptr_as_charstar = castToCharStar(ptr, Builder, bounds_insts);
	Value* call = Builder.CreateCall(StoreBounds, {ptr_as_charstar, base, max});
	bounds_insts.insert(cast<Instruction>(call));
}

/// Insert dynamic instructions to load bounds info for the given `ptr`.
/// `ptr` can be of any pointer type.
/// Bounds info for this `ptr` should have been previously stored with
/// `store_dynamic_boundsinfo`.
///
/// Insert dynamic instructions using the given `IRBuilder`.
///
/// `bounds_insts`: If we insert any instructions into the program, we'll
/// also add them to `bounds_insts`, see notes there
BoundsInfo::DynamicBoundsInfo llvm::load_dynamic_boundsinfo(
  Value* ptr,
  IRBuilder<>& Builder,
  DenseSet<const Instruction*>& bounds_insts
) {
	Module* mod = Builder.GetInsertBlock()->getModule();
	Type* CharStarTy = Builder.getInt8PtrTy();
	Type* GetBoundsRetTy = StructType::get(mod->getContext(), {CharStarTy, CharStarTy});
	FunctionType* GetBoundsTy = FunctionType::get(GetBoundsRetTy, CharStarTy, /* IsVarArgs = */ false);
	FunctionCallee GetBounds = mod->getOrInsertFunction(get_bounds_func, GetBoundsTy);
	// TODO: IRBuilder supports some kind of hook for instruction
	// insertion, maybe we can have the adding-to-bounds_insts be part
	// of this hook rather than remembering to do it individually
	// every time
	Value* arg = castToCharStar(ptr, Builder, bounds_insts);
	Value* dynbounds = Builder.CreateCall(GetBounds, arg);
	bounds_insts.insert(cast<Instruction>(dynbounds));
	Value* base = Builder.CreateExtractValue(dynbounds, 0);
	bounds_insts.insert(cast<Instruction>(base));
	Value* max = Builder.CreateExtractValue(dynbounds, 1);
	bounds_insts.insert(cast<Instruction>(max));
	return BoundsInfo::DynamicBoundsInfo(base, max);
}
