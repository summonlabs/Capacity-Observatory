#pragma once
// Capacity Observatory - fragmentation, stranding, and binding constraints.
//
// A rack needs a contiguous run of rack units and a per-rack budget of every
// dimension the profile demands. Free capacity that is spread across enclosures
// or interrupted by gaps cannot host it. The analysis below is exact integer
// arithmetic: realizable capacity, the aggregate ideal that ignores enclosure
// and gap boundaries, and the difference between them (fragmentation) are all
// reported separately, and unusable capacity is reported as stranded rather
// than folded into either number.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "capacity_observatory/dimension.hpp"
#include "capacity_observatory/reason.hpp"
#include "capacity_observatory/result.hpp"
#include "capacity_observatory/topology.hpp"

namespace co {

struct RackProfile {
  std::string name;
  std::int64_t rack_units{0};
  // Per-rack demand. A dimension that is not demanded must be left unknown, not
  // set to zero: unknown means "this profile places no requirement on that
  // dimension", while a present zero would be a positive requirement of zero.
  AmountVector per_rack;

  [[nodiscard]] static Result<RackProfile> make(std::string name, std::int64_t rack_units,
                                                AmountVector per_rack);
  [[nodiscard]] DimensionSet demanded_dimensions() const noexcept;
};

struct EnclosureCapacity {
  EnclosureId id;
  ScopePath scope;
  std::int64_t realizable_racks{0};
  // "rack-units-contiguity", "structurally-unusable", a dimension name, or a
  // reason code text when the enclosure could not be evaluated.
  std::string binding;
  ReasonCode binding_reason{ReasonCode::Ok};
  bool indeterminate{false};
  AmountVector stranded;   // free capacity in an enclosure that hosts nothing
  AmountVector headroom;   // free capacity left after the realizable racks are placed
  std::string explanation;
};

struct FragmentationReport {
  std::string profile_name;
  std::int64_t rack_units_per_rack{0};
  std::int64_t realizable_racks{0};
  // Aggregate-only bound: total free capacity divided by per-rack demand, as if
  // all free capacity were contiguous and inside one enclosure.
  std::int64_t ideal_racks{0};
  // ideal - realizable, never negative. Reported as a count of racks that the
  // aggregate suggests and enclosure reality denies.
  std::int64_t fragmented_racks{0};
  std::array<std::optional<std::int64_t>, kDimensionCount> ideal_racks_by_dimension{};
  std::array<std::optional<std::int64_t>, kDimensionCount> fragmented_by_dimension{};
  AmountVector stranded;      // capacity in enclosures that can host nothing
  AmountVector headroom;      // capacity left over after realizable placement
  DimensionSet constrained_dimensions;
  std::vector<EnclosureCapacity> enclosures;
  std::vector<std::string> explanations;
  bool indeterminate{false};
  std::size_t indeterminate_enclosures{0};
};

// Analyses the topology against one rack profile.
[[nodiscard]] Result<FragmentationReport> analyze_fragmentation(const Topology& topology,
                                                               const RackProfile& profile);

}  // namespace co
