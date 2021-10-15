#include "llvm/Transforms/Utils/DMS_BoundsInfo.h"
#include "llvm/Transforms/Utils/DMS_PointerStatus.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "DMS-bounds-info"

/// Mangled name of the get_bounds function
static const char* get_bounds_func = "_ZN5__dms16__dms_get_boundsEPv";
/// Mangled name of the store_bounds function
static const char* store_bounds_func = "_ZN5__dms18__dms_store_boundsEPvS0_S0_";
/// Mangled name of the store_infinite_bounds function
/// TODO: real mangled name
static const char* store_bounds_inf_func = "__dms_store_infinite_bounds";

/// `cur_ptr`: the pointer value for which these bounds apply.
/// Can be any pointer type.
///
/// `Builder`: the DMSIRBuilder to use to insert dynamic instructions.
///
/// Returns the "base" (minimum inbounds pointer value) of the allocation,
/// as an LLVM `Value` of type `i8*`.
/// Or, if the `kind` is UNKNOWN or INFINITE, returns NULL.
/// The `kind` should not be NOTDEFINEDYET.
Value* BoundsInfo::base_as_llvm_value(Value* cur_ptr, DMSIRBuilder& Builder) const {
	switch (kind) {
		case NOTDEFINEDYET:
			llvm_unreachable("base_as_llvm_value: BoundsInfo should be defined (at least UNKNOWN)");
		case UNKNOWN:
		case INFINITE:
			return NULL;
		case STATIC:
			return info.static_info.base_as_llvm_value(cur_ptr, Builder);
		case DYNAMIC:
		case DYNAMIC_MERGED:
			return info.dynamic_info.base.as_llvm_value(Builder);
		default:
			llvm_unreachable("Missing BoundsInfo.kind case");
	}
}

/// `cur_ptr`: the pointer value for which these bounds apply.
/// Can be any pointer type.
///
/// `Builder`: the DMSIRBuilder to use to insert dynamic instructions.
///
/// Returns the "max" (maximum inbounds pointer value) of the allocation,
/// as an LLVM `Value` of type `i8*`.
/// Or, if the `kind` is UNKNOWN or INFINITE, returns NULL.
/// The `kind` should not be NOTDEFINEDYET.
Value* BoundsInfo::max_as_llvm_value(Value* cur_ptr, DMSIRBuilder& Builder) const {
	switch (kind) {
		case NOTDEFINEDYET:
			llvm_unreachable("max_as_llvm_value: BoundsInfo should be defined (at least UNKNOWN)");
		case UNKNOWN:
		case INFINITE:
			return NULL;
		case STATIC:
			return info.static_info.max_as_llvm_value(cur_ptr, Builder);
		case DYNAMIC:
		case DYNAMIC_MERGED:
			return info.dynamic_info.max.as_llvm_value(Builder);
		default:
			llvm_unreachable("Missing BoundsInfo.kind case");
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
			A.info.dynamic_info, B.info.dynamic_info, Builder
		);
	}

	if (A.kind == STATIC && B.is_dynamic()) {
		return merge_static_dynamic(
			A.info.static_info, B.info.dynamic_info, cur_ptr, Builder
		);
	}
	if (A.is_dynamic() && B.kind == STATIC) {
		return merge_static_dynamic(
			B.info.static_info, A.info.dynamic_info, cur_ptr, Builder
		);
	}

	llvm_unreachable("Missing case in BoundsInfo::merge");
}

/// `cur_ptr` is the pointer which these bounds are for.
/// Can be any pointer type.
///
/// `Builder` is the DMSIRBuilder to use to insert dynamic instructions.
BoundsInfo BoundsInfo::merge_static_dynamic(
	const StaticBoundsInfo& static_info,
	const DynamicBoundsInfo& dynamic_info,
	Value* cur_ptr,
	DMSIRBuilder& Builder
) {
	// these are the base and max from the dynamic side
	Value* incoming_base = dynamic_info.base.as_llvm_value(Builder);
	Value* incoming_max = dynamic_info.max.as_llvm_value(Builder);
	// these are the base and max from the static side
	Value* static_base = static_info.base_as_llvm_value(cur_ptr, Builder);
	Value* static_max = static_info.max_as_llvm_value(cur_ptr, Builder);

	return merge_dynamic_dynamic(
		DynamicBoundsInfo(incoming_base, incoming_max),
		DynamicBoundsInfo(static_base, static_max),
		Builder
	);
}

