// Capacity Observatory - scope paths, authority registry, and evidence records.
#include "capacity_observatory/evidence.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <utility>

#include "capacity_observatory/hash.hpp"

namespace co {
namespace {

// Unambiguous field encoding for content digests: name, separator, length,
// separator, bytes. Prevents concatenation ambiguity between adjacent fields.
class DigestBuilder {
 public:
  DigestBuilder() { hasher_.update("co.evidence.v1\n"); }

  void field(std::string_view name, std::string_view value) {
    hasher_.update(name);
    hasher_.update(static_cast<std::uint8_t>(0x1F));
    const std::string length = std::to_string(value.size());
    hasher_.update(length);
    hasher_.update(static_cast<std::uint8_t>(0x1E));
    hasher_.update(value);
    hasher_.update(static_cast<std::uint8_t>(0x1D));
  }

  void field(std::string_view name, std::uint64_t value) { field(name, std::to_string(value)); }
  void field(std::string_view name, std::int64_t value) { field(name, std::to_string(value)); }
  void field(std::string_view name, bool value) { field(name, std::string_view(value ? "1" : "0")); }

  [[nodiscard]] Digest finish() noexcept { return hasher_.finish(); }

 private:
  Sha256 hasher_;
};

constexpr std::array<std::string_view, 5> kScopeKindText{"site", "hall", "zone", "row", "enclosure"};

}  // namespace

std::string_view scope_kind_text(ScopeKind kind) noexcept {
  const std::size_t index = static_cast<std::size_t>(kind);
  return index < kScopeKindText.size() ? kScopeKindText[index] : std::string_view("unknown");
}

Result<ScopeKind> scope_kind_from_text(std::string_view text) {
  for (std::size_t i = 0; i < kScopeKindText.size(); ++i) {
    if (kScopeKindText[i] == text) {
      return Result<ScopeKind>(static_cast<ScopeKind>(i));
    }
  }
  return make_error(ReasonCode::InvalidArgument, "unknown scope kind '" + std::string(text) + "'");
}

Result<ScopeSegment> ScopeSegment::parse(std::string_view text) {
  const std::size_t separator = text.find('=');
  if (separator == std::string_view::npos) {
    return make_error(ReasonCode::InvalidArgument,
                      "scope segment '" + std::string(text) + "' must have the form <kind>=<name>");
  }
  const Result<ScopeKind> kind = scope_kind_from_text(text.substr(0, separator));
  if (!kind.ok()) {
    return kind.status();
  }
  const std::string_view name = text.substr(separator + 1);
  if (!is_valid_identifier(name)) {
    return make_error(ReasonCode::InvalidIdentifier,
                      "scope segment name '" + std::string(name) + "' is not a valid identifier");
  }
  ScopeSegment segment;
  segment.kind = kind.value();
  segment.name = std::string(name);
  return Result<ScopeSegment>(std::move(segment));
}

std::string ScopeSegment::text() const {
  return std::string(scope_kind_text(kind)) + "=" + name;
}

Result<ScopePath> ScopePath::parse(std::string_view text) {
  if (text.empty()) {
    return make_error(ReasonCode::EmptyInput, "scope path is empty");
  }
  std::vector<ScopeSegment> segments;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t slash = text.find('/', start);
    const std::string_view piece =
        slash == std::string_view::npos ? text.substr(start) : text.substr(start, slash - start);
    if (piece.empty()) {
      return make_error(ReasonCode::InvalidArgument, "scope path '" + std::string(text) + "' has an empty segment");
    }
    const Result<ScopeSegment> segment = ScopeSegment::parse(piece);
    if (!segment.ok()) {
      return segment.status();
    }
    segments.push_back(segment.value());
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }

  for (std::size_t i = 1; i < segments.size(); ++i) {
    if (segments[i].kind <= segments[i - 1].kind) {
      return make_error(ReasonCode::InvalidArgument,
                        "scope path '" + std::string(text) +
                            "' must list distinct segment kinds from outermost to innermost");
    }
  }
  return Result<ScopePath>(ScopePath(std::move(segments)));
}

