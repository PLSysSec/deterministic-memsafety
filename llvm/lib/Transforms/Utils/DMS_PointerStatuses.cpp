#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/DMS_classifyGEPResult.h"
#include "llvm/Transforms/Utils/DMS_PointerStatuses.h"

#include <sstream>  // ostringstream

using namespace llvm;

#define DEBUG_TYPE "DMS-pointer-statuses"

static void describePointerList(const SmallVector<const Value*, 8>& ptrs, std::ostringstream& out, StringRef desc);

// Use this for any `status` except `NotDefinedYet`
void PointerStatuses::mark_as(const Value* ptr, PointerStatus status) {
  // don't explicitly mark anything `NotDefinedYet` - we reserve
  // "not in the map" to mean `NotDefinedYet`
  assert(!status.is_notdefinedyet());
  LLVM_DEBUG(dbgs() << "DMS:     marking pointer " << ptr->getNameOrAsOperand() << " as status " << status.pretty() << "\n");
  // insert() does nothing if the key was already in the map.
  // instead, it appears we have to use operator[], which seems to
  // work whether or not `ptr` was already in the map
  map[ptr] = status;
}

/// Get the status of `ptr`. If necessary, check its aliases, and those
/// aliases' aliases, etc
PointerStatus PointerStatuses::getStatus(const Value* ptr) const {
  SmallDenseSet<const Value*, 4> tried_ptrs;
  return getStatus_checking_aliases_except(ptr, tried_ptrs);
}

/// Get the status of `ptr`. If necessary, check its aliases, and those
/// aliases' aliases, etc. However, don't recurse into any aliases listed in
/// `norecurse`. (We use this to avoid infinite recursion.)
PointerStatus PointerStatuses::getStatus_checking_aliases_except(const Value* ptr, SmallDenseSet<const Value*, 4>& norecurse) const {
  PointerStatus status = getStatus_noalias(ptr);
  if (!status.is_notdefinedyet()) return status;
  // if status isn't defined, see if it's defined for any alias of this pointer
  norecurse.insert(ptr);
  for (const Value* alias : pointer_aliases[ptr]) {
    if (norecurse.insert(alias).second) {
      status = getStatus_checking_aliases_except(alias, norecurse);
    }
    if (!status.is_notdefinedyet()) return status;
  }
  return status;
}

/// Get the status of `ptr`. This function doesn't consider aliases of `ptr`;
/// if `ptr` itself doesn't have a status, returns `PointerStatus::notdefinedyet()`.
PointerStatus PointerStatuses::getStatus_noalias(const Value* ptr) const {
  auto it = map.find(ptr);
  if (it != map.end()) {
    // found it in the map
    return it->getSecond();
  }
  // if we get here, the pointer wasn't found in the map. Is it a constant pointer?
  if (const Constant* constant = dyn_cast<Constant>(ptr)) {
    if (constant->isNullValue()) {
      // the null pointer can be considered CLEAN
      return PointerStatus::clean();
    } else if (isa<UndefValue>(constant)) {
      // undef values, which includes poison values, can be considered CLEAN
      return PointerStatus::clean();
    } else if (isa<GlobalValue>(constant)) {
      // global values can be considered CLEAN
      // usually they'll be marked CLEAN in the actual `map`, but there are
      // edge cases, such as when a PHI gets status in a predecessor block but
      // the predecessor block hasn't been processed yet (eg due to a loop)
      return PointerStatus::clean();
    } else if (const ConstantExpr* expr = dyn_cast<ConstantExpr>(constant)) {
      // it's a pointer created by a compile-time constant expression
      if (expr->isGEPWithNoNotionalOverIndexing()) {
        // this seems sufficient to consider the pointer clean, based on docs
        // of this method. GEP on a constant pointer, with constant indices,
        // that LLVM thinks are all in-bounds
        return PointerStatus::clean();
      }
      switch (expr->getOpcode()) {
        case Instruction::BitCast: {
          // bitcast doesn't change the status
          return getStatus(expr->getOperand(0));
        }
        case Instruction::Select: {
          // derived from a Select of two other constant pointers.
          // merger of their two statuses.
          PointerStatus A = getStatus(expr->getOperand(1));
          PointerStatus B = getStatus(expr->getOperand(2));
          // we assume that one of them is actually or functionally NULL, so we
          // can assume we don't need to insert dynamic instructions for this
          // merger
          return PointerStatus::merge_direct(A, B, NULL);
        }
        case Instruction::GetElementPtr: {
          // constant-GEP expression
          Instruction* inst = expr->getAsInstruction();
          GetElementPtrInst* gepinst = cast<GetElementPtrInst>(inst);
          PointerStatus ret = classifyGEPResult(*gepinst, getStatus(gepinst->getPointerOperand()), DL, trust_llvm_struct_types, NULL, added_insts).classification;
          inst->deleteValue();
          return ret;
        }
        case Instruction::IntToPtr: {
          std::optional<PointerStatus> maybe_status = tryGetStatusOfConstInt(expr->getOperand(0));
          if (maybe_status.has_value()) {
            return *maybe_status;
          } else {
            // for other IntToPtrs, ideally, we have alias information, and some
            // alias will have a status. (this comes up with constant IntToPtrs
            // introduced by our pointer encoding)
            return PointerStatus::notdefinedyet();
          }
        }
        default: {
          errs() << "unhandled constant expression:\n";
          expr->dump();
          llvm_unreachable("getting status of unhandled constant expression");
        }
      }
    } else if (const ConstantAggregate* cagg = dyn_cast<ConstantAggregate>(constant)) {
      // it's a struct, array, or vector constant
      if (Constant* el = cagg->getSplatValue(true)) {
        // all the (non-undef, non-poison) values are the same value, `el`
        // we can give this the same status as `el`
        assert(el->getType()->isPointerTy());
        return getStatus_noalias(el);
      } else {
        // a struct, array, or vector constant, with multiple different entries
        errs() << "unhandled constant struct, array, or vector:\n";
        cagg->dump();
        llvm_unreachable("getting status of constant struct, array, or vector of unhandled kind");
      }
    } else {
      // a constant, but not null, a global, or a constant expression.
      errs() << "constant pointer of unhandled kind:\n";
      constant->dump();
      llvm_unreachable("getting status of constant pointer of unhandled kind");
    }
  } else {
    // not found in map, and not a constant.
    return PointerStatus::notdefinedyet();
  }
}