/// `Builder` is the DMSIRBuilder to use to insert dynamic instructions.
BoundsInfo BoundsInfo::merge_dynamic_dynamic(
	const DynamicBoundsInfo& a_info,
	const DynamicBoundsInfo& b_info,
	DMSIRBuilder& Builder
) {
	PointerWithOffset base;
	PointerWithOffset max;
	if (a_info.base.ptr == b_info.base.ptr) {
		base = PointerWithOffset(
			a_info.base.ptr,
			a_info.base.offset.slt(b_info.base.offset) ? b_info.base.offset : a_info.base.offset
		);
	} else {
		Value* a_base = a_info.base.as_llvm_value(Builder);
		Value* b_base = b_info.base.as_llvm_value(Builder);
		Value* merged_base = Builder.CreateSelect(
			Builder.CreateICmpULT(a_base, b_base),
			b_base,
			a_base
		);
		base = PointerWithOffset(merged_base);
	}
	if (a_info.max.ptr == b_info.max.ptr) {
		max = PointerWithOffset(
			a_info.max.ptr,
			a_info.max.offset.slt(b_info.max.offset) ? a_info.max.offset : b_info.max.offset
		);
	} else {
		Value* a_max = a_info.max.as_llvm_value(Builder);
		Value* b_max = b_info.max.as_llvm_value(Builder);
		Value* merged_max = Builder.CreateSelect(
			Builder.CreateICmpULT(a_max, b_max),
			a_max,
			b_max
		);
		max = PointerWithOffset(merged_max);
	}
	BoundsInfo merged = BoundsInfo::dynamic_bounds(base, max);
	merged.kind = DYNAMIC_MERGED;
	merged.merge_inputs.clear();
	merged.merge_inputs.push_back(new BoundsInfo(a_info));
	merged.merge_inputs.push_back(new BoundsInfo(b_info));
	return merged;
}

/// Insert dynamic instructions to store this bounds info.
///
/// `cur_ptr` is the pointer which these bounds are for.
///
/// `Builder` is the DMSIRBuilder to use to insert dynamic instructions.
///
/// Returns the Call instruction if one was inserted, or else NULL
Instruction* BoundsInfo::store_dynamic(Value* cur_ptr, DMSIRBuilder& Builder) const {
	assert(cur_ptr);
	assert(cur_ptr->getType()->isPointerTy());
	Module* mod = Builder.GetInsertBlock()->getModule();
	Type* CharStarTy = Builder.getInt8PtrTy();
	Value* cur_ptr_as_charstar = Builder.castToCharStar(cur_ptr);
	switch (kind) {
		case BoundsInfo::INFINITE: {
			FunctionType* StoreBoundsInfTy = FunctionType::get(Builder.getVoidTy(), {CharStarTy}, /* IsVarArgs = */ false);
			FunctionCallee StoreBoundsInf = mod->getOrInsertFunction(store_bounds_inf_func, StoreBoundsInfTy);
			return Builder.CreateCall(StoreBoundsInf, {cur_ptr_as_charstar});
		}
		case BoundsInfo::UNKNOWN: {
			DEBUG_WITH_TYPE("DMS", dbgs() << "DMS:   warning: bounds info unknown for " << cur_ptr->getNameOrAsOperand() << "; considering it as infinite bounds\n");
			FunctionType* StoreBoundsInfTy = FunctionType::get(Builder.getVoidTy(), {CharStarTy}, /* IsVarArgs = */ false);
			FunctionCallee StoreBoundsInf = mod->getOrInsertFunction(store_bounds_inf_func, StoreBoundsInfTy);
			return Builder.CreateCall(StoreBoundsInf, {cur_ptr_as_charstar});
		}
		case BoundsInfo::NOTDEFINEDYET: {
			dbgs() << "error during store_dynamic for " << cur_ptr->getNameOrAsOperand() << "\n";
			llvm_unreachable("store_dynamic: bounds info should be defined (at least UNKNOWN)");
		}
		default: {
			Value* base = base_as_llvm_value(cur_ptr, Builder);
			Value* max = max_as_llvm_value(cur_ptr, Builder);
			if (base && max) {
				FunctionType* StoreBoundsTy = FunctionType::get(Builder.getVoidTy(), {CharStarTy, CharStarTy, CharStarTy}, /* IsVarArgs = */ false);
				FunctionCallee StoreBounds = mod->getOrInsertFunction(store_bounds_func, StoreBoundsTy);
				return Builder.CreateCall(StoreBounds, {cur_ptr_as_charstar, base, max});
			} else {
				llvm_unreachable("base and/or max are NULL, but boundsinfo is not INFINITE or UNKNOWN or NOTDEFINEDYET");
			}
		}
	}
}

