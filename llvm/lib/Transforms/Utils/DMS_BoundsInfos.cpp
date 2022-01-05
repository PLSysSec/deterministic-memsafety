#include "llvm/Transforms/Utils/DMS_BoundsInfos.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

#define DEBUG_TYPE "DMS-bounds-info"

BoundsInfos::BoundsInfos(
  Function& F,
  const DataLayout& DL,
  DenseSet<const Instruction*>& added_insts,
  DenseMap<const Value*, SmallDenseSet<const Value*, 4>>& pointer_aliases
) :
  notdefinedyet_binfo(BoundsInfo::notdefinedyet()),
  unknown_binfo(BoundsInfo::unknown()),
  infinite_binfo(BoundsInfo::infinite()),
  DL(DL), added_insts(added_insts), pointer_aliases(pointer_aliases)
{
  LLVM_DEBUG(dbgs() << "Initializing bounds infos for function " << F.getNameOrAsOperand() << " with " << F.arg_size() << " operands\n");
  bool isMain = F.getName() == "main" && F.arg_size() == 2;
  if (isMain) {
    LLVM_DEBUG(dbgs() << "This is a 'main' function, setting bounds for argv\n");
    Value* argc = F.getArg(0);
    Value* argv = F.getArg(1);
    assert(argv->getType()->isPointerTy());
    assert(cast<PointerType>(argv->getType())->getElementType()->isPointerTy());
    {
      // bounds for argv: it's an array of size argc
      DMSIRBuilder Builder(&F.getEntryBlock(), DMSIRBuilder::BEGINNING, &added_insts);
      auto pointer_size_bytes = DL.getTypeStoreSize(Builder.getInt8PtrTy()).getFixedSize();
      Value* argvMax = Builder.add_offset_to_ptr(argv,
        /* argc * pointer_size_in_bytes - 1 */
        Builder.CreateSub(
          Builder.CreateMul(argc, ConstantInt::get(argc->getType(), pointer_size_bytes)),
          ConstantInt::get(argc->getType(), 1, /* signed = */ true),
          "argvMax"
        )
      );
      mark_as(argv, BoundsInfo::dynamic_bounds(argv, argvMax));
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
      loopindex->addIncoming(Builder.getInt32(0), &Entry);
      Value* stringptr = Builder.CreateGEP(Builder.getInt8PtrTy(), argv, loopindex);
      call_dms_store_infinite_bounds(stringptr, Builder);
      Value* incd_loopindex = Builder.CreateAdd(loopindex, Builder.getInt32(1));
      loopindex->addIncoming(incd_loopindex, forloopbody);
      Value* cond = Builder.CreateICmpULT(incd_loopindex, argc);
      Builder.insertCondJumpTo(cond, forloopbody);
    }
  } else {
    // For now, if any function parameters are pointers, mark their bounds info
    // as unknown()
    for (const Argument& arg : F.args()) {
      if (arg.getType()->isPointerTy()) {
        mark_as(&arg, &unknown_binfo);
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
      if (allocationSize == 0) {
        // this can result from declarations like `extern int some_arr[];`.
        // we handle this by dynamically looking up the actual array size,
        // see notes on dynamic_bounds_for_global_array().
        mark_as(&gv, BoundsInfo(
          BoundsInfo::dynamic_bounds_for_global_array(gv, F, added_insts)
        ));
      } else {
        mark_as(&gv, BoundsInfo::static_bounds(
          zero, APInt(/* bits = */ 64, /* val = */ allocationSize - 1)
        ));
      }
    } else {
      mark_as(&gv, &unknown_binfo);
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
      // store the global's size in the special table in case some other
      // translation unit needs it via __dms_get_globalarraysize(); see notes
      // there
      PointerType* gv_ptr_type = cast<PointerType>(gv->getType());
      Type* gv_type = gv_ptr_type->getElementType();
      if (gv_type->isSized()) {
        auto global_size = mod.getDataLayout().getTypeStoreSize(gv_type);
        if (global_size > 0) {
          call_dms_store_globalarraysize(
            gv,
            Builder.CreateGEP(
              Builder.getInt8Ty(),
              Builder.castToCharStar(gv),
              {Builder.getInt64(global_size - 1)}
            ),
            Builder
          );
        }

        // if the initializer involves any pointer expressions, store bounds for
        // those in the normal bounds table. see comments above
        if (gv->hasInitializer()) {
          Constant* initializer = gv->getInitializer();
          binfos.store_info_for_all_ptr_exprs(gv, initializer, Builder);
        }
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
    BoundsInfo* binfo = get_binfo(gv);
    binfo->store_dynamic(addr, gv, Builder);
  } else if (c->getType()->isPointerTy() && c->isNullValue()) {
    BoundsInfo* binfo = get_binfo(c);
    binfo->store_dynamic(addr, c, Builder);
  } else if (ConstantAggregate* cagg = dyn_cast<ConstantAggregate>(c)) {
    assert(addr->getType()->isPointerTy());
    const unsigned gepIndexSizeBits = (cagg->getType()->isStructTy()) ?
      32 // apparently required, see StructType::indexValid() in Type.cpp
      : DL.getIndexSizeInBits(cast<PointerType>(addr->getType())->getAddressSpace());
    for (unsigned i = 0; i < cagg->getNumOperands(); i++) {
      Value* op = cagg->getOperand(i);
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
        BoundsInfo* binfo = get_binfo(cexpr);
        binfo->store_dynamic(addr, cexpr, Builder);
        break;
      }
      default: {
        break;
      }
    }
  }
}

/// Get the bounds information for the given pointer.
///
/// The returned pointer is never NULL, and should have lifetime equal
/// to the lifetime of this BoundsInfos.
BoundsInfo* BoundsInfos::get_binfo(const Value* ptr) {
  BoundsInfo* binfo = get_binfo_noalias(ptr);
  assert(binfo->valid());
  if (!binfo->is_notdefinedyet()) {
    LLVM_DEBUG(dbgs() << "DMS:     bounds info for " << ptr->getNameOrAsOperand() << " is " << binfo->pretty() << "\n");
    return binfo;
  }
  // if bounds info isn't defined, see if it's defined for any alias of this pointer
  for (const Value* alias : pointer_aliases[ptr]) {
    BoundsInfo* binfo = get_binfo_noalias(alias);
    assert(binfo->valid());
    if (!binfo->is_notdefinedyet()) {
      LLVM_DEBUG(dbgs() << "DMS:     bounds info for " << ptr->getNameOrAsOperand() << " is " << binfo->pretty() << ", via alias " << alias->getNameOrAsOperand() << "\n");
      return binfo;
    }
  }
  LLVM_DEBUG(dbgs() << "DMS:     bounds info for " << ptr->getNameOrAsOperand() << " is " << binfo->pretty() << "\n");
  return binfo;
}

/// Like `get_binfo()`, but doesn't check aliases of the given ptr, if any
/// exist. This is used internally by `get_binfo()`.
///
/// The returned pointer is never NULL, and should have lifetime equal
/// to the lifetime of this BoundsInfos.
BoundsInfo* BoundsInfos::get_binfo_noalias(const Value* ptr) {
  assert(ptr->getType()->isPointerTy());
  BoundsInfo* binfo = map.lookup(ptr);
  if (!binfo) {
    LLVM_DEBUG(dbgs() << "DMS:     map.lookup() returned NULL, so setting " << ptr->getNameOrAsOperand() << " to NotDefinedYet\n");
    map[ptr] = &notdefinedyet_binfo;
    binfo = &notdefinedyet_binfo;
  }
  assert(binfo->valid());
  if (!binfo->is_notdefinedyet()) return binfo;
  if (const Constant* constant = dyn_cast<const Constant>(ptr)) {
    if (constant->isNullValue()) {
      return &infinite_binfo;
    } else if (isa<UndefValue>(constant)) {
      // this includes both undef and poison
      return &infinite_binfo;
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
          BoundsInfo* ret = bounds_info_for_gep(*gepinst);
          assert(ret->valid());
          inst->deleteValue();
          return ret;
        }
        case Instruction::IntToPtr: {
          if (const ConstantInt* cint = dyn_cast<ConstantInt>(expr->getOperand(0))) {
            // if it's IntToPtr of zero, or any other number < 4K (corresponding
            // to the first page of memory, which is unmapped), we can treat it
            // as infinite bounds, just like the null pointer
            if (cint->getValue().ult(4*1024)) {
              return &infinite_binfo;
            } else {
              // IntToPtr of any other constant number, UNKNOWN
              return &unknown_binfo;
            }
          }
          // for other IntToPtrs, ideally, we have alias information, and some
          // alias will have bounds info
          return &notdefinedyet_binfo;
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
  assert(binfo->valid());
  return binfo;
}

/// Mark the given pointer as having the merger of the two given bounds
/// information.
///
/// `A` and `B` should have lifetime equal to the lifetime of this BoundsInfos.
///
/// `Builder` is the DMSIRBuilder to use to insert dynamic instructions, if
/// that is necessary.
void BoundsInfos::mark_as_merged(
  Value* ptr,
  BoundsInfo& A,
  BoundsInfo& B,
  DMSIRBuilder& Builder
) {
  if (A.is_notdefinedyet()) return mark_as(ptr, &B);
  if (B.is_notdefinedyet()) return mark_as(ptr, &A);
  if (A.is_unknown()) return mark_as(ptr, &A);
  if (B.is_unknown()) return mark_as(ptr, &B);
  if (A.is_infinite()) return mark_as(ptr, &B);
  if (B.is_infinite()) return mark_as(ptr, &A);

  if (A == B) return mark_as(ptr, &A); // this also avoids forcing, if both A and B are still dynamic-lazy but equivalent

  // four cases remain: static-static, static-dynamic, dynamic-static, dynamic-dynamic
  if (BoundsInfo::Static* A_sinfo = std::get_if<BoundsInfo::Static>(&A.data)) {
    BoundsInfo::Dynamic A_dinfo = BoundsInfo::Dynamic::from_static(*A_sinfo, ptr, Builder);
    if (BoundsInfo::Static* B_sinfo = std::get_if<BoundsInfo::Static>(&B.data)) {
      BoundsInfo::Dynamic B_dinfo = BoundsInfo::Dynamic::from_static(*B_sinfo, ptr, Builder);
      mark_as(ptr, BoundsInfo(std::move(
        BoundsInfo::Dynamic::merge(A_dinfo, B_dinfo, Builder)
      )));
    } else if (BoundsInfo::Dynamic* B_dinfo = std::get_if<BoundsInfo::Dynamic>(&B.data)) {
      mark_as(ptr, BoundsInfo(std::move(
        BoundsInfo::Dynamic::merge(A_dinfo, *B_dinfo, Builder)
      )));
    } else {
      llvm_unreachable("Missing BoundsInfo case");
    }
  } else if (BoundsInfo::Dynamic* A_dinfo = std::get_if<BoundsInfo::Dynamic>(&A.data)) {
    if (BoundsInfo::Static* B_sinfo = std::get_if<BoundsInfo::Static>(&B.data)) {
      BoundsInfo::Dynamic B_dinfo = BoundsInfo::Dynamic::from_static(*B_sinfo, ptr, Builder);
      mark_as(ptr, BoundsInfo(std::move(
        BoundsInfo::Dynamic::merge(*A_dinfo, B_dinfo, Builder)
      )));
    } else if (BoundsInfo::Dynamic* B_dinfo = std::get_if<BoundsInfo::Dynamic>(&B.data)) {
      mark_as(ptr, BoundsInfo(std::move(
        BoundsInfo::Dynamic::merge(*A_dinfo, *B_dinfo, Builder)
      )));
    } else {
      llvm_unreachable("Missing BoundsInfo case");
    }
  } else {
    llvm_unreachable("Missing BoundsInfo case");
  }
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
  BoundsInfo* binfo = get_binfo(storedVal);
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
    Instruction* new_bounds_call = binfo->store_dynamic(store.getPointerOperand(), storedVal, Builder);
    store_bounds_calls[&store] = BoundsStoringCall(new_bounds_call, binfo);
  }
}

/// Propagate bounds information for an Alloca instruction.
void BoundsInfos::propagate_bounds(AllocaInst& alloca) {
  // we know the bounds of the allocation statically
  PointerType* resultType = cast<PointerType>(alloca.getType());
  auto allocationSize = DL.getTypeStoreSize(resultType->getElementType()).getFixedSize();
  mark_as(&alloca, BoundsInfo::static_bounds(
    zero, APInt(/* bits = */ 64, /* val = */ allocationSize - 1)
  ));
}

/// Copy the bounds for the input pointer (must be operand 0) to the output
/// pointer
void BoundsInfos::propagate_bounds_id(Instruction& inst) {
  Value* input_ptr = inst.getOperand(0);
  assert(input_ptr->getType()->isPointerTy());
  mark_as(&inst, get_binfo(input_ptr));
}

/// Get bounds info for the given GEP.
///
/// The returned pointer is never NULL, and should have lifetime equal
/// to the lifetime of this BoundsInfos.
BoundsInfo* BoundsInfos::bounds_info_for_gep(GetElementPtrInst& gep) {
  // the pointer resulting from the GEP still gets access to the whole allocation,
  // ie the same access that the GEP's input pointer had
  Value* input_ptr = gep.getPointerOperand();
  BoundsInfo* binfo = get_binfo(input_ptr);
  if (binfo->is_notdefinedyet()) {
    llvm_unreachable("GEP input ptr's BoundsInfo should be defined (at least Unknown)");
  } else if (binfo->is_unknown()) {
    LLVM_DEBUG(dbgs() << "GEP input ptr has unknown bounds\n");
    return binfo;
  } else if (binfo->is_infinite()) {
    return binfo;
  } else if (binfo->is_static()) {
    const BoundsInfo::Static& static_info = std::get<BoundsInfo::Static>(binfo->data);
    const std::optional<APInt> constant_offset = computeGEPOffset(gep, DL);
    if (constant_offset.has_value()) {
      mark_as(&gep, BoundsInfo::static_bounds(
        static_info.low_offset - *constant_offset,
        static_info.high_offset - *constant_offset
      ));
      return map[&gep];
    } else {
      // bounds of the new pointer aren't known statically
      // and actually, we don't care what the dynamic GEP offset is:
      // it doesn't change the `base` and `max` of the allocation

      // `base` is `input_ptr` plus the input pointer's low_offset
      const BoundsInfo::PointerWithOffset base = BoundsInfo::PointerWithOffset(input_ptr, static_info.low_offset);
      // `max` is `input_ptr` plus the input pointer's high_offset
      const BoundsInfo::PointerWithOffset max = BoundsInfo::PointerWithOffset(input_ptr, static_info.high_offset);
      mark_as(&gep, BoundsInfo::dynamic_bounds(base, max));
      return map[&gep];
    }
  } else if (binfo->is_dynamic()) {
    // regardless of the GEP offset, the `base` and `max` don't change
    return binfo;
  } else {
    assert(binfo->valid());
    llvm_unreachable("Missing BoundsInfo case");
  }
}

/// Propagate bounds information for a GEP instruction.
void BoundsInfos::propagate_bounds(GetElementPtrInst& gep) {
  mark_as(&gep, bounds_info_for_gep(gep));
}

/// Used only for the cache in `propagate_bounds(Select&)`; see notes there
struct SelectCacheKey {
  SelectInst* select;
  BoundsInfo* binfo1;
  BoundsInfo* binfo2;

  explicit SelectCacheKey(SelectInst* select, BoundsInfo* binfo1, BoundsInfo* binfo2)
    : select(select), binfo1(binfo1), binfo2(binfo2) {}
  SelectCacheKey() : select(NULL), binfo1(NULL), binfo2(NULL) {}

  bool operator==(const SelectCacheKey& other) const {
    // compare as pointers, not deep compare, if this causes a cache miss that
    // wasn't technically necessary that's fine
    return select == other.select && binfo1 == other.binfo1 && binfo2 == other.binfo2;
  }
  bool operator!=(const SelectCacheKey& other) const {
    return !(*this == other);
  }
};

// it seems this is required in order for SelectCacheKey to be a key type
// in a DenseMap
namespace llvm {
template<> struct DenseMapInfo<SelectCacheKey> {
  static inline SelectCacheKey getEmptyKey() {
    return SelectCacheKey();
  }
  static inline SelectCacheKey getTombstoneKey() {
    return SelectCacheKey(
      DenseMapInfo<SelectInst*>::getTombstoneKey(),
      DenseMapInfo<BoundsInfo*>::getTombstoneKey(),
      DenseMapInfo<BoundsInfo*>::getTombstoneKey()
    );
  }
  static unsigned getHashValue(const SelectCacheKey &Val) {
    return DenseMapInfo<SelectInst*>::getHashValue(Val.select) ^
      DenseMapInfo<BoundsInfo*>::getHashValue(Val.binfo1) ^
      DenseMapInfo<BoundsInfo*>::getHashValue(Val.binfo2);
  }
  static bool isEqual(const SelectCacheKey &LHS, const SelectCacheKey &RHS) {
    return LHS == RHS;
  }
};
} // end namespace llvm

void BoundsInfos::propagate_bounds(SelectInst& select) {
  // if we aren't selecting a pointer, we have nothing to do
  if (!select.getType()->isPointerTy()) return;
  BoundsInfo* binfo1 = get_binfo(select.getTrueValue());
  BoundsInfo* binfo2 = get_binfo(select.getFalseValue());

  // cache from cache key to the resulting BoundsInfo for the SelectInst
  static DenseMap<SelectCacheKey, BoundsInfo*> cache;
  SelectCacheKey Key(&select, binfo1, binfo2);
  bool have_cache_entry = cache.count(Key) > 0;
  if (have_cache_entry) {
    mark_as(&select, cache[Key]);
  } else {
    // the merge may need to insert instructions that use the final
    // value of the select, so we need a builder pointing after the
    // select
    DMSIRBuilder AfterSelect(&select, DMSIRBuilder::AFTER, &added_insts);
    mark_as_merged(&select, *binfo1, *binfo2, AfterSelect);
    // update the cache for the future
    cache[Key] = get_binfo(&select);
  }
}

void BoundsInfos::propagate_bounds(IntToPtrInst& inttoptr, PointerKind inttoptr_kind) {
  // if we're considering it a clean ptr, then also assume it is valid for the
  // entire size of the data its type claims it points to
  if (inttoptr_kind == PointerKind::CLEAN) {
    PointerType* resultType = cast<PointerType>(inttoptr.getType());
    if (resultType->getElementType()->isSized()) {
      auto allocationSize = DL.getTypeStoreSize(resultType->getElementType()).getFixedSize();
      mark_as(&inttoptr, BoundsInfo::static_bounds(
        zero, APInt(/* bits = */ 64, /* val = */ allocationSize - 1)
      ));
    } else {
      mark_as(&inttoptr, &unknown_binfo);
    }
  } else {
    mark_as(&inttoptr, &unknown_binfo);
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
  BoundsInfo::Dynamic dyninfo = BoundsInfo::dynamic_bounds_for_ptr(
    load.getPointerOperand(),
    loaded_ptr,
    added_insts
  );
  mark_as(&load, BoundsInfo(std::move(dyninfo)));
}

void BoundsInfos::propagate_bounds(PHINode& phi) {
  // if the PHI isn't choosing between pointers, we have nothing to do
  if (!phi.getType()->isPointerTy()) return;
  struct Incoming {
    /// Pointer value
    Value* ptr;
    /// Bounds info for that pointer value
    BoundsInfo* binfo;
    /// Block that pointer is coming from
    BasicBlock* bb;
  };
  SmallVector<Incoming, 4> incoming_binfos;
  for (const Use& use : phi.incoming_values()) {
    Value* value = use.get();
    incoming_binfos.push_back(Incoming {
      value,
      get_binfo(value),
      phi.getIncomingBlock(use)
    });
  }
  BoundsInfo* prev_iteration_binfo = get_binfo(&phi);
  assert(incoming_binfos.size() >= 1);
  bool any_incoming_bounds_are_dynamic = false;
  bool any_incoming_bounds_are_unknown = false;
  for (auto& incoming : incoming_binfos) {
    if (incoming.binfo->is_dynamic()) {
      any_incoming_bounds_are_dynamic = true;
    }
    if (incoming.binfo->is_unknown()) {
      any_incoming_bounds_are_unknown = true;
    }
  }
  if (any_incoming_bounds_are_unknown) {
    mark_as(&phi, &unknown_binfo);
  } else if (any_incoming_bounds_are_dynamic) {
    // in this case, we'll use PHIs to select the proper dynamic
    // `base` and `max`, much as we used PHIs for the dynamic_kind
    // above.
    // Of course, if we already inserted PHIs in a previous iteration,
    // let's not insert them again.
    PHINode* base_phi = NULL;
    PHINode* max_phi = NULL;
    if (const BoundsInfo::Dynamic* prev_iteration_dyninfo = std::get_if<BoundsInfo::Dynamic>(&prev_iteration_binfo->data)) {
      if ((base_phi = dyn_cast<PHINode>(prev_iteration_dyninfo->getBase().ptr))) {
        assert(prev_iteration_dyninfo->getBase().offset == 0);
        assert(incoming_binfos.size() == base_phi->getNumIncomingValues());
        for (const Incoming& incoming : incoming_binfos) {
          // if dynamic instructions are necessary to compute phi
          // incoming value, insert them at the end of the
          // corresponding block, not here
          DMSIRBuilder IncomingBlockBuilder(incoming.bb, DMSIRBuilder::END, &added_insts);
          Value* incoming_base;
          if (incoming.binfo->is_notdefinedyet()) {
            // for now, treat this input to the phi as infinite bounds.
            // this will be updated on a future iteration if necessary, once the
            // incoming pointer has defined bounds.
            // We use infinite() instead of unknown() because with unknown(), we
            // force all dependent pointers to be unknown(), and this could
            // force this phi input to be unknown() in the next iteration and
            // result in a false fixpoint where everything is unknown().
            // Instead, infinite() allows us to compute the
            // most-permissive-possible bounds for this phi input for the next
            // iteration.
            incoming_base = BoundsInfo::infinite().base_as_llvm_value(incoming.ptr, IncomingBlockBuilder);
          } else {
            incoming_base = incoming.binfo->base_as_llvm_value(incoming.ptr, IncomingBlockBuilder);
          }
          assert(incoming_base);
          const Value* old_base = base_phi->getIncomingValueForBlock(incoming.bb);
          if (incoming_base != old_base) {
            base_phi->setIncomingValueForBlock(incoming.bb, incoming_base);
          }
        }
      } else {
        // prev_iteration base was not a phi. we'll have to insert a fresh phi
      }
      if ((max_phi = dyn_cast<PHINode>(prev_iteration_dyninfo->getMax().ptr))) {
        assert(prev_iteration_dyninfo->getMax().offset == 0);
        assert(incoming_binfos.size() == max_phi->getNumIncomingValues());
        for (const Incoming& incoming : incoming_binfos) {
          // if dynamic instructions are necessary to compute phi
          // incoming value, insert them at the end of the
          // corresponding block, not here
          DMSIRBuilder IncomingBlockBuilder(incoming.bb, DMSIRBuilder::END, &added_insts);
          Value* incoming_max;
          if (incoming.binfo->is_notdefinedyet()) {
            // see notes above for the base_phi case
            incoming_max = BoundsInfo::infinite().max_as_llvm_value(incoming.ptr, IncomingBlockBuilder);
          } else {
            incoming_max = incoming.binfo->max_as_llvm_value(incoming.ptr, IncomingBlockBuilder);
          }
          assert(incoming_max);
          const Value* old_max = max_phi->getIncomingValueForBlock(incoming.bb);
          if (incoming_max != old_max) {
            max_phi->setIncomingValueForBlock(incoming.bb, incoming_max);
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
      for (const Incoming& incoming : incoming_binfos) {
        // if dynamic instructions are necessary to compute phi
        // incoming value, insert them at the end of the
        // corresponding block, not here
        DMSIRBuilder IncomingBlockBuilder(incoming.bb, DMSIRBuilder::END, &added_insts);
        Value* base;
        if (incoming.binfo->is_notdefinedyet()) {
          // for now, treat this input to the phi as infinite bounds.
          // this will be updated on a future iteration if necessary, once the
          // incoming pointer has defined bounds.
          // We use infinite() instead of unknown() because with unknown(), we
          // force all dependent pointers to be unknown(), and this could force
          // this phi input to be unknown() in the next iteration and result in
          // a false fixpoint where everything is unknown().
          // Instead, infinite() allows us to compute the
          // most-permissive-possible bounds for this phi input for the next
          // iteration.
          base = BoundsInfo::infinite().base_as_llvm_value(incoming.ptr, IncomingBlockBuilder);
        } else {
          base = incoming.binfo->base_as_llvm_value(incoming.ptr, IncomingBlockBuilder);
        }
        assert(base);
        base_phi->addIncoming(base, incoming.bb);
      }
      assert(base_phi->isComplete());
    }
    if (!max_phi) {
      DMSIRBuilder PhiBuilder(phi.getParent(), DMSIRBuilder::BEGINNING, &added_insts);
      max_phi = PhiBuilder.CreatePHI(PhiBuilder.getInt8PtrTy(), phi.getNumIncomingValues(), Twine(phi.getNameOrAsOperand(), "_max"));
      added_insts.insert(max_phi);
      for (const Incoming& incoming : incoming_binfos) {
        // if dynamic instructions are necessary to compute phi
        // incoming value, insert them at the end of the
        // corresponding block, not here
        DMSIRBuilder IncomingBlockBuilder(incoming.bb, DMSIRBuilder::END, &added_insts);
        Value* max;
        if (incoming.binfo->is_notdefinedyet()) {
          // see notes above for the base_phi case
          max = BoundsInfo::infinite().max_as_llvm_value(incoming.ptr, IncomingBlockBuilder);
        } else {
          max = incoming.binfo->max_as_llvm_value(incoming.ptr, IncomingBlockBuilder);
        }
        assert(max);
        max_phi->addIncoming(max, incoming.bb);
      }
      assert(max_phi->isComplete());
    }
    mark_as(&phi, BoundsInfo::dynamic_bounds(base_phi, max_phi));
  } else {
    // no incoming bounds are dynamic. let's just merge them statically
    // start with an initial value, which we'll merge everything else into
    mark_as(&phi, &infinite_binfo);
    assert(phi.getNumIncomingValues() >= 1);
    DMSIRBuilder PostPhiBuilder(phi.getParent(), DMSIRBuilder::BEGINNING, &added_insts);
    for (const Incoming& incoming : incoming_binfos) {
      // merge each incoming bounds info into the final value
      mark_as_merged(&phi, *map.lookup(&phi), *incoming.binfo, PostPhiBuilder);
    }
  }
}

void BoundsInfos::propagate_bounds(CallBase& call, IsAllocatingCall& IAC) {
  if (IAC.allocation_bytes.has_value()) {
    if (ConstantInt* allocationSize = dyn_cast<ConstantInt>(*IAC.allocation_bytes)) {
      // allocating a constant number of bytes.
      // we know the bounds of the allocation statically.
      mark_as(&call, BoundsInfo::static_bounds(
        zero, allocationSize->getValue() - 1
      ));
    } else {
      // allocating a dynamic number of bytes.
      // We need a dynamic addition instruction to compute the upper
      // bound. Only insert that the first time -- the bounds info
      // here should not change from iteration to iteration
      if (!is_binfo_present(&call)) {
        DMSIRBuilder Builder(&call, DMSIRBuilder::AFTER, &added_insts);
        Value* callPlusBytes = Builder.add_offset_to_ptr(&call, *IAC.allocation_bytes);
        Value* max = Builder.add_offset_to_ptr(callPlusBytes, minusone);
        mark_as(&call, BoundsInfo::dynamic_bounds(&call, max));
      }
    }
  } else {
    if (IAC.CNI.kind == CallNameInfo::NAMEDCALL) {
      if (IAC.CNI.name == "__ctype_b_loc") {
        // special-case calls of __ctype_b_loc(), we know it returns a valid pointer
        // See https://stackoverflow.com/questions/37702434/ctype-b-loc-what-is-its-purpose
        LLVMContext& ctx = call.getContext();
        uint64_t pointer_size_bytes = DL.getTypeStoreSize(Type::getInt16PtrTy(ctx)).getFixedSize();
        mark_as(&call, BoundsInfo::static_bounds(0, pointer_size_bytes - 1));
        return;
      }
    }
    // If we get here, we don't have any special information about the bounds of
    // the returned pointer. For now, we'll just mark unknown.
    // TODO: better interprocedural way to get bounds info
    mark_as(&call, &unknown_binfo);
  }
}