Result<ScopePath> ScopePath::of(std::initializer_list<ScopeSegment> segments) {
  std::vector<ScopeSegment> copy(segments);
  for (std::size_t i = 0; i < copy.size(); ++i) {
    if (!is_valid_identifier(copy[i].name)) {
      return make_error(ReasonCode::InvalidIdentifier, "scope segment name is not a valid identifier");
    }
    if (i > 0 && copy[i].kind <= copy[i - 1].kind) {
      return make_error(ReasonCode::InvalidArgument, "scope segments must be ordered outermost first");
    }
  }
  return Result<ScopePath>(ScopePath(std::move(copy)));
}

bool ScopePath::is_prefix_of(const ScopePath& other) const noexcept {
  if (segments_.size() > other.segments_.size()) {
    return false;
  }
  for (std::size_t i = 0; i < segments_.size(); ++i) {
    if (!(segments_[i] == other.segments_[i])) {
      return false;
    }
  }
  return true;
}

ScopePath ScopePath::parent() const {
  if (segments_.empty()) {
    return ScopePath{};
  }
  std::vector<ScopeSegment> copy(segments_.begin(), segments_.end() - 1);
  return ScopePath(std::move(copy));
}

std::string ScopePath::text() const {
  std::string out;
  for (const ScopeSegment& segment : segments_) {
    if (!out.empty()) {
      out.push_back('/');
    }
    out.append(segment.text());
  }
  return out;
}

std::string_view assertion_text(CapacityAssertion assertion) noexcept {
  switch (assertion) {
    case CapacityAssertion::Nameplate: return "nameplate";
    case CapacityAssertion::Governed: return "governed";
    case CapacityAssertion::Planned: return "planned";
    case CapacityAssertion::Installed: return "installed";
    case CapacityAssertion::Observed: return "observed";
    case CapacityAssertion::Reserved: return "reserved";
    case CapacityAssertion::Committed: return "committed";
    case CapacityAssertion::Available: return "available";
    case CapacityAssertion::Stranded: return "stranded";
    case CapacityAssertion::Disputed: return "disputed";
    case CapacityAssertion::ExcludedPolicy: return "excluded-policy";
    case CapacityAssertion::ExcludedMaintenance: return "excluded-maintenance";
    case CapacityAssertion::ExcludedFailure: return "excluded-failure";
    case CapacityAssertion::OperationalReserve: return "operational-reserve";
  }
  return "unknown";
}

Result<CapacityAssertion> assertion_from_text(std::string_view text) {
  for (std::size_t i = 0; i < kAssertionCount; ++i) {
    const auto candidate = static_cast<CapacityAssertion>(i);
    if (assertion_text(candidate) == text) {
      return Result<CapacityAssertion>(candidate);
    }
  }
  return make_error(ReasonCode::InvalidArgument, "unknown capacity assertion '" + std::string(text) + "'");
}

std::string_view authority_role_text(AuthorityRole role) noexcept {
  switch (role) {
    case AuthorityRole::CommittedCapacity: return "committed-capacity";
    case AuthorityRole::Reservations: return "reservations";
    case AuthorityRole::InstalledInventory: return "installed-inventory";
    case AuthorityRole::Policy: return "policy";
    case AuthorityRole::PlantTelemetry: return "plant-telemetry";
    case AuthorityRole::Economics: return "economics";
    case AuthorityRole::Arbitration: return "arbitration";
  }
  return "unknown";
}

std::string AssertionSet::text() const {
  std::string out;
  for (std::size_t i = 0; i < kAssertionCount; ++i) {
    const auto assertion = static_cast<CapacityAssertion>(i);
    if (!contains(assertion)) {
      continue;
    }
    if (!out.empty()) {
      out.push_back(',');
    }
    out.append(assertion_text(assertion));
  }
  return out.empty() ? std::string("none") : out;
}