/// Insert dynamic instructions to load bounds info for the given `ptr`.
/// `ptr` can be of any pointer type.
/// Bounds info for this `ptr` should have been previously stored with
/// `store_dynamic_boundsinfo`.
///
/// Insert dynamic instructions using the given `DMSIRBuilder`.
BoundsInfo::DynamicBoundsInfo BoundsInfo::load_dynamic(Value* ptr, DMSIRBuilder& Builder) {
	assert(ptr);
	assert(ptr->getType()->isPointerTy());
	Module* mod = Builder.GetInsertBlock()->getModule();
	Type* CharStarTy = Builder.getInt8PtrTy();
	Type* GetBoundsRetTy = StructType::get(mod->getContext(), {CharStarTy, CharStarTy});
	FunctionType* GetBoundsTy = FunctionType::get(GetBoundsRetTy, CharStarTy, /* IsVarArgs = */ false);
	FunctionCallee GetBounds = mod->getOrInsertFunction(get_bounds_func, GetBoundsTy);
	Value* dynbounds = Builder.CreateCall(GetBounds, Builder.castToCharStar(ptr));
	Value* base = Builder.CreateExtractValue(dynbounds, 0);
	Value* max = Builder.CreateExtractValue(dynbounds, 1);
	return BoundsInfo::DynamicBoundsInfo(base, max);
}

BoundsInfos::BoundsInfos(
	const Function& F,
	const DataLayout& DL,
	DenseSet<const Instruction*>& added_insts,
	DenseMap<const Value*, SmallDenseSet<const Value*, 4>>& pointer_aliases
) : DL(DL), added_insts(added_insts), pointer_aliases(pointer_aliases) {
	// For now, if any function parameters are pointers, mark their bounds info
	// as UNKNOWN
	for (const Argument& arg : F.args()) {
		if (arg.getType()->isPointerTy()) {
			map[&arg] = BoundsInfo::unknown();
		}
	}

	// Mark appropriate bounds info for global variables. We know this bounds
	// information statically
	for (const GlobalValue& gv : F.getParent()->global_values()) {
		assert(gv.getType()->isPointerTy());
		Type* globalType = gv.getValueType();
		if (globalType->isSized()) {
			auto allocationSize = DL.getTypeStoreSize(globalType).getFixedSize();
			map[&gv] = BoundsInfo::static_bounds(
				zero, APInt(/* bits = */ 64, /* val = */ allocationSize - 1)
			);
		} else {
			map[&gv] = BoundsInfo::unknown();
		}
	}
}

/// Get the bounds information for the given pointer.
BoundsInfo BoundsInfos::get_binfo(const Value* ptr) {
	BoundsInfo binfo = get_binfo_noalias(ptr);
	if (binfo.get_kind() != BoundsInfo::NOTDEFINEDYET) return binfo;
	// if bounds info isn't defined, see if it's defined for any alias of this pointer
	for (const Value* alias : pointer_aliases[ptr]) {
		binfo = get_binfo_noalias(alias);
		if (binfo.get_kind() != BoundsInfo::NOTDEFINEDYET) return binfo;
	}
	return binfo;
}

/// Like `get_binfo()`, but doesn't check aliases of the given ptr, if any
/// exist. This is used internally by `get_binfo()`.
BoundsInfo BoundsInfos::get_binfo_noalias(const Value* ptr) {
	assert(ptr->getType()->isPointerTy());
	BoundsInfo binfo = map.lookup(ptr);
	if (binfo.get_kind() != BoundsInfo::NOTDEFINEDYET) return binfo;
	if (const Constant* constant = dyn_cast<const Constant>(ptr)) {
		if (constant->isNullValue()) {
			return BoundsInfo::infinite();
		} else if (isa<UndefValue>(constant)) {
			// this includes both undef and poison
			return BoundsInfo::infinite();
		} else if (const ConstantExpr* expr = dyn_cast<ConstantExpr>(constant)) {
			// it's a pointer created by a compile-time constant expression
			switch (expr->getOpcode()) {
				case Instruction::BitCast: {
					// bitcast doesn't change the bounds
					return get_binfo_noalias(expr->getOperand(0));
				}
				case Instruction::GetElementPtr: {
					// constant-GEP expression
					Instruction* inst = expr->getAsInstruction();
					GetElementPtrInst* gepinst = cast<GetElementPtrInst>(inst);
					propagate_bounds(*gepinst, DL);
					return map.lookup(gepinst);
				}
				case Instruction::IntToPtr: {
					// return a NOTDEFINEDYET; ideally, we have alias information, and an
					// alias will have bounds info
					return BoundsInfo();
				}
				default: {
					dbgs() << "unhandled constant expression:\n";
					expr->dump();
					llvm_unreachable("getting bounds info for unhandled constant expression");
				}
			}
		} else {
			dbgs() << "unhandled constant:\n";
			constant->dump();
			llvm_unreachable("unhandled constant type (not an expression)");
		}
	}
	return binfo;
}

