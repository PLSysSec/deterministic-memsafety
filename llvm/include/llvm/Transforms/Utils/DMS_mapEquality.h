#pragma once

#include "llvm/ADT/DenseMap.h"
#include <optional>

/// Return type of `mapsAreEqual`
template<typename K>
struct MapEqualityResult {
public:
  // One of the following results

  /// Maps are equal
  class Equal {
  friend class MapEqualityResult;
  private:
    Equal() = default;
  };

  /// Maps are not equal, but are the same size
  class NotEqualSameSize {
  friend class MapEqualityResult;
  public:
    /// here is one key the maps disagree on.
    const K* disagreementKey;
  private:
    NotEqualSameSize(const K* disagreementKey) : disagreementKey(disagreementKey) {}
  };

  /// Maps are not equal because they are different sizes
  class DifferentSizes {
  friend class MapEqualityResult;
  public:
    /// here is one key present in the larger map but not the smaller
    /// This may be NULL in optimized builds, because it's expensive to compute
    const K* unmatchedKey;
  private:
    DifferentSizes(const K* unmatchedKey) : unmatchedKey(unmatchedKey) {}
  };

  /// The actual value: one of these alternatives
  std::variant<Equal, NotEqualSameSize, DifferentSizes> result;

  MapEqualityResult(Equal eq) : result(eq) {}
  MapEqualityResult(NotEqualSameSize neq) : result(neq) {}
  MapEqualityResult(DifferentSizes diff) : result(diff) {}

  bool areEqual() const {
    return std::holds_alternative<Equal>(result);
  }

private:
  static MapEqualityResult is_equal() {
    return MapEqualityResult(Equal());
  }
  static MapEqualityResult not_equal_same_size(const K& key) {
    return MapEqualityResult(NotEqualSameSize(&key));
  }
  static MapEqualityResult not_same_size(const K& key) {
    return MapEqualityResult(DifferentSizes(&key));
  }
  #ifdef NDEBUG
  static MapEqualityResult not_same_size() {
    return MapEqualityResult(DifferentSizes(NULL));
  }
  #endif

public:
  template<typename V, unsigned N>
  static MapEqualityResult mapsAreEqual(
    const llvm::SmallDenseMap<K, V, N> &A,
    const llvm::SmallDenseMap<K, V, N> &B
  ) {
    // first: maps of different sizes can never be equal
    if (A.size() != B.size()) {
      #ifdef NDEBUG
      return MapEqualityResult<K>::not_same_size();
      #else
      // find an unmatched key, just for reporting (in the end this is just used
      // for debugging)
      if (A.size() > B.size()) {
        for (const auto &pair : A) {
          if (!B.count(pair.getFirst())) {
            return MapEqualityResult<K>::not_same_size(pair.getFirst());
          }
        }
      } else {
        for (const auto &pair : B) {
          if (!A.count(pair.getFirst())) {
            return MapEqualityResult<K>::not_same_size(pair.getFirst());
          }
        }
      }
      llvm_unreachable("Failed to find an unmatched key");
      #endif
    }
    // now check that all keys in A are also in B, and map to the same things
    for (const auto &pair : A) {
      const K& key = pair.getFirst();
      const auto& it = B.find(key);
      if (it == B.end()) {
        // key wasn't in B
        return MapEqualityResult<K>::not_equal_same_size(key);
      }
      if (it->getSecond() != pair.getSecond()) {
        // maps disagree on what this key maps to
        return MapEqualityResult<K>::not_equal_same_size(key);
      }
    }
    // we don't need the reverse check (all keys in B are also in A) because we
    // already checked that A and B have the same number of keys, and all keys in
    // A are also in B
    return MapEqualityResult<K>::is_equal();
  }
};
