#include "llvm/Transforms/Utils/DMS_BoundsInfos.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

#define DEBUG_TYPE "DMS-bounds-info"

const APInt zero = APInt(/* bits = */ 64, /* val = */ 0);
const APInt minusone = APInt(/* bits = */ 64, /* val = */ -1);

BoundsInfos::BoundsInfos(
  Function& F,
  const DataLayout& DL,
  DenseSet<const Instruction*>& added_insts,
  DenseMap<const Value*, SmallDenseSet<const Value*, 4>>& pointer_aliases
) :
  notdefinedyet_binfo(BoundsInfo::notdefinedyet()),
  unknown_binfo(BoundsInfo::unknown()),
  infinite_binfo(BoundsInfo::infinite()),
  DL(DL), added_insts(added_insts), pointer_aliases(pointer_aliases),
  runtime_stack_slots(F, added_insts)
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
      DMSIRBuilder Builder(forloopbody, DMSIRBuilder::PHIBEGINNING, &added_insts);
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
          BoundsInfo::dynamic_bounds_for_global_array(gv, F, added_insts, runtime_stack_slots)
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

/// Call this (at least) once for each source _module_ (not function).
///
/// Returns `false` if it definitely did not make any changes, or `true` if it
/// did (or may have).
bool BoundsInfos::module_initialization(Module& mod) {
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
    return false;
  }

  // This `new_func` does the initialization
  FunctionType* FuncTy = FunctionType::get(Type::getVoidTy(mod.getContext()), {}, false);
  Function* new_func = cast<Function>(mod.getOrInsertFunction("__DMS_bounds_initialization", FuncTy).getCallee());
  new_func->setLinkage(GlobalValue::PrivateLinkage);
  BasicBlock* EntryBlock = BasicBlock::Create(mod.getContext(), "entry", new_func);
  // the added_insts and pointer_aliases aren't important for the
  // __DMS_bounds_initialization function, so we just have some here that we'll
  // throw away when we're done
  DenseSet<const Instruction*> added_insts;
  DenseMap<const Value*, SmallDenseSet<const Value*, 4>> pointer_aliases;
  DMSIRBuilder Builder(EntryBlock, DMSIRBuilder::BEGINNING, &added_insts);
  BoundsInfos binfos(*new_func, mod.getDataLayout(), added_insts, pointer_aliases);
  for (GlobalObject& gobj : mod.global_objects()) {
    if (GlobalVariable* gv = dyn_cast<GlobalVariable>(&gobj)) {
      // don't do anything for the special variables llvm.global_ctors and
      // llvm.global_dtors
      if (gv->hasName() && (gv->getName() == "llvm.global_ctors" || gv->getName() == "llvm.global_dtors")) {
        continue;
      }
      PointerType* gv_ptr_type = cast<PointerType>(gv->getType());
      Type* gv_type = gv_ptr_type->getElementType();
      if (gv_type->isSized()) {
        // store the global's size in the special table in case some other
        // translation unit needs it via __dms_get_globalarraysize(); see notes
        // there
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
  return true;
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
  assert(ptr->getType()->isPtrOrPtrVectorTy());
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
        case Instruction::Select: {
          // merger of the bounds for the two constant pointers we're selecting between.
          // we assume that one of them is actually or functionally NULL, so we
          // can assume we don't need to insert dynamic instructions for this
          // merger
          BoundsInfo* A = get_binfo(expr->getOperand(1));
          BoundsInfo* B = get_binfo(expr->getOperand(2));
          mark_as_merged(expr, *A, *B, NULL);
          return get_binfo_noalias(expr);
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
          if (BoundsInfo* binfo = try_get_binfo_for_const_int(expr->getOperand(0))) {
            return binfo;
          } else {
            // for other IntToPtrs, ideally, we have alias information, and some
            // alias will have bounds info
            return &notdefinedyet_binfo;
          }
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

/// Try to get bounds info for the given `Constant` of integer type.
///
/// BoundsInfo only makes sense for a pointer, but this will still try to do the
/// right thing interpreting the const int as a pointer value.
///
/// For instance, this can return sensible bounds for constant 0 (which is just
/// NULL), or for integers which are somehow eventually derived from a pointer
/// via PtrToInt.
///
/// However, this only recognizes a few patterns, so in other cases where it's
/// not sure, it will return NULL.
BoundsInfo* BoundsInfos::try_get_binfo_for_const_int(const Constant* c) {
  if (const ConstantInt* cint = dyn_cast<ConstantInt>(c)) {
    // if the integer we're treating as a pointer is zero, or any other number <
    // 4K (corresponding to the first page of memory, which is unmapped), we can
    // treat it as infinite bounds, just like the null pointer
    if (cint->getValue().ult(4*1024)) {
      return &infinite_binfo;
    } else {
      // any other constant number, UNKNOWN
      return &unknown_binfo;
    }
  } else if (const ConstantExpr* cexpr = dyn_cast<ConstantExpr>(c)) {
    // the integer we're treating as a pointer is another constant expression, of int type.
    switch (cexpr->getOpcode()) {
      case Instruction::PtrToInt: {
        // derived from a PtrToInt ... just use the binfo of the orig ptr
        return get_binfo(cexpr->getOperand(0));
      }
      case Instruction::SExt:
      case Instruction::ZExt:
      {
        // derived from a SExt or ZExt. Recurse
        return try_get_binfo_for_const_int(cexpr->getOperand(0));
      }
      case Instruction::Select: {
        // derived from a Select of two other constant integers. Recurse.
        BoundsInfo* A = try_get_binfo_for_const_int(cexpr->getOperand(1));
        BoundsInfo* B = try_get_binfo_for_const_int(cexpr->getOperand(2));
        if (A && B) {
          // we assume that one of them is actually or functionally NULL, so we
          // can assume we don't need to insert dynamic instructions for this
          // merger
          mark_as_merged(cexpr, *A, *B, NULL);
          return get_binfo_noalias(cexpr);
        } else {
          // couldn't get bounds for one or both of A or B, so can't get bounds
          // for the Select
          return NULL;
        }
      }
      default:
        // anything else is not a pattern we handle here
        return NULL;
    }
  } else {
    // anything else is not a pattern we handle here
    return NULL;
  }
}

/// Mark the given pointer as having the merger of the two given bounds
/// information.
///
/// `A` and `B` should have lifetime equal to the lifetime of this BoundsInfos.
///
/// `Builder` is the DMSIRBuilder to use to insert dynamic instructions, if
/// that is necessary.
/// Passing NULL for `Builder` is allowed if you know that at least one of A
/// or B has "trivial" bounds -- e.g., Unknown or Infinite.
void BoundsInfos::mark_as_merged(
  const Value* ptr,
  BoundsInfo& A,
  BoundsInfo& B,
  DMSIRBuilder* Builder
) {
  if (A.is_notdefinedyet()) return mark_as(ptr, &B);
  if (B.is_notdefinedyet()) return mark_as(ptr, &A);
  if (A.is_unknown()) return mark_as(ptr, &A);
  if (B.is_unknown()) return mark_as(ptr, &B);
  if (A.is_infinite()) return mark_as(ptr, &B);
  if (B.is_infinite()) return mark_as(ptr, &A);

  if (A == B) return mark_as(ptr, &A); // this also avoids forcing, if both A and B are still dynamic-lazy but equivalent

  // if we reach this point, Builder must not be NULL. Caller is responsible for this
  assert(Builder);

  // four cases remain: static-static, static-dynamic, dynamic-static, dynamic-dynamic
  if (BoundsInfo::Static* A_sinfo = std::get_if<BoundsInfo::Static>(&A.data)) {
    BoundsInfo::Dynamic A_dinfo = BoundsInfo::Dynamic::from_static(*A_sinfo, ptr, *Builder);
    if (BoundsInfo::Static* B_sinfo = std::get_if<BoundsInfo::Static>(&B.data)) {
      BoundsInfo::Dynamic B_dinfo = BoundsInfo::Dynamic::from_static(*B_sinfo, ptr, *Builder);
      mark_as(ptr, BoundsInfo(std::move(
        BoundsInfo::Dynamic::merge(A_dinfo, B_dinfo, *Builder)
      )));
    } else if (BoundsInfo::Dynamic* B_dinfo = std::get_if<BoundsInfo::Dynamic>(&B.data)) {
      mark_as(ptr, BoundsInfo(std::move(
        BoundsInfo::Dynamic::merge(A_dinfo, *B_dinfo, *Builder)
      )));
    } else {
      llvm_unreachable("Missing BoundsInfo case");
    }
  } else if (BoundsInfo::Dynamic* A_dinfo = std::get_if<BoundsInfo::Dynamic>(&A.data)) {
    if (BoundsInfo::Static* B_sinfo = std::get_if<BoundsInfo::Static>(&B.data)) {
      BoundsInfo::Dynamic B_dinfo = BoundsInfo::Dynamic::from_static(*B_sinfo, ptr, *Builder);
      mark_as(ptr, BoundsInfo(std::move(
        BoundsInfo::Dynamic::merge(*A_dinfo, B_dinfo, *Builder)
      )));
    } else if (BoundsInfo::Dynamic* B_dinfo = std::get_if<BoundsInfo::Dynamic>(&B.data)) {
      mark_as(ptr, BoundsInfo(std::move(
        BoundsInfo::Dynamic::merge(*A_dinfo, *B_dinfo, *Builder)
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
    mark_as_merged(&select, *binfo1, *binfo2, &AfterSelect);
    // update the cache for the future
    cache[Key] = get_binfo(&select);
  }
}

void BoundsInfos::propagate_bounds(IntToPtrInst& inttoptr, const PointerStatus inttoptr_status) {
  // if we're considering it a clean ptr, then also assume it is valid for the
  // entire size of the data its type claims it points to
  if (inttoptr_status.is_clean()) {
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
    added_insts,
    runtime_stack_slots
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
      DMSIRBuilder PhiBuilder(phi.getParent(), DMSIRBuilder::PHIBEGINNING, &added_insts);
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
      DMSIRBuilder PhiBuilder(phi.getParent(), DMSIRBuilder::PHIBEGINNING, &added_insts);
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
      mark_as_merged(&phi, *map.lookup(&phi), *incoming.binfo, &PostPhiBuilder);
    }
  }
}

static SmallVector<int64_t> get_pointer_offsets(StructType* struct_ty, const DataLayout& DL, DMSIRBuilder& Builder);
static SmallVector<int64_t> get_pointer_offsets(ArrayType* array_ty, const DataLayout& DL, DMSIRBuilder& Builder);

/// private helper function:
///
/// return a SmallVector containing all of the (byte) offsets within the struct
/// type at which pointers are stored
static SmallVector<int64_t> get_pointer_offsets(StructType* struct_ty, const DataLayout& DL, DMSIRBuilder& Builder) {
  SmallVector<int64_t> results;
  for (unsigned i = 0; i < struct_ty->getNumElements(); i++) {
    Type* struct_el_type = struct_ty->getElementType(i);
    if (struct_el_type->isIntegerTy() || struct_el_type->isFloatingPointTy()) {
      // assume it's non-pointer data. Nothing to do.
    } else if (struct_el_type->isPointerTy()) {
      results.push_back(DL.getIndexedOffsetInType(struct_ty, {Builder.getInt64(i)}));
    } else if (struct_el_type->isStructTy()) {
      int64_t offset_of_struct = DL.getIndexedOffsetInType(struct_ty, {Builder.getInt64(i)});
      SmallVector<int64_t> inner_offsets = get_pointer_offsets(cast<StructType>(struct_el_type), DL, Builder);
      for (int64_t inner_offset : inner_offsets) {
        results.push_back(offset_of_struct + inner_offset);
      }
    } else if (struct_el_type->isArrayTy()) {
      int64_t offset_of_array = DL.getIndexedOffsetInType(struct_ty, {Builder.getInt64(i)});
      SmallVector<int64_t> inner_offsets = get_pointer_offsets(cast<ArrayType>(struct_el_type), DL, Builder);
      for (int64_t inner_offset : inner_offsets) {
        results.push_back(offset_of_array + inner_offset);
      }
    } else {
      errs() << "get_pointer_offsets: unhandled struct element type:\n";
      struct_el_type->dump();
      llvm_unreachable("add handling for this struct element type");
    }
  }
  return results;
}

/// private helper function:
///
/// return a SmallVector containing all of the (byte) offsets within the array
/// type at which pointers are stored
static SmallVector<int64_t> get_pointer_offsets(ArrayType* array_ty, const DataLayout& DL, DMSIRBuilder& Builder) {
  SmallVector<int64_t> results;
  Type* el_type = array_ty->getElementType();
  if (el_type->isIntegerTy() || el_type->isFloatingPointTy()) {
    // assume it's non-pointer data. Nothing to do.
  } else if (el_type->isPointerTy()) {
    for (unsigned i = 0; i < array_ty->getNumElements(); i++) {
      results.push_back(i * DL.getTypeStoreSize(el_type));
    }
  } else if (el_type->isStructTy()) {
    SmallVector<int64_t> struct_offsets = get_pointer_offsets(cast<StructType>(el_type), DL, Builder);
    for (unsigned i = 0; i < array_ty->getNumElements(); i++) {
      for (int64_t struct_offset : struct_offsets) {
        results.push_back(i * DL.getTypeStoreSize(el_type) + struct_offset);
      }
    }
  } else if (el_type->isArrayTy()) {
    SmallVector<int64_t> array_offsets = get_pointer_offsets(cast<ArrayType>(el_type), DL, Builder);
    for (unsigned i = 0; i < array_ty->getNumElements(); i++) {
      for (int64_t array_offset : array_offsets) {
        results.push_back(i * DL.getTypeStoreSize(el_type) + array_offset);
      }
    }
  } else {
    errs() << "get_pointer_offsets: unhandled array element type:\n";
    el_type->dump();
    llvm_unreachable("add handling for this array element type");
  }
  return results;
}

void BoundsInfos::propagate_bounds(CallBase& call, const IsAllocatingCall& IAC) {
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
    // not an allocation call
    if (IAC.CNI.kind == CallNameInfo::NAMEDCALL) {
      if (IAC.CNI.name == "__ctype_b_loc") {
        // special-case calls of __ctype_b_loc(), we know it returns a valid pointer
        // See https://stackoverflow.com/questions/37702434/ctype-b-loc-what-is-its-purpose
        LLVMContext& ctx = call.getContext();
        uint64_t pointer_size_bytes = DL.getTypeStoreSize(Type::getInt16PtrTy(ctx)).getFixedSize();
        mark_as(&call, BoundsInfo::static_bounds(0, pointer_size_bytes - 1));
        return;
      } else if (IAC.CNI.name.startswith("llvm.memcpy") || IAC.CNI.name.startswith("llvm.memmove")) {
        // memcpy or memmove: if we're copying pointers stored in memory we also
        // need to copy the metadata entries in the global table, so that the
        // new pointer locations are valid keys
        // the main question is how do we know whether we're copying pointers
        // (and if so, at what addresses)
        // memcpy and memmove src and dest arguments are always i8*, but let's
        // check how the src i8* was produced
        Value* src = call.getArgOperand(1);
        Value* dst = call.getArgOperand(0);
        Value* len_bytes = call.getArgOperand(2);
        if (const BitCastInst* bitcast = dyn_cast<BitCastInst>(src)) {
          // src was bitcasted from another pointer (LLVM doesn't allow
          // bitcasting from int to ptr, that would have to be IntToPtr).
          // let's check the type of that pointer
          Type* src_ptr_ty = bitcast->getOperand(0)->getType();
          assert(src_ptr_ty->isPointerTy());
          Type* src_elem_ty = cast<PointerType>(src_ptr_ty)->getElementType();
          if (src_elem_ty->isIntegerTy() || src_elem_ty->isFloatingPointTy()) {
            // our src ptr was bitcasted from a type like i64*. let's assume
            // we're just copying non-pointer data. Nothing to do.
            return;
          } else if (src_elem_ty->isPointerTy()) {
            // our src ptr was bitcasted from a pointer type like i8**.
            // assume that every element of the memcpy is a pointer.
            // we need to propagate bounds metadata in the global table for
            // every pointer in the memcpy.
            DMSIRBuilder Builder(&call, DMSIRBuilder::AFTER, &added_insts);
            Value* pointer_size_bytes = Builder.getInt64(DL.getPointerSize());
            call_dms_copy_bounds_in_interval(src, dst, len_bytes, pointer_size_bytes, Builder);
            // nothing else we need to do (memcpy and memmove return void)
            return;
          } else if (src_elem_ty->isStructTy()) {
            StructType* struct_ty = cast<StructType>(src_elem_ty);
            auto struct_size_bytes = DL.getTypeAllocSize(struct_ty);
            DMSIRBuilder Builder(&call, DMSIRBuilder::AFTER, &added_insts);
            // our src ptr was bitcasted from struct_ty*.
            // if struct_ty contains pointers, we need to propagate bounds
            for (int64_t offset : get_pointer_offsets(struct_ty, DL, Builder)) {
              Value* first_ptr_addr_src = Builder.add_offset_to_ptr(src, Builder.getInt64(offset));
              Value* first_ptr_addr_dst = Builder.add_offset_to_ptr(dst, Builder.getInt64(offset));
              call_dms_copy_bounds_in_interval(
                first_ptr_addr_src,
                first_ptr_addr_dst,
                Builder.CreateSub(len_bytes, Builder.getInt64(offset)),
                Builder.getInt64(struct_size_bytes),
                Builder
              );
            }
            return;
          } else if (src_elem_ty->isArrayTy()) {
            ArrayType* array_ty = cast<ArrayType>(src_elem_ty);
            Type* array_elem_ty = array_ty->getElementType();
            if (array_elem_ty->isIntegerTy() || array_elem_ty->isFloatingPointTy()) {
              // our src ptr was bitcasted from a type like [5 x i64]*. let's
              // assume we're just copying non-pointer data. Nothing to do.
              return;
            } else if (array_elem_ty->isPointerTy()) {
              // our src ptr was bitcasted from a type like [5 x i8*]*. assume
              // that every element of the memcpy is a pointer, and propagate
              // bounds metadata in the global table just like we do for i8**.
              DMSIRBuilder Builder(&call, DMSIRBuilder::AFTER, &added_insts);
              Value* pointer_size_bytes = Builder.getInt64(DL.getPointerSize());
              call_dms_copy_bounds_in_interval(src, dst, len_bytes, pointer_size_bytes, Builder);
              return;
            } else if (array_elem_ty->isStructTy()) {
              DMSIRBuilder Builder(&call, DMSIRBuilder::AFTER, &added_insts);
              // our src ptr was bitcasted from a type like [5 x struct_ty]*.
              // (yes, we've seen a type like this in the wild in SPEC.)
              // treat this just like struct_ty*
              StructType* struct_ty = cast<StructType>(array_elem_ty);
              auto struct_size_bytes = DL.getTypeAllocSize(struct_ty);
              for (int64_t offset : get_pointer_offsets(struct_ty, DL, Builder)) {
                Value* first_ptr_addr_src = Builder.add_offset_to_ptr(src, Builder.getInt64(offset));
                Value* first_ptr_addr_dst = Builder.add_offset_to_ptr(dst, Builder.getInt64(offset));
                call_dms_copy_bounds_in_interval(
                  first_ptr_addr_src,
                  first_ptr_addr_dst,
                  Builder.CreateSub(len_bytes, Builder.getInt64(offset)),
                  Builder.getInt64(struct_size_bytes),
                  Builder
                );
              }
              return;
            } else {
              errs() << "LLVM memcpy or memmove src is bitcast from an unhandled array type:\n";
              array_ty->dump();
              llvm_unreachable("add handling for this memcpy or memmove src");
              return;
            }
          } else {
            errs() << "LLVM memcpy or memmove src is bitcast from an unhandled type:\n";
            src_ptr_ty->dump();
            llvm_unreachable("add handling for this memcpy or memmove src");
            return;
          }
        } else if (isa<IntToPtrInst>(src) || isa<GetElementPtrInst>(src) || isa<CallBase>(src)) {
          // memcpy src is an i8* directly produced from IntToPtr, GEP, or call.
          // assume this is a "native" i8*, and treat it like a memcpy of a
          // bunch of i8s -- i.e., we're just copying non-pointer data; nothing
          // to do.
          return;
        } else if (isa<Constant>(src)) {
          // memcpy src is an i8* that is a compile-time constant (possibly
          // constant expression).
          // For now, we will again assume this is a "native" i8*, and treat it
          // like a memcpy of a bunch of i8s -- i.e., we're just copying
          // non-pointer data; nothing to do.
          return;
        } else if (isa<Argument>(src)) {
          // memcpy src is an i8* that was passed to this function.  For now, we
          // will assume that a buffer of this kind never contains pointer data;
          // nothing to do.
          return;
        } else if (isa<PHINode>(src)) {
          // memcpy src is an i8* derived from a PHI node.
          // this indicates some type of more-complicated operation, not just
          // casting the pointer to i8*/void* immediately at the memcpy call
          // site, so for now we'll assume that this is "actually" an i8* and
          // treat it like a memcpy of a bunch of i8s -- i.e., we're just
          // copying non-pointer data; nothing to do.
          return;
        } else {
          errs() << "LLVM memcpy or memmove src is of an unhandled kind:\n";
          src->dump();
          llvm_unreachable("add handling for this memcpy or memmove src");
          return;
        }
      }
    }
    // If we get here, we don't have any special information about the bounds of
    // the returned pointer. For now, we'll just mark unknown.
    // TODO: better interprocedural way to get bounds info
    mark_as(&call, &unknown_binfo);
  }
}
