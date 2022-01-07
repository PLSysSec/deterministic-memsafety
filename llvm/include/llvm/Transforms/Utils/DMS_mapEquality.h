#pragma once

#include "llvm/ADT/DenseMap.h"
#include <optional>

/// Return type of `mapsAreEqual`
template<typename K>
struct MapEqualityResult {
public:
  /// are the maps equal
  bool areEqual() const {
    return !result.has_value();
  }

  /// if the maps aren't equal, but are the same size, here is one key they
  /// disagree on.  (If the maps are equal, this will return NULL; and if the
  /// maps aren't equal because they're different sizes, this will also return
  /// NULL.)
  const K* disagreementKey() const {
    return result.value_or(NULL);
  }

private:
  /// no value means the maps are equal
  /// if there is a value, it's interpreted like disagreement_key() above
  std::optional<const K*> result;

  MapEqualityResult(std::optional<const K*> result) : result(result) {}

  static MapEqualityResult is_equal() {
    return MapEqualityResult(std::nullopt);
  }
  static MapEqualityResult not_equal(const K& key) {
    return MapEqualityResult(&key);
  }
  static MapEqualityResult not_same_size() {
    return MapEqualityResult((const K*)NULL);
  }

public:
  template<typename V, unsigned N>
  static MapEqualityResult mapsAreEqual(
    const llvm::SmallDenseMap<K, V, N> &A,
    const llvm::SmallDenseMap<K, V, N> &B
  ) {
    // first: maps of different sizes can never be equal
    if (A.size() != B.size()) return MapEqualityResult<K>::not_same_size();
    // now check that all keys in A are also in B, and map to the same things
    for (const auto &pair : A) {
      const K& key = pair.getFirst();
      const auto& it = B.find(key);
      if (it == B.end()) {
        // key wasn't in B
        return MapEqualityResult<K>::not_equal(key);
      }
      if (it->getSecond() != pair.getSecond()) {
        // maps disagree on what this key maps to
        return MapEqualityResult<K>::not_equal(key);
      }
    }
    // we don't need the reverse check (all keys in B are also in A) because we
    // already checked that A and B have the same number of keys, and all keys in
    // A are also in B
    return MapEqualityResult<K>::is_equal();
  }
};