/// Propagate bounds information for a Store instruction.
void BoundsInfos::propagate_bounds(StoreInst& store) {
	// if we aren't storing a pointer, we have nothing to do
	Value* storedVal = store.getValueOperand();
	if (!storedVal->getType()->isPointerTy()) return;
	// we store the bounds info so that when this pointer is later
	// loaded, we can get the bounds info back
	BoundsInfo binfo = get_binfo(storedVal);
	bool need_regenerate_bounds_store;
	if (store_bounds_calls.count(&store) > 0) {
		// there is already a Call instruction storing bounds info for
		// this stored pointer. Make sure that the bounds info it's
		// storing hasn't changed.
		BoundsStoringCall& BSC = store_bounds_calls[&store];
		if (binfo == BSC.binfo) {
			// bounds info is up to date; nothing to do
			need_regenerate_bounds_store = false;
		} else {
			// whoops, bounds info has changed. remove the old Call
			// instruction storing the bounds info, and generate a new one
			BSC.call_inst->eraseFromParent();
			need_regenerate_bounds_store = true;
		}
	} else {
		need_regenerate_bounds_store = true;
	}
	if (need_regenerate_bounds_store) {
		DMSIRBuilder Builder(&store, DMSIRBuilder::AFTER, &added_insts);
		Instruction* new_bounds_call = binfo.store_dynamic(storedVal, Builder);
		store_bounds_calls[&store] = BoundsStoringCall(new_bounds_call, binfo);
	}
}

/// Propagate bounds information for an Alloca instruction.
void BoundsInfos::propagate_bounds(AllocaInst& alloca) {
	// we know the bounds of the allocation statically
	PointerType* resultType = cast<PointerType>(alloca.getType());
	auto allocationSize = DL.getTypeStoreSize(resultType->getElementType()).getFixedSize();
	map[&alloca] = BoundsInfo::static_bounds(
		zero, APInt(/* bits = */ 64, /* val = */ allocationSize - 1)
	);
}

/// Copy the bounds for the input pointer (must be operand 0) to the output
/// pointer
void BoundsInfos::propagate_bounds_id(Instruction& inst) {
	Value* input_ptr = inst.getOperand(0);
	assert(input_ptr->getType()->isPointerTy());
	map[&inst] = get_binfo(input_ptr);
}

/// Propagate bounds information for a GEP instruction.
void BoundsInfos::propagate_bounds(GetElementPtrInst& gep, const DataLayout& DL) {
	// propagate the input pointer's bounds to the new pointer. We let
	// the new pointer still have access to the whole allocation
	Value* input_ptr = gep.getPointerOperand();
	const BoundsInfo binfo = get_binfo(input_ptr);
	switch (binfo.get_kind()) {
		case BoundsInfo::NOTDEFINEDYET:
			llvm_unreachable("GEP input ptr's BoundsInfo should be defined (at least UNKNOWN)");
		case BoundsInfo::UNKNOWN:
			map[&gep] = binfo;
			break;
		case BoundsInfo::INFINITE:
			map[&gep] = binfo;
			break;
		case BoundsInfo::STATIC: {
			const BoundsInfo::StaticBoundsInfo* static_info = binfo.static_info();
			const GEPConstantOffset gco = computeGEPOffset(gep, DL);
			if (gco.is_constant) {
				map[&gep] = BoundsInfo::static_bounds(
					static_info->low_offset + gco.offset,
					static_info->high_offset - gco.offset
				);
			} else {
				// bounds of the new pointer aren't known statically
				// and actually, we don't care what the dynamic GEP offset is:
				// it doesn't change the `base` and `max` of the allocation
				const BoundsInfo::StaticBoundsInfo* input_static_info = binfo.static_info();
				// `base` is `input_ptr` minus the input pointer's low_offset
				const BoundsInfo::PointerWithOffset base = BoundsInfo::PointerWithOffset(input_ptr, -input_static_info->low_offset);
				// `max` is `input_ptr` plus the input pointer's high_offset
				const BoundsInfo::PointerWithOffset max = BoundsInfo::PointerWithOffset(input_ptr, input_static_info->high_offset);
				map[&gep] = BoundsInfo::dynamic_bounds(base, max);
			}
			break;
		}
		case BoundsInfo::DYNAMIC:
		case BoundsInfo::DYNAMIC_MERGED:
		{
			// regardless of the GEP offset, the `base` and `max` don't change
			map[&gep] = binfo;
			break;
		}
		default:
			llvm_unreachable("Missing BoundsInfo.kind case");
	}
}