/// Try to get the status of the given `Constant` of integer type.
///
/// PointerStatus only makes sense for the status of a pointer, but this will
/// still try to do the right thing interpreting the const int as a pointer
/// value.
///
/// For instance, this can return a sensible status for constant 0 (which is
/// just NULL), or for integers which are somehow eventually derived from
/// a pointer via PtrToInt.
///
/// However, this only recognizes a few patterns, so in other cases where it's
/// not sure, it just won't return a status.
std::optional<PointerStatus> PointerStatuses::tryGetStatusOfConstInt(const Constant* c) const {
  if (const ConstantInt* cint = dyn_cast<ConstantInt>(c)) {
    // if the integer we're treating as a pointer is zero, or any other number <
    // 4K (corresponding to the first page of memory, which is unmapped), we can
    // treat it as CLEAN, just like the null pointer
    if (cint->getValue().ult(4*1024)) {
      return PointerStatus::clean();
    } else {
      // IntToPtr of any other constant number, we go by inttoptr_kind
      return inttoptr_status;
    }
  } else if (const ConstantExpr* cexpr = dyn_cast<ConstantExpr>(c)) {
    // the integer we're treating as a pointer is another constant expression, of int type.
    switch (cexpr->getOpcode()) {
      case Instruction::PtrToInt: {
        // derived from a PtrToInt ... just use the status of the orig ptr
        return getStatus(cexpr->getOperand(0));
      }
      case Instruction::SExt:
      case Instruction::ZExt:
      {
        // derived from a SExt or ZExt. Recurse
        return tryGetStatusOfConstInt(cexpr->getOperand(0));
      }
      case Instruction::Select: {
        // derived from a Select of two other constant integers.
        // merger of their two statuses.
        std::optional<PointerStatus> A = tryGetStatusOfConstInt(cexpr->getOperand(1));
        std::optional<PointerStatus> B = tryGetStatusOfConstInt(cexpr->getOperand(2));
        if (A.has_value() && B.has_value()) {
          // we assume that one of them is actually or functionally NULL, so we
          // can assume we don't need to insert dynamic instructions for this
          // merger
          return PointerStatus::merge_direct(*A, *B, NULL);
        } else {
          // couldn't get bounds for one or both of A or B, so can't get bounds
          // for the Select
          return std::nullopt;
        }
      }
      default:
        // anything else is not a pattern we handle here
        return std::nullopt;
    }
  } else {
    // anything else is not a pattern we handle here
    return std::nullopt;
  }
}

