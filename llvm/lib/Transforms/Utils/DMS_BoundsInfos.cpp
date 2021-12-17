#include "llvm/Transforms/Utils/DMS_BoundsInfos.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

#define DEBUG_TYPE "DMS-bounds-info"

BoundsInfos::BoundsInfos(
  Function& F,
  const DataLayout& DL,
  DenseSet<const Instruction*>& added_insts,
  DenseMap<const Value*, SmallDenseSet<const Value*, 4>>& pointer_aliases
) : DL(DL), added_insts(added_insts), pointer_aliases(pointer_aliases) {
  LLVM_DEBUG(dbgs() << "Initializing bounds infos for function " << F.getNameOrAsOperand() << " with " << F.arg_size() << " operands\n");
  bool isMain = F.getName() == "main" && F.arg_size() == 2;
  if (isMain) {
    LLVM_DEBUG(dbgs() << "This is a 'main' function, setting bounds for argv and globals\n");
    Value* argc = F.getArg(0);
    Value* argv = F.getArg(1);
    assert(argv->getType()->isPointerTy());
    {
      // bounds for argv: it's an array of size argc
      DMSIRBuilder Builder(&F.getEntryBlock(), DMSIRBuilder::BEGINNING, &added_insts);
      Value* argvMax = Builder.add_offset_to_ptr(argv,
        /* argc * pointer_size_in_bytes - 1 */
        Builder.CreateSub(
          Builder.CreateMul(argc, ConstantInt::get(argc->getType(), 8)),
          ConstantInt::get(argc->getType(), 1, /* signed = */ true),
					"argvMax"
        )
      );
      map[argv] = BoundsInfo::dynamic_bounds(argv, argvMax);
      LLVM_DEBUG(dbgs() << "Setting bounds for " << argv->getNameOrAsOperand() << " to dynamic\n");
    }
    {
      // bounds for each of the strings in argv:
      // for now, just assume infinite (so we won't bounds-check accesses to the
      // strings in argv)
      // In the future, instead we could call strlen() on each of them
      // dynamically, in order to establish dynamic bounds
      // At any rate, we need a dynamic for-loop, in order to call
      // __dms_store_infinite_bounds() for each argv element, since we don't know
      // how many argv elements there are statically
      BasicBlock& Entry = F.getEntryBlock();
      BasicBlock* forloopbody = Entry.splitBasicBlock(Entry.getFirstInsertionPt());
      DMSIRBuilder Builder(forloopbody, forloopbody->getFirstInsertionPt(), &added_insts);
      PHINode* loopindex = Builder.CreatePHI(Builder.getInt32Ty(), 2, "__dms_argc_loopindex");
      loopindex->addIncoming(Builder.getIntN(/* bits = */ 32, /* val = */ 0), &Entry);
      Value* stringptr = Builder.CreateGEP(Builder.getInt8PtrTy(), argv, loopindex);
      call_dms_store_infinite_bounds(stringptr, Builder);
      Value* incd_loopindex = Builder.CreateAdd(loopindex, Builder.getIntN(/* bits = */ 32, /* val = */ 1));
      loopindex->addIncoming(incd_loopindex, forloopbody);
      Value* cond = Builder.CreateICmpULT(incd_loopindex, argc);
      Builder.insertCondJumpTo(cond, forloopbody);
    }
  } else {
    // For now, if any function parameters are pointers, mark their bounds info
    // as UNKNOWN
    for (const Argument& arg : F.args()) {
      if (arg.getType()->isPointerTy()) {
        map[&arg] = BoundsInfo::unknown();
      }
    }
  }

  // Mark appropriate bounds info for global variables. We know this bounds
  // information statically
  for (GlobalValue& gv : F.getParent()->global_values()) {
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

/// Call this (at least) once for each source _module_ (not function)
void BoundsInfos::module_initialization(
  Module& mod,
  DenseSet<const Instruction*>& added_insts,
  DenseMap<const Value*, SmallDenseSet<const Value*, 4>>& pointer_aliases
) {
  // the problem we're solving here is that global initializers could contain
  // pointers to other globals.
  // If we load one of these pointers from a global, we expect to find dynamic
  // bounds info for it in the table.
  // But, there was never a `Store` instruction storing the pointer to that
  // memory, so our hooks never ensured that bounds info for it was added to
  // the table.
  // Hence, this function exists to add all such pointers to the table.
  // This is done once per module, and initializing the table happens in an
  // LLVM module-level global constructor.

	// if this function already exists in the module, assume we've already added
	// this initialization code
	if (mod.getFunction("__DMS_bounds_initialization")) {
		return;
	}

  // This `new_func` does the initialization
  FunctionType* FuncTy = FunctionType::get(Type::getVoidTy(mod.getContext()), {}, false);
	Function* new_func = cast<Function>(mod.getOrInsertFunction("__DMS_bounds_initialization", FuncTy).getCallee());
	new_func->setLinkage(GlobalValue::PrivateLinkage);
	BasicBlock* EntryBlock = BasicBlock::Create(mod.getContext(), "entry", new_func);
  DMSIRBuilder Builder(EntryBlock, DMSIRBuilder::BEGINNING, &added_insts);
  BoundsInfos binfos(*new_func, mod.getDataLayout(), added_insts, pointer_aliases);
  for (GlobalObject& gobj : mod.global_objects()) {
    if (GlobalVariable* gv = dyn_cast<GlobalVariable>(&gobj)) {
      if (gv->hasInitializer()) {
        Constant* initializer = gv->getInitializer();
        binfos.store_info_for_all_ptr_exprs(gv, initializer, Builder);
      }
    }
  }
  Builder.CreateRetVoid();
  // Ensure `new_func` is called during module construction. 65535 should ensure
  // it's called after any other such hooks
  appendToGlobalCtors(mod, new_func, 65535);
}

/// For all pointer expressions used in the given `Constant` (which we assume is
/// the initializer for the given `addr`), make entries in the dynamic bounds
/// table for each pointer expression. (This includes, eg, pointers to global
/// variables, GEPs of such pointers, etc.)
///
/// If dynamic instructions need to be inserted, use `Builder`.
void BoundsInfos::store_info_for_all_ptr_exprs(Value* addr, Constant* c, DMSIRBuilder& Builder) {
  if (GlobalValue* gv = dyn_cast<GlobalValue>(c)) {
    assert(gv->getType()->isPointerTy());
    BoundsInfo binfo = get_binfo(gv);
    binfo.store_dynamic(addr, gv, Builder);
  } else if (c->getType()->isPointerTy() && c->isNullValue()) {
    BoundsInfo binfo = get_binfo(c);
    binfo.store_dynamic(addr, c, Builder);
  } else if (ConstantAggregate* cagg = dyn_cast<ConstantAggregate>(c)) {
    for (unsigned i = 0; i < cagg->getNumOperands(); i++) {
      Value* op = cagg->getOperand(i);
      unsigned gepIndexSizeBits = DL.getIndexSizeInBits(cast<PointerType>(addr->getType())->getAddressSpace());
      store_info_for_all_ptr_exprs(
        Builder.CreateGEP(cagg->getType(), addr, {
          Builder.getIntN(gepIndexSizeBits, 0),
          Builder.getIntN(gepIndexSizeBits, i)
        }),
        cast<Constant>(op), // op must be a constant
        Builder
      );
    }
  } else if (ConstantExpr* cexpr = dyn_cast<ConstantExpr>(c)) {
    switch (cexpr->getOpcode()) {
      case Instruction::BitCast:
      case Instruction::AddrSpaceCast:
      {
        store_info_for_all_ptr_exprs(addr, cexpr->getOperand(0), Builder);
        break;
      }
      case Instruction::GetElementPtr: {
        BoundsInfo binfo = get_binfo(cexpr);
        binfo.store_dynamic(addr, cexpr, Builder);
        break;
      }
      default: {
        break;
      }
    }
  }
}

/// Get the bounds information for the given pointer.
BoundsInfo BoundsInfos::get_binfo(const Value* ptr) const {
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
BoundsInfo BoundsInfos::get_binfo_noalias(const Value* ptr) const {
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
          BoundsInfo ret = bounds_info_for_gep(*gepinst);
          inst->deleteValue();
          return ret;
        }
        case Instruction::IntToPtr: {
          // ideally, we have alias information, and some alias will have bounds
          // info
          return BoundsInfo::notdefinedyet();
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
///
/// Namely, we store the bounds info so that when this pointer is later loaded,
/// we can get the bounds info back.
///
/// If `override_stored_ptr` is not NULL, then store bounds info for that,
/// rather than the actual value being `store`d. This is used when we're storing
/// an encoded value, in which case we need to store bounds info for the decoded
/// value (which will be passed as `override_stored_ptr`)
void BoundsInfos::propagate_bounds(StoreInst& store, Value* override_stored_ptr) {
  // if we aren't storing a pointer, we have nothing to do
  Value* storedVal = override_stored_ptr == NULL ? store.getValueOperand() : override_stored_ptr;
  if (!storedVal->getType()->isPointerTy()) return;
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
    Instruction* new_bounds_call = binfo.store_dynamic(store.getPointerOperand(), storedVal, Builder);
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

BoundsInfo BoundsInfos::bounds_info_for_gep(GetElementPtrInst& gep) const {
  // the pointer resulting from the GEP still gets access to the whole allocation,
  // ie the same access that the GEP's input pointer had
  Value* input_ptr = gep.getPointerOperand();
  const BoundsInfo binfo = get_binfo(input_ptr);
  switch (binfo.get_kind()) {
    case BoundsInfo::NOTDEFINEDYET:
      llvm_unreachable("GEP input ptr's BoundsInfo should be defined (at least UNKNOWN)");
    case BoundsInfo::UNKNOWN:
      LLVM_DEBUG(dbgs() << "GEP input ptr has unknown bounds\n");
      return binfo;
    case BoundsInfo::INFINITE:
      return binfo;
    case BoundsInfo::STATIC: {
      const BoundsInfo::StaticBoundsInfo* static_info = binfo.static_info();
      const GEPConstantOffset gco = computeGEPOffset(gep, DL);
      if (gco.is_constant) {
        return BoundsInfo::static_bounds(
          static_info->low_offset - gco.offset,
          static_info->high_offset - gco.offset
        );
      } else {
        // bounds of the new pointer aren't known statically
        // and actually, we don't care what the dynamic GEP offset is:
        // it doesn't change the `base` and `max` of the allocation
        const BoundsInfo::StaticBoundsInfo* input_static_info = binfo.static_info();
        // `base` is `input_ptr` plus the input pointer's low_offset
        const BoundsInfo::PointerWithOffset base = BoundsInfo::PointerWithOffset(input_ptr, input_static_info->low_offset);
        // `max` is `input_ptr` plus the input pointer's high_offset
        const BoundsInfo::PointerWithOffset max = BoundsInfo::PointerWithOffset(input_ptr, input_static_info->high_offset);
        return BoundsInfo::dynamic_bounds(base, max);
      }
    }
    case BoundsInfo::DYNAMIC:
    case BoundsInfo::DYNAMIC_MERGED:
    {
      // regardless of the GEP offset, the `base` and `max` don't change
      return binfo;
    }
    default:
      llvm_unreachable("Missing BoundsInfo.kind case");
  }
}

/// Propagate bounds information for a GEP instruction.
void BoundsInfos::propagate_bounds(GetElementPtrInst& gep) {
  map[&gep] = bounds_info_for_gep(gep);
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

/// the loaded_ptr may be different from the literal result of the `load` due to
/// pointer encoding
void BoundsInfos::propagate_bounds(LoadInst& load, Instruction* loaded_ptr) {
  // if the load isn't loading a pointer, we have nothing to do
  if (!load.getType()->isPointerTy()) return;
  // compute the bounds of the loaded pointer. if we need to (eventually,
  // lazily) do a dynamic lookup in the global bounds table, this requires the
  // decoded pointer value.
  BoundsInfo::DynamicBoundsInfo dyninfo = BoundsInfo::dynamic_bounds_for_ptr(
    load.getPointerOperand(),
    loaded_ptr,
    added_insts
  );
  map[&load] = BoundsInfo(std::move(dyninfo));
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
  const BoundsInfo prev_iteration_binfo = get_binfo(&phi);
  assert(incoming_binfos.size() >= 1);
  bool any_incoming_bounds_are_dynamic = false;
  bool any_incoming_bounds_are_unknown = false;
  for (auto& tuple : incoming_binfos) {
    const BoundsInfo incoming_binfo = std::get<1>(tuple);
    if (incoming_binfo.is_dynamic()) {
      any_incoming_bounds_are_dynamic = true;
    }
    if (incoming_binfo.get_kind() == BoundsInfo::UNKNOWN) {
      any_incoming_bounds_are_unknown = true;
    }
  }
  if (any_incoming_bounds_are_unknown) {
    map[&phi] = BoundsInfo::unknown();
  } else if (any_incoming_bounds_are_dynamic) {
    // in this case, we'll use PHIs to select the proper dynamic
    // `base` and `max`, much as we used PHIs for the dynamic_kind
    // above.
    // Of course, if we already inserted PHIs in a previous iteration,
    // let's not insert them again.
    PHINode* base_phi = NULL;
    PHINode* max_phi = NULL;
    if (const BoundsInfo::DynamicBoundsInfo* prev_iteration_dyninfo = prev_iteration_binfo.dynamic_info()) {
      if ((base_phi = dyn_cast<PHINode>(prev_iteration_dyninfo->getBase().ptr))) {
        assert(prev_iteration_dyninfo->getBase().offset == 0);
        assert(incoming_binfos.size() == base_phi->getNumIncomingValues());
        for (auto& tuple : incoming_binfos) {
          Value* incoming_ptr = std::get<0>(tuple);
          const BoundsInfo incoming_binfo = std::get<1>(tuple);
          BasicBlock* incoming_bb = std::get<2>(tuple);
          // if dynamic instructions are necessary to compute phi
          // incoming value, insert them at the end of the
          // corresponding block, not here
          DMSIRBuilder IncomingBlockBuilder(incoming_bb, DMSIRBuilder::END, &added_insts);
          Value* incoming_base;
          if (incoming_binfo.get_kind() == BoundsInfo::NOTDEFINEDYET) {
            // for now, treat this input to the phi as INFINITE bounds.
            // this will be updated on a future iteration if necessary, once the
            // incoming pointer has defined bounds.
            // We use INFINITE instead of UNKNOWN because with UNKNOWN, we force
            // all dependent pointers to be UNKNOWN, and this could force this
            // phi input to be UNKNOWN in the next iteration and result in a
            // false fixpoint where everything is UNKNOWN.
            // Instead, INFINITE allows us to compute the
            // most-permissive-possible bounds for this phi input for the next
            // iteration.
            incoming_base = BoundsInfo::infinite().base_as_llvm_value(incoming_ptr, IncomingBlockBuilder);
          } else {
            incoming_base = incoming_binfo.base_as_llvm_value(incoming_ptr, IncomingBlockBuilder);
          }
          assert(incoming_base);
          const Value* old_base = base_phi->getIncomingValueForBlock(incoming_bb);
          if (incoming_base != old_base) {
            base_phi->setIncomingValueForBlock(incoming_bb, incoming_base);
          }
        }
      } else {
        // prev_iteration base was not a phi. we'll have to insert a fresh phi
      }
      if ((max_phi = dyn_cast<PHINode>(prev_iteration_dyninfo->getMax().ptr))) {
        assert(prev_iteration_dyninfo->getMax().offset == 0);
        assert(incoming_binfos.size() == max_phi->getNumIncomingValues());
        for (auto& tuple : incoming_binfos) {
          Value* incoming_ptr = std::get<0>(tuple);
          const BoundsInfo incoming_binfo = std::get<1>(tuple);
          BasicBlock* incoming_bb = std::get<2>(tuple);
          // if dynamic instructions are necessary to compute phi
          // incoming value, insert them at the end of the
          // corresponding block, not here
          DMSIRBuilder IncomingBlockBuilder(incoming_bb, DMSIRBuilder::END, &added_insts);
          Value* incoming_max;
          if (incoming_binfo.get_kind() == BoundsInfo::NOTDEFINEDYET) {
            // see notes above for the base_phi case
            incoming_max = BoundsInfo::infinite().max_as_llvm_value(incoming_ptr, IncomingBlockBuilder);
          } else {
            incoming_max = incoming_binfo.max_as_llvm_value(incoming_ptr, IncomingBlockBuilder);
          }
          assert(incoming_max);
          const Value* old_max = max_phi->getIncomingValueForBlock(incoming_bb);
          if (incoming_max != old_max) {
            max_phi->setIncomingValueForBlock(incoming_bb, incoming_max);
          }
        }
      } else {
        // prev_iteration max was not a phi. we'll have to insert a fresh phi
      }
    } else {
      // prev_iteration boundsinfo was not dynamic. we'll have to insert fresh phis
    }
    if (!base_phi) {
      DMSIRBuilder PhiBuilder(phi.getParent(), DMSIRBuilder::BEGINNING, &added_insts);
      base_phi = PhiBuilder.CreatePHI(PhiBuilder.getInt8PtrTy(), phi.getNumIncomingValues(), Twine(phi.getNameOrAsOperand(), "_base"));
      added_insts.insert(base_phi);
      for (auto& tuple : incoming_binfos) {
        Value* incoming_ptr = std::get<0>(tuple);
        const BoundsInfo incoming_binfo = std::get<1>(tuple);
        BasicBlock* incoming_bb = std::get<2>(tuple);
        // if dynamic instructions are necessary to compute phi
        // incoming value, insert them at the end of the
        // corresponding block, not here
        DMSIRBuilder IncomingBlockBuilder(incoming_bb, DMSIRBuilder::END, &added_insts);
        Value* base;
        if (incoming_binfo.get_kind() == BoundsInfo::NOTDEFINEDYET) {
          // for now, treat this input to the phi as INFINITE bounds.
          // this will be updated on a future iteration if necessary, once the
          // incoming pointer has defined bounds.
          // We use INFINITE instead of UNKNOWN because with UNKNOWN, we force
          // all dependent pointers to be UNKNOWN, and this could force this phi
          // input to be UNKNOWN in the next iteration and result in a false
          // fixpoint where everything is UNKNOWN.
          // Instead, INFINITE allows us to compute the most-permissive-possible
          // bounds for this phi input for the next iteration.
          base = BoundsInfo::infinite().base_as_llvm_value(incoming_ptr, IncomingBlockBuilder);
        } else {
          base = incoming_binfo.base_as_llvm_value(incoming_ptr, IncomingBlockBuilder);
        }
        assert(base);
        base_phi->addIncoming(base, incoming_bb);
      }
      assert(base_phi->isComplete());
    }
    if (!max_phi) {
      DMSIRBuilder PhiBuilder(phi.getParent(), DMSIRBuilder::BEGINNING, &added_insts);
      max_phi = PhiBuilder.CreatePHI(PhiBuilder.getInt8PtrTy(), phi.getNumIncomingValues(), Twine(phi.getNameOrAsOperand(), "_max"));
      added_insts.insert(max_phi);
      for (auto& tuple : incoming_binfos) {
        Value* incoming_ptr = std::get<0>(tuple);
        const BoundsInfo incoming_binfo = std::get<1>(tuple);
        BasicBlock* incoming_bb = std::get<2>(tuple);
        // if dynamic instructions are necessary to compute phi
        // incoming value, insert them at the end of the
        // corresponding block, not here
        DMSIRBuilder IncomingBlockBuilder(incoming_bb, DMSIRBuilder::END, &added_insts);
        Value* max;
        if (incoming_binfo.get_kind() == BoundsInfo::NOTDEFINEDYET) {
          // see notes above for the base_phi case
          max = BoundsInfo::infinite().max_as_llvm_value(incoming_ptr, IncomingBlockBuilder);
        } else {
          max = incoming_binfo.max_as_llvm_value(incoming_ptr, IncomingBlockBuilder);
        }
        assert(max);
        max_phi->addIncoming(max, incoming_bb);
      }
      assert(max_phi->isComplete());
    }
    map[&phi] = BoundsInfo::dynamic_bounds(base_phi, max_phi);
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

      DMSIRBuilder PostPhiBuilder(phi.getParent(), DMSIRBuilder::BEGINNING, &added_insts);
      for (auto& tuple : incoming_binfos) {
        const BoundsInfo binfo = std::get<1>(tuple);
        merged_binfo = BoundsInfo::merge(merged_binfo, binfo, &phi, PostPhiBuilder);
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
    if (IAC.CNI.kind == CallNameInfo::NAMEDCALL) {
      if (IAC.CNI.name == "__ctype_b_loc") {
        // special-case calls of __ctype_b_loc(), we know it returns a pointer
        // to an array of 384 pointers (specifically i16*).
        // See https://stackoverflow.com/questions/37702434/ctype-b-loc-what-is-its-purpose
        LLVMContext& ctx = call.getContext();
        uint64_t pointer_size_bytes = DL.getTypeStoreSize(Type::getInt16PtrTy(ctx)).getFixedSize();
        map[&call] = BoundsInfo::static_bounds(0, 384*pointer_size_bytes - 1);
        return;
      }
    }
    // If we get here, we don't have any special information about the bounds of
    // the returned pointer. For now, we'll just mark UNKNOWN.
    // TODO: better interprocedural way to get bounds info
    map[&call] = BoundsInfo::unknown();
  }
}