AuthorityRegistry AuthorityRegistry::standard() {
  AuthorityRegistry registry;
  const auto add_contract = [&registry](AuthorityId id, AuthorityRole role, AssertionSet assertions, Duration budget,
                                        bool synthetic) {
    AuthorityContract contract;
    contract.id = std::move(id);
    contract.role = role;
    contract.dimensions = DimensionSet::all();
    contract.assertions = assertions;
    contract.staleness_budget = budget;
    contract.synthetic_source = synthetic;
    const Result<void> added = registry.add(std::move(contract));
    (void)added;
  };

  add_contract(AuthorityId("dccp"), AuthorityRole::CommittedCapacity,
               AssertionSet::of(CapacityAssertion::Committed), seconds(900), false);
  add_contract(AuthorityId("asi"), AuthorityRole::Reservations, AssertionSet::of(CapacityAssertion::Reserved),
               seconds(300), false);
  add_contract(AuthorityId("dfi"), AuthorityRole::InstalledInventory,
               AssertionSet::of(CapacityAssertion::Installed)
                   .add(CapacityAssertion::Planned)
                   .add(CapacityAssertion::Available)
                   .add(CapacityAssertion::Stranded),
               seconds(3600), false);
  add_contract(AuthorityId("policy"), AuthorityRole::Policy,
               AssertionSet::of(CapacityAssertion::Nameplate)
                   .add(CapacityAssertion::Governed)
                   .add(CapacityAssertion::ExcludedPolicy)
                   .add(CapacityAssertion::ExcludedMaintenance)
                   .add(CapacityAssertion::ExcludedFailure)
                   .add(CapacityAssertion::OperationalReserve),
               seconds(86400), false);
  add_contract(AuthorityId("plant"), AuthorityRole::PlantTelemetry, AssertionSet::of(CapacityAssertion::Observed),
               seconds(60), true);
  add_contract(AuthorityId("economics"), AuthorityRole::Economics, AssertionSet{}, seconds(3600), true);
  add_contract(AuthorityId("arbiter"), AuthorityRole::Arbitration, AssertionSet::of(CapacityAssertion::Disputed),
               seconds(86400), false);
  return registry;
}

Result<void> AuthorityRegistry::add(AuthorityContract contract) {
  if (contract.id.empty()) {
    return make_error(ReasonCode::InvalidIdentifier, "authority id is empty");
  }
  if (find(contract.id) != nullptr) {
    return make_error(ReasonCode::AlreadyExists, "authority '" + contract.id.value() + "' is already registered");
  }
  contracts_.push_back(std::move(contract));
  return Result<void>{};
}

const AuthorityContract* AuthorityRegistry::find(const AuthorityId& id) const noexcept {
  for (const AuthorityContract& contract : contracts_) {
    if (contract.id == id) {
      return &contract;
    }
  }
  return nullptr;
}

std::string_view provenance_text(Provenance provenance) noexcept {
  switch (provenance) {
    case Provenance::Observed: return "observed";
    case Provenance::Recovered: return "recovered";
    case Provenance::Synthetic: return "synthetic";
  }
  return "unknown";
}

bool ValidityWindow::contains(Timestamp instant) const noexcept {
  if (from.has_value() && instant < from.value()) {
    return false;
  }
  if (until.has_value() && instant > until.value()) {
    return false;
  }
  return true;
}

Result<EvidenceRecord> EvidenceRecord::make(AuthorityId authority,
                                            AuthorityRole role,
                                            ScopePath scope,
                                            Dimension dimension,
                                            CapacityAssertion assertion,
                                            Unit unit,
                                            std::int64_t declared_amount,
                                            Generation generation,
                                            Epoch epoch,
                                            Revision revision) {
  if (unit.dimension() != dimension) {
    return make_error(ReasonCode::DimensionMismatch,
                      "unit '" + unit.symbol() + "' denotes " + std::string(dimension_text(unit.dimension())) +
                          " but the record dimension is " + std::string(dimension_text(dimension)));
  }
  const Result<Amount> canonical = unit.to_canonical(declared_amount);
  if (!canonical.ok()) {
    return canonical.status().with_context("evidence amount conversion");
  }

  EvidenceRecord record;
  record.authority = std::move(authority);
  record.role = role;
  record.scope = std::move(scope);
  record.dimension = dimension;
  record.assertion = assertion;
  record.unit = unit;
  record.declared_amount = declared_amount;
  record.amount = canonical.value();
  record.generation = generation;
  record.epoch = epoch;
  record.revision = revision;
  return Result<EvidenceRecord>(std::move(record));
}