void BoundsInfos::propagate_bounds(SelectInst& select) {
	// if we aren't selecting a pointer, we have nothing to do
	if (!select.getType()->isPointerTy()) return;
	const BoundsInfo& binfo1 = get_binfo(select.getTrueValue());
	const BoundsInfo& binfo2 = get_binfo(select.getFalseValue());
	const BoundsInfo& prev_iteration_binfo = get_binfo(&select);
	if (
		prev_iteration_binfo.get_kind() == BoundsInfo::DYNAMIC_MERGED
		&& prev_iteration_binfo.merge_inputs.size() == 2
		&& *prev_iteration_binfo.merge_inputs[0] == binfo1
		&& *prev_iteration_binfo.merge_inputs[1] == binfo2
	) {
		// no need to update
	} else {
		// the merge may need to insert instructions that use the final
		// value of the select, so we need a builder pointing after the
		// select
		DMSIRBuilder AfterSelect(&select, DMSIRBuilder::AFTER, &added_insts);
		map[&select] = BoundsInfo::merge(binfo1, binfo2, &select, AfterSelect);
	}
}

void BoundsInfos::propagate_bounds(IntToPtrInst& inttoptr, PointerKind inttoptr_kind) {
	// if we're considering it a clean ptr, then also assume it is valid for the
	// entire size of the data its type claims it points to
	if (inttoptr_kind == PointerKind::CLEAN) {
		PointerType* resultType = cast<PointerType>(inttoptr.getType());
		auto allocationSize = DL.getTypeStoreSize(resultType->getElementType()).getFixedSize();
		map[&inttoptr] = BoundsInfo::static_bounds(
			zero, APInt(/* bits = */ 64, /* val = */ allocationSize - 1)
		);
	} else {
		map[&inttoptr] = BoundsInfo::unknown();
	}
}

