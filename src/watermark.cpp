// Capacity Observatory - evidence acceptance and freshness evaluation.
#include "capacity_observatory/watermark.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace co {

std::string EvidenceSlotKey::text() const {
  return authority.value() + "|" + scope.text() + "|" + std::string(dimension_text(dimension)) + "|" +
         std::string(assertion_text(assertion));
}

bool operator<(const EvidenceSlotKey& a, const EvidenceSlotKey& b) noexcept {
  if (a.authority != b.authority) {
    return a.authority < b.authority;
  }
  if (!(a.scope == b.scope)) {
    return a.scope < b.scope;
  }
  if (a.dimension != b.dimension) {
    return a.dimension < b.dimension;
  }
  return a.assertion < b.assertion;
}

std::string_view acceptance_outcome_text(AcceptanceOutcome outcome) noexcept {
  switch (outcome) {
    case AcceptanceOutcome::Accepted: return "accepted";
    case AcceptanceOutcome::Duplicate: return "duplicate";
    case AcceptanceOutcome::Conflicted: return "conflicted";
    case AcceptanceOutcome::StaleGeneration: return "stale-generation";
    case AcceptanceOutcome::StaleEpoch: return "stale-epoch";
    case AcceptanceOutcome::Refused: return "refused";
    case AcceptanceOutcome::Invalid: return "invalid";
  }
  return "unknown";
}

EvidenceWindow::EvidenceWindow(const AuthorityRegistry& registry, std::shared_ptr<Clock> clock)
    : registry_(&registry), clock_(std::move(clock)), refusal_capacity_(kDefaultRefusalCapacity) {}

EvidenceWindow::EvidenceWindow(const AuthorityRegistry& registry, std::shared_ptr<Clock> clock,
                               std::size_t refusal_capacity)
    : registry_(&registry), clock_(std::move(clock)), refusal_capacity_(refusal_capacity) {}

namespace {

AcceptanceDecision refuse(AcceptanceOutcome outcome, ReasonCode reason, std::string explanation) {
  AcceptanceDecision decision;
  decision.outcome = outcome;
  decision.reason = reason;
  decision.explanation = std::move(explanation);
  decision.affects_derived_state = false;
  return decision;
}

}  // namespace

Freshness EvidenceWindow::freshness_of(const EvidenceRecord& record) const {
  const AuthorityContract* contract = registry_->find(record.authority);
  const Duration budget = contract == nullptr ? Duration{} : contract->staleness_budget;
  const Result<FreshnessEvaluation> evaluation = evaluate_freshness(
      record.has_observed_at, record.observed_at, clock_->now(), budget, record.provenance == Provenance::Recovered);
  if (!evaluation.ok()) {
    return Freshness::Unknown;
  }
  return evaluation.value().freshness;
}