Result<void> EvidenceRecord::validate() const {
  if (authority.empty()) {
    return make_error(ReasonCode::MalformedEvidence, "authority id is missing");
  }
  // Identifiers are validated here and not only in the parsing constructors:
  // a record can reach the window through a directly constructed AuthorityId or
  // ScopePath, and a scope whose text cannot be parsed back would poison the
  // ledger, which reparses every contributing scope.
  if (!is_valid_identifier(authority.value())) {
    return make_error(ReasonCode::InvalidIdentifier,
                      "authority id '" + authority.value() + "' is not a well formed identifier");
  }
  if (scope.empty()) {
    return make_error(ReasonCode::MalformedEvidence, "scope path is missing");
  }
  for (std::size_t i = 0; i < scope.segments().size(); ++i) {
    const ScopeSegment& segment = scope.segments()[i];
    if (!is_valid_identifier(segment.name)) {
      return make_error(ReasonCode::InvalidIdentifier,
                        "scope segment name '" + segment.name + "' is not a well formed identifier");
    }
    if (i > 0 && segment.kind <= scope.segments()[i - 1].kind) {
      return make_error(ReasonCode::InvalidArgument,
                        "scope path '" + scope.text() +
                            "' must list distinct segment kinds from outermost to innermost");
    }
  }
  if (unit.dimension() != dimension) {
    return make_error(ReasonCode::DimensionMismatch, "declared unit does not match the record dimension");
  }
  const Result<Amount> expected = unit.to_canonical(declared_amount);
  if (!expected.ok()) {
    return expected.status().with_context("evidence validation");
  }
  if (!(expected.value() == amount)) {
    return make_error(ReasonCode::MalformedEvidence,
                      "canonical amount " + std::to_string(amount.canonical()) +
                          " does not match the declared amount " + std::to_string(declared_amount) + " " +
                          unit.symbol());
  }
  if (amount.is_negative()) {
    return make_error(ReasonCode::NegativeAmount,
                      "capacity magnitude must not be negative but is " + std::to_string(amount.canonical()));
  }
  if (validity.from.has_value() && validity.until.has_value() && validity.until.value() < validity.from.value()) {
    return make_error(ReasonCode::MalformedEvidence, "validity window closes before it opens");
  }
  return Result<void>{};
}

std::string EvidenceRecord::slot_text() const {
  return authority.value() + "|" + scope.text() + "|" + std::string(dimension_text(dimension)) + "|" +
         std::string(assertion_text(assertion));
}

std::string EvidenceRecord::id_text() const {
  return slot_text() + "|e" + std::to_string(epoch.value()) + "|g" + std::to_string(generation.value());
}

Digest EvidenceRecord::content_digest() const {
  DigestBuilder builder;
  builder.field("authority", authority.value());
  builder.field("role", authority_role_text(role));
  builder.field("scope", scope.text());
  builder.field("dimension", dimension_text(dimension));
  builder.field("assertion", assertion_text(assertion));
  builder.field("unit", unit.symbol());
  builder.field("unit-numerator", unit.numerator());
  builder.field("unit-denominator", unit.denominator());
  builder.field("declared-amount", declared_amount);
  builder.field("canonical-amount", amount.canonical());
  builder.field("epoch", epoch.value());
  builder.field("generation", generation.value());
  builder.field("revision", revision.value());
  builder.field("provenance", provenance_text(provenance));
  builder.field("has-observed-at", has_observed_at);
  builder.field("observed-at", has_observed_at ? observed_at.unix_nanos() : 0);
  builder.field("validity-from", validity.from.has_value() ? validity.from.value().unix_nanos() : 0);
  builder.field("validity-from-present", validity.from.has_value());
  builder.field("validity-until", validity.until.has_value() ? validity.until.value().unix_nanos() : 0);
  builder.field("validity-until-present", validity.until.has_value());
  builder.field("source", source);
  return builder.finish();
}

}  // namespace co
