// Capacity Observatory - snapshot assembly, JSON, and text rendering.
#include "capacity_observatory/snapshot.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "capacity_observatory/platform.hpp"

namespace co {
namespace {

json::Value optional_amount_json(const std::optional<Amount>& amount) {
  if (!amount.has_value()) {
    return json::Value();  // explicit null: unknown, never zero
  }
  return json::Value(amount.value().canonical());
}

std::string optional_amount_text(const std::optional<Amount>& amount) {
  return amount.has_value() ? std::to_string(amount.value().canonical()) : std::string("unknown");
}

json::Value declared_json(const ClassAmounts& declared) {
  json::Object members;
  for (const CapacityAssertion assertion : {CapacityAssertion::Nameplate, CapacityAssertion::Governed,
                                            CapacityAssertion::Planned, CapacityAssertion::Installed,
                                            CapacityAssertion::Observed, CapacityAssertion::Reserved,
                                            CapacityAssertion::Committed, CapacityAssertion::Available,
                                            CapacityAssertion::Stranded, CapacityAssertion::Disputed,
                                            CapacityAssertion::ExcludedPolicy, CapacityAssertion::ExcludedMaintenance,
                                            CapacityAssertion::ExcludedFailure, CapacityAssertion::OperationalReserve}) {
    const std::optional<Amount>& value = declared.get(assertion);
    if (value.has_value()) {
      members.emplace_back(std::string(assertion_text(assertion)), json::Value(value.value().canonical()));
    }
  }
  return json::Value::object(std::move(members));
}

json::Value line_to_json(const LedgerLine& line) {
  json::Object members;
  members.emplace_back("dimension", json::Value(std::string(dimension_text(line.dimension))));
  members.emplace_back("canonical_unit", json::Value(std::string(canonical_unit_text(line.dimension))));
  members.emplace_back("state", json::Value(std::string(line_state_text(line.state))));
  members.emplace_back("freshness", json::Value(std::string(freshness_text(line.worst_freshness))));
  members.emplace_back("declared", declared_json(line.declared));

  json::Object derived;
  derived.emplace_back("exclusion_total", optional_amount_json(line.exclusion_total));
  derived.emplace_back("serviceable", optional_amount_json(line.serviceable));
  derived.emplace_back("free_usable", optional_amount_json(line.free_usable));
  derived.emplace_back("governed", optional_amount_json(line.governed));
  derived.emplace_back("governed_derived", json::Value(line.governed_derived));
  members.emplace_back("derived", json::Value::object(std::move(derived)));

  json::Object residuals;
  for (const ClosureCheck& check : line.closures) {
    json::Object entry;
    entry.emplace_back("reason", json::Value(std::string(reason_text(check.code))));
    entry.emplace_back("residual", optional_amount_json(check.residual));
    entry.emplace_back("holds", json::Value(check.holds));
    entry.emplace_back("indeterminate", json::Value(check.indeterminate));
    entry.emplace_back("explanation", json::Value(check.explanation));
    residuals.emplace_back(check.name, json::Value::object(std::move(entry)));
  }
  members.emplace_back("closures", json::Value::object(std::move(residuals)));
  members.emplace_back("unexplained_governance", optional_amount_json(line.unexplained_governance));
  members.emplace_back("unexplained_installed", optional_amount_json(line.unexplained_installed));
  members.emplace_back("unexplained_available", optional_amount_json(line.unexplained_available));
  members.emplace_back("overcommitted", json::Value(line.overcommitted));
  members.emplace_back("overcommitment", optional_amount_json(line.overcommitment));
  members.emplace_back("unknown_classes", json::Value(static_cast<std::int64_t>(line.unknown_classes)));
  members.emplace_back("evidence_count", json::Value(static_cast<std::int64_t>(line.evidence_count)));

  json::Array scopes;
  for (const ScopePath& scope : line.contributing_scopes) {
    scopes.emplace_back(json::Value(scope.text()));
  }
  members.emplace_back("contributing_scopes", json::Value::array(std::move(scopes)));

  json::Array evidence;
  for (const std::string& id : line.evidence_ids) {
    evidence.emplace_back(json::Value(id));
  }
  members.emplace_back("evidence", json::Value::array(std::move(evidence)));

  json::Array explanations;
  for (const std::string& text : line.explanations) {
    explanations.emplace_back(json::Value(text));
  }
  members.emplace_back("explanations", json::Value::array(std::move(explanations)));
  return json::Value::object(std::move(members));
}

json::Value fragmentation_to_json(const FragmentationReport& report) {
  json::Object members;
  members.emplace_back("profile", json::Value(report.profile_name));
  members.emplace_back("rack_units_per_rack", json::Value(report.rack_units_per_rack));
  members.emplace_back("realizable_racks", json::Value(report.realizable_racks));
  members.emplace_back("ideal_racks", json::Value(report.ideal_racks));
  members.emplace_back("fragmented_racks", json::Value(report.fragmented_racks));
  members.emplace_back("indeterminate", json::Value(report.indeterminate));
  members.emplace_back("indeterminate_enclosures",
                       json::Value(static_cast<std::int64_t>(report.indeterminate_enclosures)));

  json::Object ideal_by_dimension;
  json::Object fragmented_by_dimension;
  for (const Dimension dimension : kAllDimensions) {
    const std::optional<std::int64_t>& ideal = report.ideal_racks_by_dimension[dimension_index(dimension)];
    const std::optional<std::int64_t>& fragmented = report.fragmented_by_dimension[dimension_index(dimension)];
    if (ideal.has_value()) {
      ideal_by_dimension.emplace_back(std::string(dimension_text(dimension)), json::Value(ideal.value()));
    }
    if (fragmented.has_value()) {
      fragmented_by_dimension.emplace_back(std::string(dimension_text(dimension)), json::Value(fragmented.value()));
    }
  }
  members.emplace_back("ideal_by_dimension", json::Value::object(std::move(ideal_by_dimension)));
  members.emplace_back("fragmented_by_dimension", json::Value::object(std::move(fragmented_by_dimension)));
  members.emplace_back("constrained_dimensions", json::Value(dimension_set_text(report.constrained_dimensions)));

  json::Object stranded;
  json::Object headroom;
  for (const Dimension dimension : kAllDimensions) {
    if (report.stranded.has(dimension)) {
      stranded.emplace_back(std::string(dimension_text(dimension)),
                            json::Value(report.stranded.get(dimension).value().canonical()));
    }
    if (report.headroom.has(dimension)) {
      headroom.emplace_back(std::string(dimension_text(dimension)),
                            json::Value(report.headroom.get(dimension).value().canonical()));
    }
  }
  members.emplace_back("stranded", json::Value::object(std::move(stranded)));
  members.emplace_back("headroom", json::Value::object(std::move(headroom)));

  json::Array enclosures;
  for (const EnclosureCapacity& enclosure : report.enclosures) {
    json::Object entry;
    entry.emplace_back("id", json::Value(enclosure.id.value()));
    entry.emplace_back("scope", json::Value(enclosure.scope.text()));
    entry.emplace_back("realizable_racks", json::Value(enclosure.realizable_racks));
    entry.emplace_back("binding", json::Value(enclosure.binding));
    entry.emplace_back("binding_reason", json::Value(std::string(reason_text(enclosure.binding_reason))));
    entry.emplace_back("indeterminate", json::Value(enclosure.indeterminate));
    entry.emplace_back("explanation", json::Value(enclosure.explanation));
    enclosures.emplace_back(json::Value::object(std::move(entry)));
  }
  members.emplace_back("enclosures", json::Value::array(std::move(enclosures)));

  json::Array explanations;
  for (const std::string& text : report.explanations) {
    explanations.emplace_back(json::Value(text));
  }
  members.emplace_back("explanations", json::Value::array(std::move(explanations)));
  return json::Value::object(std::move(members));
}

std::string version_text() {
  std::ostringstream out;
  out << CO_VERSION_MAJOR << '.' << CO_VERSION_MINOR << '.' << CO_VERSION_PATCH;
  return out.str();
}

}  // namespace

std::vector<std::string> summarize_refusals(const EvidenceWindow& window, std::size_t limit) {
  std::vector<std::string> lines;
  const std::vector<RefusedEvidence>& refusals = window.refusals();
  const std::size_t start = refusals.size() > limit ? refusals.size() - limit : 0;
  for (std::size_t i = start; i < refusals.size(); ++i) {
    const RefusedEvidence& refusal = refusals[i];
    lines.push_back(refusal.record.id_text() + " -> " +
                    std::string(acceptance_outcome_text(refusal.decision.outcome)) + " (" +
                    std::string(reason_text(refusal.decision.reason)) + "): " + refusal.decision.explanation);
  }
  if (window.dropped_refusals() != 0) {
    lines.push_back(std::to_string(window.dropped_refusals()) +
                    " further refusal(s) were counted but not retained because the bounded audit trail is full");
  }
  return lines;
}

Result<Snapshot> build_snapshot(const EvidenceWindow& window,
                                const ScopePath& scope,
                                DimensionSet dimensions,
                                const Topology* topology,
                                const RackProfile* profile,
                                const Store* store) {
  Snapshot snapshot;
  snapshot.scope = scope;
  snapshot.generated_at = window.clock()->now();

  LedgerRequest request;
  request.scope = scope;
  request.dimensions = dimensions;
  Result<Ledger> ledger = compose_ledger(window, request);
  if (!ledger.ok()) {
    return ledger.status();
  }
  snapshot.ledger = std::move(ledger).value();

  if (topology != nullptr && profile != nullptr) {
    Result<FragmentationReport> report = analyze_fragmentation(*topology, *profile);
    if (!report.ok()) {
      return report.status().with_context("fragmentation analysis");
    }
    snapshot.fragmentation = std::move(report).value();
  }

  SnapshotProvenance& provenance = snapshot.provenance;
  provenance.generated_at = snapshot.generated_at;
  provenance.evidence_records = window.size();
  provenance.accepted_total = window.accepted_count();
  provenance.refused_records = window.refusals().size();
  provenance.dropped_refusals = window.dropped_refusals();
  provenance.store_attached = store != nullptr;
  if (store != nullptr) {
    provenance.session_epoch = store->session_epoch();
    provenance.store_directory = store->directory();
    provenance.durable_mutations = store->mutations().size();
  }

  bool every_recovered = window.size() != 0;
  for (const auto& entry : window.current()) {
    if (entry.second.provenance != Provenance::Recovered) {
      every_recovered = false;
    }
  }
  provenance.recovered_only = every_recovered;

  for (const AuthorityContract& contract : window.registry().contracts()) {
    if (contract.synthetic_source) {
      ++provenance.synthetic_sources;
    }
  }
  provenance.plant_evidence_note =
      "plant, DCIM, telemetry, and economic evidence is SYNTHETIC unless a deployment attaches real plant data; "
      "installed/committed/reserved/policy evidence is REAL only when the named authority actually published it";

  if (provenance.recovered_only) {
    snapshot.explanations.push_back(
        "every contributing record was recovered from durable storage; recovered evidence is not fresh evidence and "
        "no recovered record is promoted to a current observation by recovery alone");
  }
  snapshot.refused_summary = summarize_refusals(window, 32);

  snapshot.explanations.push_back(snapshot.ledger.explanation);
  snapshot.explanations.push_back("evidence window holds " + std::to_string(window.size()) +
                                  " current record(s), " + std::to_string(window.accepted_count()) +
                                  " accepted in total, " + std::to_string(window.refusals().size()) +
                                  " retained refusal(s)");
  if (snapshot.ledger.has_residuals()) {
    snapshot.explanations.push_back(
        "at least one closure residual is non-zero: declared authority figures are not fully explained by the other "
        "evidence in scope; the residual is reported rather than clamped");
  }
  if (snapshot.ledger.has_overcommitment()) {
    snapshot.explanations.push_back("at least one dimension is overcommitted: allocated capacity exceeds the governed "
                                    "ceiling or installed capacity");
  }
  return Result<Snapshot>(std::move(snapshot));
}

json::Value snapshot_to_json(const Snapshot& snapshot) {
  json::Object root;
  json::Object tool;
  tool.emplace_back("name", json::Value(std::string("capacity-observatory")));
  tool.emplace_back("version", json::Value(version_text()));
  tool.emplace_back("boundary", json::Value(std::string(
      "reads and explains capacity state and deltas; owns no canonical capacity, reservation, placement, admission, "
      "entitlement, or reconciliation mutation")));
  root.emplace_back("tool", json::Value::object(std::move(tool)));
  root.emplace_back("scope", json::Value(snapshot.scope.text()));
  root.emplace_back("generated_at", json::Value(snapshot.generated_at.unix_nanos()));

  json::Object provenance;
  provenance.emplace_back("store_attached", json::Value(snapshot.provenance.store_attached));
  provenance.emplace_back("store_directory", json::Value(snapshot.provenance.store_directory));
  provenance.emplace_back("session_epoch", json::Value(static_cast<std::int64_t>(snapshot.provenance.session_epoch.value())));
  provenance.emplace_back("evidence_records", json::Value(static_cast<std::int64_t>(snapshot.provenance.evidence_records)));
  provenance.emplace_back("accepted_total", json::Value(static_cast<std::int64_t>(snapshot.provenance.accepted_total)));
  provenance.emplace_back("refused_records", json::Value(static_cast<std::int64_t>(snapshot.provenance.refused_records)));
  provenance.emplace_back("dropped_refusals", json::Value(static_cast<std::int64_t>(snapshot.provenance.dropped_refusals)));
  provenance.emplace_back("durable_mutations", json::Value(static_cast<std::int64_t>(snapshot.provenance.durable_mutations)));
  provenance.emplace_back("recovered_only", json::Value(snapshot.provenance.recovered_only));
  provenance.emplace_back("synthetic_authorities", json::Value(static_cast<std::int64_t>(snapshot.provenance.synthetic_sources)));
  provenance.emplace_back("plant_evidence_note", json::Value(snapshot.provenance.plant_evidence_note));
  root.emplace_back("provenance", json::Value::object(std::move(provenance)));

  json::Object ledger;
  ledger.emplace_back("scope", json::Value(snapshot.ledger.scope.text()));
  ledger.emplace_back("has_residuals", json::Value(snapshot.ledger.has_residuals()));
  ledger.emplace_back("has_overcommitment", json::Value(snapshot.ledger.has_overcommitment()));
  ledger.emplace_back("incomplete_lines", json::Value(static_cast<std::int64_t>(snapshot.ledger.incomplete_lines())));
  json::Array lines;
  for (const LedgerLine& line : snapshot.ledger.lines) {
    lines.emplace_back(line_to_json(line));
  }
  ledger.emplace_back("lines", json::Value::array(std::move(lines)));
  ledger.emplace_back("explanation", json::Value(snapshot.ledger.explanation));
  root.emplace_back("ledger", json::Value::object(std::move(ledger)));

  if (snapshot.fragmentation.has_value()) {
    root.emplace_back("fragmentation", fragmentation_to_json(snapshot.fragmentation.value()));
  } else {
    root.emplace_back("fragmentation", json::Value());
  }

  json::Array refused;
  for (const std::string& text : snapshot.refused_summary) {
    refused.emplace_back(json::Value(text));
  }
  root.emplace_back("refused", json::Value::array(std::move(refused)));

  json::Array explanations;
  for (const std::string& text : snapshot.explanations) {
    explanations.emplace_back(json::Value(text));
  }
  root.emplace_back("explanations", json::Value::array(std::move(explanations)));
  return json::Value::object(std::move(root));
}

std::string render_snapshot_text(const Snapshot& snapshot) {
  std::ostringstream out;
  out << "capacity-observatory " << version_text() << '\n';
  out << "scope: " << snapshot.scope.text() << '\n';
  out << "generated_at: " << snapshot.generated_at.unix_nanos() << " (unix nanoseconds)\n";
  out << "boundary: explanation and inspection only; no canonical capacity, reservation, placement, admission, "
         "entitlement, or reconciliation mutation is owned here\n";
  if (snapshot.provenance.store_attached) {
    out << "store: " << snapshot.provenance.store_directory << " (session epoch e"
        << snapshot.provenance.session_epoch.value() << ", " << snapshot.provenance.durable_mutations
        << " durable mutation(s))\n";
  } else {
    out << "store: none attached\n";
  }
  out << "evidence: " << snapshot.provenance.evidence_records << " current, " << snapshot.provenance.accepted_total
      << " accepted, " << snapshot.provenance.refused_records << " refused retained\n";
  if (snapshot.provenance.recovered_only) {
    out << "freshness: every record was recovered from durable storage; recovery is not observation\n";
  }
  out << '\n';

  for (const LedgerLine& line : snapshot.ledger.lines) {
    out << "[" << dimension_text(line.dimension) << "] state=" << line_state_text(line.state)
        << " freshness=" << freshness_text(line.worst_freshness) << " evidence=" << line.evidence_count
        << " unknown_classes=" << line.unknown_classes << (line.overcommitted ? " OVERCOMMITTED" : "") << '\n';
    out << "  declared " << canonical_unit_text(line.dimension) << ':' << '\n';
    for (const CapacityAssertion assertion :
         {CapacityAssertion::Nameplate, CapacityAssertion::Governed, CapacityAssertion::Planned,
          CapacityAssertion::Installed, CapacityAssertion::Observed, CapacityAssertion::Reserved,
          CapacityAssertion::Committed, CapacityAssertion::Available, CapacityAssertion::Stranded,
          CapacityAssertion::Disputed, CapacityAssertion::ExcludedPolicy, CapacityAssertion::ExcludedMaintenance,
          CapacityAssertion::ExcludedFailure, CapacityAssertion::OperationalReserve}) {
      const std::optional<Amount>& value = line.declared.get(assertion);
      if (!value.has_value()) {
        continue;
      }
      out << "    " << assertion_text(assertion) << " = " << value.value().canonical() << '\n';
    }
    out << "  derived:" << '\n';
    out << "    exclusion_total = " << optional_amount_text(line.exclusion_total) << '\n';
    out << "    serviceable = " << optional_amount_text(line.serviceable) << '\n';
    out << "    free_usable = " << optional_amount_text(line.free_usable) << '\n';
    out << "    governed = " << optional_amount_text(line.governed)
        << (line.governed_derived ? " (derived from nameplate)" : " (declared)") << '\n';
    out << "  closures:" << '\n';
    for (const ClosureCheck& check : line.closures) {
      out << "    " << check.name << ": residual="
          << (check.indeterminate ? std::string("indeterminate") : optional_amount_text(check.residual))
          << " reason=" << reason_text(check.code) << '\n';
      out << "      " << check.explanation << '\n';
    }
    if (line.unexplained_governance.has_value() || line.unexplained_installed.has_value() ||
        line.unexplained_available.has_value()) {
      out << "  unexplained: governance=" << optional_amount_text(line.unexplained_governance)
          << " installed=" << optional_amount_text(line.unexplained_installed)
          << " available=" << optional_amount_text(line.unexplained_available) << '\n';
    }
    if (!line.contributing_scopes.empty()) {
      out << "  contributing scopes:";
      for (const ScopePath& scope : line.contributing_scopes) {
        out << ' ' << scope.text();
      }
      out << '\n';
    }
    for (const std::string& explanation : line.explanations) {
      out << "  note: " << explanation << '\n';
    }
    out << '\n';
  }

  if (snapshot.fragmentation.has_value()) {
    const FragmentationReport& report = snapshot.fragmentation.value();
    out << "[fragmentation] profile=" << report.profile_name << " rack_units=" << report.rack_units_per_rack
        << " realizable=" << report.realizable_racks << " ideal=" << report.ideal_racks
        << " fragmented=" << report.fragmented_racks << (report.indeterminate ? " INDETERMINATE" : "") << '\n';
    out << "  constrained dimensions: " << dimension_set_text(report.constrained_dimensions) << '\n';
    for (const EnclosureCapacity& enclosure : report.enclosures) {
      out << "  enclosure " << enclosure.id.value() << " (" << enclosure.scope.text()
          << "): realizable=" << enclosure.realizable_racks << " binding=" << enclosure.binding << '\n';
    }
    for (const std::string& explanation : report.explanations) {
      out << "  note: " << explanation << '\n';
    }
    out << '\n';
  }

  if (!snapshot.refused_summary.empty()) {
    out << "[refused evidence]\n";
    for (const std::string& line : snapshot.refused_summary) {
      out << "  " << line << '\n';
    }
    out << '\n';
  }

  out << "[explanation]\n";
  for (const std::string& explanation : snapshot.explanations) {
    out << "  " << explanation << '\n';
  }
  return out.str();
}

}  // namespace co