AcceptanceDecision EvidenceWindow::consider(const EvidenceRecord& record) {
  const Timestamp now = clock_->now();

  const Result<void> valid = record.validate();
  if (!valid.ok()) {
    AcceptanceDecision decision = refuse(AcceptanceOutcome::Invalid, valid.status().code(), valid.status().detail());
    if (refusals_.size() < refusal_capacity_) {
      refusals_.push_back(RefusedEvidence{record, decision});
    } else {
      ++dropped_refusals_;
    }
    return decision;
  }

  const AuthorityContract* contract = registry_->find(record.authority);
  if (contract == nullptr) {
    AcceptanceDecision decision = refuse(AcceptanceOutcome::Refused, ReasonCode::UnknownAuthority,
                                        "authority '" + record.authority.value() +
                                            "' is not present in the authority registry");
    if (refusals_.size() < refusal_capacity_) {
      refusals_.push_back(RefusedEvidence{record, decision});
    } else {
      ++dropped_refusals_;
    }
    return decision;
  }

  if (!contract->accepts(record.dimension, record.assertion)) {
    AcceptanceDecision decision = refuse(
        AcceptanceOutcome::Refused, ReasonCode::EvidenceRefused,
        "authority '" + record.authority.value() + "' (" + std::string(authority_role_text(contract->role)) +
            ") is not authoritative for " + std::string(dimension_text(record.dimension)) + "/" +
            std::string(assertion_text(record.assertion)) + "; its contract grants: " + contract->assertions.text());
    if (refusals_.size() < refusal_capacity_) {
      refusals_.push_back(RefusedEvidence{record, decision});
    } else {
      ++dropped_refusals_;
    }
    return decision;
  }

  if (!record.validity.contains(now)) {
    const bool before = record.validity.from.has_value() && now < record.validity.from.value();
    AcceptanceDecision decision =
        refuse(AcceptanceOutcome::Refused, before ? ReasonCode::EvidenceNotYetValid : ReasonCode::EvidenceExpired,
               before ? "evidence validity window opens at " + std::to_string(record.validity.from.value().unix_nanos())
                      : "evidence validity window closed at " +
                            std::to_string(record.validity.until.has_value() ? record.validity.until.value().unix_nanos() : 0));
    if (refusals_.size() < refusal_capacity_) {
      refusals_.push_back(RefusedEvidence{record, decision});
    } else {
      ++dropped_refusals_;
    }
    return decision;
  }

  EvidenceSlotKey key;
  key.authority = record.authority;
  key.scope = record.scope;
  key.dimension = record.dimension;
  key.assertion = record.assertion;

  const Digest digest = record.content_digest();
  AcceptanceDecision decision;
  decision.reason = ReasonCode::Ok;

  const auto watermark = watermarks_.find(key);
  if (watermark != watermarks_.end() && watermark->second.present) {
    const Watermark& mark = watermark->second;
    if (record.epoch < mark.epoch) {
      decision = refuse(AcceptanceOutcome::StaleEpoch, ReasonCode::StaleEpoch,
                        "record epoch e" + std::to_string(record.epoch.value()) + " is older than the accepted epoch e" +
                            std::to_string(mark.epoch.value()));
      if (refusals_.size() < refusal_capacity_) {
        refusals_.push_back(RefusedEvidence{record, decision});
      } else {
        ++dropped_refusals_;
      }
      return decision;
    }
    if (record.epoch == mark.epoch) {
      if (record.generation < mark.generation) {
        decision = refuse(AcceptanceOutcome::StaleGeneration, ReasonCode::StaleGeneration,
                          "record generation g" + std::to_string(record.generation.value()) +
                              " is older than the accepted generation g" + std::to_string(mark.generation.value()));
        if (refusals_.size() < refusal_capacity_) {
          refusals_.push_back(RefusedEvidence{record, decision});
        } else {
          ++dropped_refusals_;
        }
        return decision;
      }
      if (record.generation == mark.generation) {
        if (digest == mark.digest) {
          decision.outcome = AcceptanceOutcome::Duplicate;
          decision.reason = ReasonCode::DuplicateEvidence;
          decision.explanation = "generation g" + std::to_string(record.generation.value()) +
                                 " with digest " + digest.hex().substr(0, 16) + " was already applied";
          decision.affects_derived_state = false;
          return decision;
        }
        decision = refuse(AcceptanceOutcome::Conflicted, ReasonCode::ConflictingEvidence,
                          "generation g" + std::to_string(record.generation.value()) + " was already accepted with digest " +
                              mark.digest.hex().substr(0, 16) + " but this record has digest " +
                              digest.hex().substr(0, 16));
        if (refusals_.size() < refusal_capacity_) {
          refusals_.push_back(RefusedEvidence{record, decision});
        } else {
          ++dropped_refusals_;
        }
        return decision;
      }
    }
  }

  // Accepted. Epochs and generations advance monotonically; a gap or an epoch
  // reset is recorded in the explanation rather than silently tolerated.
  const bool first = watermark == watermarks_.end() || !watermark->second.present;
  decision.outcome = AcceptanceOutcome::Accepted;
  decision.affects_derived_state = true;
  if (!first) {
    const Watermark& mark = watermark->second;
    decision.epoch_advanced = record.epoch > mark.epoch;
    decision.generation_gap = !decision.epoch_advanced && record.generation.value() > mark.generation.value() + 1;
  }

  const Result<FreshnessEvaluation> freshness = evaluate_freshness(
      record.has_observed_at, record.observed_at, now, contract->staleness_budget,
      record.provenance == Provenance::Recovered);
  decision.freshness = freshness.ok() ? freshness.value().freshness : Freshness::Unknown;

  std::string explanation = "accepted " + record.authority.value() + " " + record.scope.text() + " " +
                            std::string(dimension_text(record.dimension)) + "/" +
                            std::string(assertion_text(record.assertion)) + " e" +
                            std::to_string(record.epoch.value()) + " g" + std::to_string(record.generation.value()) +
                            " = " + std::to_string(record.amount.canonical()) + " " +
                            std::string(canonical_unit_text(record.dimension));
  if (decision.epoch_advanced) {
    explanation += "; authority epoch advanced from e" + std::to_string(watermark->second.epoch.value()) +
                   " which resets the generation watermark";
  }
  if (decision.generation_gap) {
    explanation += "; generation gap: previous accepted generation was g" +
                   std::to_string(watermark->second.generation.value());
    decision.reason = ReasonCode::GenerationGap;
  }
  if (freshness.ok()) {
    explanation += "; freshness=" + std::string(freshness_text(decision.freshness));
    if (contract->staleness_budget.nanos() <= 0) {
      explanation += " (no staleness budget declared)";
    }
  }
  decision.explanation = std::move(explanation);

  Watermark updated;
  updated.epoch = record.epoch;
  updated.generation = record.generation;
  updated.digest = digest;
  updated.present = true;
  watermarks_[key] = updated;
  current_[key] = record;
  ++accepted_count_;
  return decision;
}

const Watermark* EvidenceWindow::watermark_for(const EvidenceSlotKey& key) const noexcept {
  const auto found = watermarks_.find(key);
  return found == watermarks_.end() ? nullptr : &found->second;
}

void EvidenceWindow::mark_all_recovered(Timestamp recovery_instant) {
  for (auto& entry : current_) {
    entry.second.provenance = Provenance::Recovered;
    if (!entry.second.has_observed_at) {
      entry.second.observed_at = recovery_instant;
    }
    // The content digest covers provenance, so the watermark has to move with it.
    // Without this the window would disagree with itself: re-submitting the very
    // record it just handed out would look like the same generation carrying
    // different content and be refused as ConflictingEvidence.
    const auto watermark = watermarks_.find(entry.first);
    if (watermark != watermarks_.end()) {
      watermark->second.digest = entry.second.content_digest();
    }
  }
}

void EvidenceWindow::clear() {
  current_.clear();
  watermarks_.clear();
  refusals_.clear();
  dropped_refusals_ = 0;
  accepted_count_ = 0;
}

}  // namespace co