void BoundsInfos::propagate_bounds(PHINode& phi) {
	// if the PHI isn't choosing between pointers, we have nothing to do
	if (!phi.getType()->isPointerTy()) return;
	SmallVector<std::tuple<Value*, BoundsInfo, BasicBlock*>, 4> incoming_binfos;
	for (const Use& use : phi.incoming_values()) {
		BasicBlock* bb = phi.getIncomingBlock(use);
		Value* value = use.get();
		incoming_binfos.push_back(std::make_tuple(
			value,
			get_binfo(value),
			bb
		));
	}
	DMSIRBuilder Builder(&phi, DMSIRBuilder::BEFORE, &added_insts);
	const BoundsInfo prev_iteration_binfo = get_binfo(&phi);
	assert(incoming_binfos.size() >= 1);
	bool any_incoming_bounds_are_dynamic = false;
	bool any_incoming_bounds_are_unknown = false;
	bool any_incoming_bounds_are_notdefinedyet = false;
	for (auto& tuple : incoming_binfos) {
		const BoundsInfo incoming_binfo = std::get<1>(tuple);
		if (incoming_binfo.is_dynamic()) {
			any_incoming_bounds_are_dynamic = true;
		}
		if (incoming_binfo.get_kind() == BoundsInfo::UNKNOWN) {
			any_incoming_bounds_are_unknown = true;
		}
		if (incoming_binfo.get_kind() == BoundsInfo::NOTDEFINEDYET) {
			any_incoming_bounds_are_notdefinedyet = true;
		}
	}
	if (any_incoming_bounds_are_unknown) {
		map[&phi] = BoundsInfo::unknown();
	} else if (any_incoming_bounds_are_dynamic) {
		if (any_incoming_bounds_are_notdefinedyet) {
			// in this case, just mark UNKNOWN for this iteration; we'll
			// insert PHIs and compute the proper dynamic bounds in the next
			// iteration, when everything is defined
			map[&phi] = BoundsInfo::unknown();
		} else {
			// in this case, we'll use PHIs to select the proper dynamic
			// `base` and `max`, much as we used PHIs for the dynamic_kind
			// above.
			// Of course, if we already inserted PHIs in a previous iteration,
			// let's not insert them again.
			PHINode* base_phi = NULL;
			PHINode* max_phi = NULL;
			if (const BoundsInfo::DynamicBoundsInfo* prev_iteration_dyninfo = prev_iteration_binfo.dynamic_info()) {
				if ((base_phi = dyn_cast<PHINode>(prev_iteration_dyninfo->base.ptr))) {
					assert(prev_iteration_dyninfo->base.offset == 0);
					assert(incoming_binfos.size() == base_phi->getNumIncomingValues());
					for (auto& tuple : incoming_binfos) {
						const BoundsInfo incoming_binfo = std::get<1>(tuple);
						BasicBlock* incoming_bb = std::get<2>(tuple);
						const Value* old_base = base_phi->getIncomingValueForBlock(incoming_bb);
						switch (incoming_binfo.get_kind()) {
							case BoundsInfo::NOTDEFINEDYET:
							case BoundsInfo::UNKNOWN:
							case BoundsInfo::INFINITE:
								llvm_unreachable("Bad incoming_binfo.kind here");
							case BoundsInfo::STATIC:
								// for now we just assume that static bounds don't
								// change from iteration to iteration
								break;
							case BoundsInfo::DYNAMIC:
							case BoundsInfo::DYNAMIC_MERGED:
							{
								const BoundsInfo::DynamicBoundsInfo* incoming_dyninfo = incoming_binfo.dynamic_info();
								if (incoming_dyninfo->base.offset != 0) {
									// need to recalculate base_phi
									base_phi = NULL;
									break;
								}
								const Value* incoming_base = incoming_dyninfo->base.ptr;
								if (incoming_base != old_base) {
									// need to recalculate base_phi
									base_phi = NULL;
									break;
								}
								// if we get here, this incoming block is still good and doesn't need recalculating
								break;
							}
							default:
								llvm_unreachable("Missing BoundsInfo.kind case");
						}
						if (!base_phi) break;
					}
				} else {
					// prev_iteration base was not a phi. we'll have to insert a fresh phi
				}
				if ((max_phi = dyn_cast<PHINode>(prev_iteration_dyninfo->max.ptr))) {
					assert(prev_iteration_dyninfo->max.offset == 0);
					assert(incoming_binfos.size() == max_phi->getNumIncomingValues());
					for (auto& tuple : incoming_binfos) {
						const BoundsInfo incoming_binfo = std::get<1>(tuple);
						BasicBlock* incoming_bb = std::get<2>(tuple);
						const Value* old_max = max_phi->getIncomingValueForBlock(incoming_bb);
						switch (incoming_binfo.get_kind()) {
							case BoundsInfo::NOTDEFINEDYET:
							case BoundsInfo::UNKNOWN:
							case BoundsInfo::INFINITE:
								llvm_unreachable("Bad incoming_binfo.kind here");
							case BoundsInfo::STATIC:
								// for now we just assume that static bounds don't
								// change from iteration to iteration
								break;
							case BoundsInfo::DYNAMIC:
							case BoundsInfo::DYNAMIC_MERGED:
							{
								const BoundsInfo::DynamicBoundsInfo* incoming_dyninfo = incoming_binfo.dynamic_info();
								if (incoming_dyninfo->max.offset != 0) {
									// need to recalculate max_phi
									max_phi = NULL;
									break;
								}
								const Value* incoming_max = incoming_dyninfo->max.ptr;
								if (incoming_max != old_max) {
									// need to recalculate max_phi
									max_phi = NULL;
									break;
								}
								// if we get here, this incoming block is still good and doesn't need recalculating
								break;
							}
							default:
								llvm_unreachable("Missing BoundsInfo.kind case");
						}
						if (!max_phi) break;
					}
				} else {
					// prev_iteration max was not a phi. we'll have to insert a fresh phi
				}
			} else {
				// prev_iteration boundsinfo was not dynamic. we'll have to insert fresh phis
			}
			if (!base_phi) {
				base_phi = Builder.CreatePHI(Builder.getInt8PtrTy(), phi.getNumIncomingValues());
				added_insts.insert(base_phi);
				for (auto& tuple : incoming_binfos) {
					Value* incoming_ptr = std::get<0>(tuple);
					const BoundsInfo incoming_binfo = std::get<1>(tuple);
					BasicBlock* incoming_bb = std::get<2>(tuple);
					// if dynamic instructions are necessary to compute phi
					// incoming value, insert them at the end of the
					// corresponding block, not here
					DMSIRBuilder IncomingBlockBuilder(incoming_bb, DMSIRBuilder::END, &added_insts);
					Value* base = incoming_binfo.base_as_llvm_value(incoming_ptr, IncomingBlockBuilder);
					assert(base);
					base_phi->addIncoming(base, incoming_bb);
				}
				assert(base_phi->isComplete());
			}
			if (!max_phi) {
				max_phi = Builder.CreatePHI(Builder.getInt8PtrTy(), phi.getNumIncomingValues());
				added_insts.insert(max_phi);
				for (auto& tuple : incoming_binfos) {
					Value* incoming_ptr = std::get<0>(tuple);
					const BoundsInfo incoming_binfo = std::get<1>(tuple);
					BasicBlock* incoming_bb = std::get<2>(tuple);
					// if dynamic instructions are necessary to compute phi
					// incoming value, insert them at the end of the
					// corresponding block, not here
					DMSIRBuilder IncomingBlockBuilder(incoming_bb, DMSIRBuilder::END, &added_insts);
					Value* max = incoming_binfo.max_as_llvm_value(incoming_ptr, IncomingBlockBuilder);
					assert(max);
					max_phi->addIncoming(max, incoming_bb);
				}
				assert(max_phi->isComplete());
			}
			map[&phi] = BoundsInfo::dynamic_bounds(base_phi, max_phi);
		}
	} else {
		// no incoming bounds are dynamic. let's just merge them statically
		bool any_merge_inputs_changed = false;
		if (prev_iteration_binfo.get_kind() != BoundsInfo::DYNAMIC_MERGED)
			any_merge_inputs_changed = true;
		if (prev_iteration_binfo.merge_inputs.size() != incoming_binfos.size())
			any_merge_inputs_changed = true;
		if (!any_merge_inputs_changed) {
			for (unsigned i = 0; i < incoming_binfos.size(); i++) {
				if (*prev_iteration_binfo.merge_inputs[i] != std::get<1>(incoming_binfos[i])) {
					any_merge_inputs_changed = true;
					break;
				}
			}
		}
		if (any_merge_inputs_changed) {
			// have to update the boundsinfo
			BoundsInfo merged_binfo = BoundsInfo::infinite(); // just the initial value we start the merge with
			assert(phi.getNumIncomingValues() >= 1);
			for (auto& tuple : incoming_binfos) {
				const BoundsInfo binfo = std::get<1>(tuple);
				merged_binfo = BoundsInfo::merge(merged_binfo, binfo, &phi, Builder);
			}
			map[&phi] = merged_binfo;
		}
	}
}

void BoundsInfos::propagate_bounds(CallBase& call, IsAllocatingCall& IAC) {
	if (IAC.is_allocating) {
		if (ConstantInt* allocationSize = dyn_cast<ConstantInt>(IAC.allocation_bytes)) {
			// allocating a constant number of bytes.
			// we know the bounds of the allocation statically.
			map[&call] = BoundsInfo::static_bounds(
				zero, allocationSize->getValue() - 1
			);
		} else {
			// allocating a dynamic number of bytes.
			// We need a dynamic addition instruction to compute the upper
			// bound. Only insert that the first time -- the bounds info
			// here should not change from iteration to iteration
			if (!is_binfo_present(&call)) {
				DMSIRBuilder Builder(&call, DMSIRBuilder::AFTER, &added_insts);
				Value* callPlusBytes = Builder.add_offset_to_ptr(&call, IAC.allocation_bytes);
				Value* max = Builder.add_offset_to_ptr(callPlusBytes, minusone);
				map[&call] = BoundsInfo::dynamic_bounds(&call, max);
			}
		}
	} else {
		map[&call] = BoundsInfo::unknown();
	}
}
