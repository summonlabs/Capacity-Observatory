#pragma once
// Capacity Observatory - placement topology (enclosures and their free budgets).
//
// Topology is inspection input: it describes what placement room exists, not
// what may be admitted. The observatory never places or admits anything.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "capacity_observatory/dimension.hpp"
#include "capacity_observatory/evidence.hpp"
#include "capacity_observatory/result.hpp"

namespace co {

struct EnclosureBudget {
  EnclosureId id;
  ScopePath scope;
  // Free (uncommitted, unreserved, unexcluded) capacity per dimension.
  // nullopt means UNKNOWN and is never treated as zero.
  AmountVector free;
  // Longest run of contiguous free rack units. This is what a rack of height N
  // actually requires, so it can be smaller than free_ru_total.
  std::optional<std::int64_t> free_ru_contiguous;
  // Total free rack units across all gaps.
  std::optional<std::int64_t> free_ru_total;
  // False when the enclosure exists but cannot host anything for a structural
  // reason (no cooling path, condemned floor, no power feed).
  bool structurally_usable{true};
  std::string unusable_reason;
};

class Topology {
 public:
  Topology() = default;

  [[nodiscard]] static Result<Topology> make(std::vector<EnclosureBudget> enclosures);

  [[nodiscard]] const std::vector<EnclosureBudget>& enclosures() const noexcept { return enclosures_; }
  [[nodiscard]] std::size_t size() const noexcept { return enclosures_.size(); }
  [[nodiscard]] bool empty() const noexcept { return enclosures_.empty(); }

  // Keeps enclosures whose scope is at or below 'scope'.
  [[nodiscard]] Result<Topology> narrowed_to(const ScopePath& scope) const;

  [[nodiscard]] const EnclosureBudget* find(const EnclosureId& id) const noexcept;

 private:
  std::vector<EnclosureBudget> enclosures_;
};

}  // namespace co