std::string PointerStatuses::describe() const {
  SmallVector<const Value*, 8> clean_ptrs = SmallVector<const Value*, 8>();
  SmallVector<const Value*, 8> blem_ptrs = SmallVector<const Value*, 8>();
  SmallVector<const Value*, 8> dirty_ptrs = SmallVector<const Value*, 8>();
  SmallVector<const Value*, 8> unk_ptrs = SmallVector<const Value*, 8>();
  SmallVector<const Value*, 8> dyn_ptrs = SmallVector<const Value*, 8>();
  SmallVector<const Value*, 8> dynnull_ptrs = SmallVector<const Value*, 8>();
  SmallVector<const Value*, 8> ndy_ptrs = SmallVector<const Value*, 8>();
  for (auto& pair : map) {
    const Value* ptr = pair.getFirst();
    if (ptr->hasName() && ptr->getName().startswith("__DMS")) {
      // name starts with __DMS, skip it
      continue;
    }
    const PointerStatus& status = pair.getSecond();
    if (status.is_clean()) {
      clean_ptrs.push_back(ptr);
    } else if (status.is_blemished()) {
      blem_ptrs.push_back(ptr);
    } else if (status.is_dirty()) {
      dirty_ptrs.push_back(ptr);
    } else if (status.is_unknown()) {
      unk_ptrs.push_back(ptr);
    } else if (status.is_dynamic()) {
      const PointerStatus::Dynamic& dyn = std::get<PointerStatus::Dynamic>(status.data);
      if (dyn.dynamic_kind) dyn_ptrs.push_back(ptr);
      else dynnull_ptrs.push_back(ptr);
    } else if (status.is_notdefinedyet()) {
      ndy_ptrs.push_back(ptr);
    } else {
      llvm_unreachable("Missing PointerStatus case");
    }
  }
  std::ostringstream out;
  describePointerList(clean_ptrs, out, "clean");
  out << " and ";
  describePointerList(blem_ptrs, out, "blem");
  out << " and ";
  describePointerList(dirty_ptrs, out, "dirty");
  out << " and ";
  describePointerList(unk_ptrs, out, "unk");
  out << " and ";
  describePointerList(dyn_ptrs, out, "dyn");
  out << " and ";
  describePointerList(dynnull_ptrs, out, "dynnull");
  out << " and ";
  describePointerList(ndy_ptrs, out, "ndy");
  return out.str();
}

/// Merge the given PointerStatuses. If they disagree on any pointer, use
/// `PointerStatus::merge_with_phi` to combine the statuses.
/// Recall that any pointer not appearing in the `map` is considered NOTDEFINEDYET.
///
/// `merge_block`: block where the merge is happening. The merged statuses are for this block.
PointerStatuses PointerStatuses::merge(const SmallVector<const PointerStatuses*, 4>& statuses, BasicBlock& merge_block) {
  assert(statuses.size() > 0);
  for (size_t i = 0; i < statuses.size() - 1; i++) {
    assert(statuses[i]->DL == statuses[i+1]->DL);
    assert(statuses[i]->trust_llvm_struct_types == statuses[i+1]->trust_llvm_struct_types);
    assert(&statuses[i]->added_insts == &statuses[i+1]->added_insts);
  }
  PointerStatuses merged(merge_block, statuses[0]->DL, statuses[0]->trust_llvm_struct_types, statuses[0]->inttoptr_status, statuses[0]->added_insts, statuses[0]->pointer_aliases);
  for (size_t i = 0; i < statuses.size(); i++) {
    for (const auto& pair : statuses[i]->map) {
      SmallVector<StatusWithBlock, 4> statuses_for_ptr;
      const Value* ptr = pair.getFirst();
      statuses_for_ptr.push_back(
        StatusWithBlock(pair.getSecond(), &statuses[i]->block)
      );
      bool handled = false;
      for (size_t prev = 0; prev < i; prev++) {
        const auto& it = statuses[prev]->map.find(ptr);
        // if this ptr is in a previous map, we've already handled it
        if (it != statuses[prev]->map.end()) {
          handled = true;
          break;
        }
        // otherwise it's implicitly NOTDEFINEDYET in that map
        statuses_for_ptr.push_back(
          StatusWithBlock(PointerStatus::notdefinedyet(), &statuses[prev]->block)
        );
      }
      if (handled) continue;
      // pointer is not in a previous map, so we need to handle it now
      // get the statuses in the next map(s), if any
      for (size_t next = i + 1; next < statuses.size(); next++) {
        const auto& it = statuses[next]->map.find(ptr);
        PointerStatus status_in_next = (it == statuses[next]->map.end()) ?
          // implicitly NOTDEFINEDYET in next
          PointerStatus::notdefinedyet() :
          // defined in next, get the status
          it->getSecond();
        statuses_for_ptr.push_back(
          StatusWithBlock(status_in_next, &statuses[next]->block)
        );
      }
      // now we have all the statuses, do the merge
      merged.mark_as(ptr, PointerStatus::merge_with_phi(statuses_for_ptr, ptr, &merge_block));
    }
  }
  return merged;
}

/// Print a description of the pointers in `ptrs` to `out`. `desc` is an
/// adjective describing the pointers (e.g., "clean")
static void describePointerList(const SmallVector<const Value*, 8>& ptrs, std::ostringstream& out, StringRef desc) {
  std::string desc_str = desc.str();
  switch (ptrs.size()) {
    case 0: {
      out << "0 " << desc_str << " ptrs";
      break;
    }
    case 1: {
      const Value* ptr = ptrs[0];
      out << "1 " << desc_str << " ptr (" << ptr->getNameOrAsOperand() << ")";
      break;
    }
    default: {
      if (ptrs.size() <= 8) {
        out << ptrs.size() << " " << desc_str << " ptrs (";
        for (const Value* ptr : ptrs) {
          out << ptr->getNameOrAsOperand() << ", ";
        }
        out << ")";
      } else {
        out << ptrs.size() << " " << desc_str << " ptrs";
      }
      break;
    }
  }
}
