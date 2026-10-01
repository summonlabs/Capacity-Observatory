#pragma once
// Capacity Observatory - stable reason codes.
//
// Every public API outcome that is not a plain success carries a ReasonCode.
// Reason codes are part of the observable contract: they are stable, string
// addressed, and classified so that callers can react without string matching.

#include <cstdint>
#include <string_view>

// X(name, text, description)
#define CO_REASON_CODE_LIST(X)                                                                    \
  X(Ok, "ok", "operation completed")                                                              \
  X(InvalidArgument, "invalid-argument", "an argument is outside its accepted domain")             \
  X(InvalidIdentifier, "invalid-identifier", "identifier text is not a well formed identifier")    \
  X(MissingField, "missing-field", "a required field is absent")                                   \
  X(UnknownField, "unknown-field", "a field is not part of the accepted schema")                   \
  X(TypeMismatch, "type-mismatch", "a value has the wrong JSON/type shape")                        \
  X(MalformedInput, "malformed-input", "input bytes are malformed")                                \
  X(EmptyInput, "empty-input", "input carries no records")                                         \
  X(Overflow, "overflow", "checked arithmetic would exceed the representable range")               \
  X(Underflow, "underflow", "checked arithmetic would go below the representable range")           \
  X(DivisionByZero, "division-by-zero", "a checked division had a zero divisor")                   \
  X(NegativeAmount, "negative-amount", "a magnitude was negative where only non-negative is valid") \
  X(DimensionMismatch, "dimension-mismatch", "quantities of different dimensions were combined")   \
  X(InexactScale, "inexact-scale", "unit conversion is not exact and was refused, not rounded")    \
  X(UnknownUnit, "unknown-unit", "unit text does not denote a supported unit")                     \
  X(NonCanonicalUnit, "non-canonical-unit", "unit is not the canonical unit of its dimension")     \
  X(AmountUnknown, "amount-unknown", "an amount is unknown and cannot be treated as zero")          \
  X(UnknownAuthority, "unknown-authority", "authority is not present in the authority registry")    \
  X(UnknownLifecycle, "unknown-lifecycle", "lifecycle stage is not a known stage")                  \
  X(MalformedEvidence, "malformed-evidence", "evidence record failed structural validation")        \
  X(EvidenceNotYetValid, "evidence-not-yet-valid", "evidence validity window has not opened")       \
  X(EvidenceExpired, "evidence-expired", "evidence validity window has closed")                     \
  X(StaleGeneration, "stale-generation", "evidence generation is older than the accepted watermark") \
  X(StaleEpoch, "stale-epoch", "evidence epoch is older than the accepted authority epoch")          \
  X(ConflictingEvidence, "conflicting-evidence", "an equal generation carries a different digest")   \
  X(DuplicateEvidence, "duplicate-evidence", "an identical record was already applied")              \
  X(GenerationGap, "generation-gap", "accepted generation skipped predecessors")                     \
  X(RecoveredNotFresh, "recovered-not-fresh", "evidence was recovered from persistence, not re-observed") \
  X(EvidenceRefused, "evidence-refused", "evidence was refused and did not affect derived state")     \
  X(UnknownNameplate, "unknown-nameplate", "nameplate capacity is unknown, not zero")                 \
  X(UnknownInstalled, "unknown-installed", "installed capacity is unknown, not zero")                 \
  X(UnknownCommitted, "unknown-committed", "committed capacity is unknown, not zero")                 \
  X(UnknownReserved, "unknown-reserved", "reserved capacity is unknown, not zero")                    \
  X(UnknownExclusions, "unknown-exclusions", "exclusion capacity is unknown, not zero")               \
  X(Overcommitted, "overcommitted", "allocations exceed the capacity they draw from")                 \
  X(ResidualUnallocated, "residual-unallocated", "serviceable capacity is not explained by any class") \
  X(ResidualGovernance, "residual-governance", "declared governed ceiling disagrees with nameplate minus exclusions") \
  X(AvailableMismatch, "available-mismatch", "declared available capacity disagrees with computed available capacity") \
  X(UndeterminedCapacity, "undetermined-capacity", "derived capacity is indeterminate for this scope") \
  X(NoTopology, "no-topology", "no topology is known for the scope")                                   \
  X(NoEnclosure, "no-enclosure", "scope has no enclosure with free placement capacity")                \
  X(ZeroProfile, "zero-profile", "placement profile demands nothing")                                  \
  X(IncompatibleProfile, "incompatible-profile", "placement profile cannot be satisfied by any enclosure shape") \
  X(IncompatibleDimensions, "incompatible-dimensions", "enclosure and profile dimensional budgets are incomparable") \
  X(BindingConstraint, "binding-constraint", "a specific dimension binds realizable capacity")          \
  X(StrandedCapacity, "stranded-capacity", "capacity is physically present but structurally unusable")   \
  X(FragmentedCapacity, "fragmented-capacity", "aggregate free capacity exceeds realizable placement")   \
  X(EnclosureOverflow, "enclosure-overflow", "enclosure budget would be exceeded")                        \
  X(SlotOverflow, "slot-overflow", "rack unit slot budget would be exceeded")                            \
  X(NotFound, "not-found", "the requested object does not exist")                                        \
  X(AlreadyExists, "already-exists", "the object already exists")                                        \
  X(LockHeld, "lock-held", "another process holds the exclusive store lock")                              \
  X(BadMagic, "bad-magic", "file does not begin with the store magic")                                    \
  X(UnsupportedVersion, "unsupported-version", "file version is not supported by this build")             \
  X(VersionMismatch, "version-mismatch", "declared version disagrees with the observed layout")           \
  X(InteriorCorruption, "interior-corruption", "corruption was found before the tail and the file was rejected") \
  X(TornTailRecovered, "torn-tail-recovered", "an incomplete trailing record was discarded conservatively") \
  X(IntegrityMismatch, "integrity-mismatch", "stored integrity value does not match the payload")          \
  X(ShortWrite, "short-write", "a durable write completed fewer bytes than requested")                       \
  X(FlushFailure, "flush-failure", "durability flush failed")                                                 \
  X(PublishFailure, "publish-failure", "atomic publication of a file failed")                                  \
  X(IoFailure, "io-failure", "an operating system I/O call failed")                                            \
  X(StaleEpochReplay, "stale-epoch-replay", "a superseded epoch was presented and refused")                     \
  X(EpochExhausted, "epoch-exhausted", "epoch counter cannot be advanced further")                              \
  X(IdempotentReplay, "idempotent-replay", "an identical mutation identity was already committed")               \
  X(IdempotencyConflict, "idempotency-conflict", "a mutation identity was reused with different content")         \
  X(StoreClosed, "store-closed", "the store is closed")                                                           \
  X(StoreAlreadyOpen, "store-already-open", "the store is already open")                                          \
  X(RecordTooLarge, "record-too-large", "a record exceeds the configured maximum frame size")                      \
  X(CapacityExceeded, "capacity-exceeded", "a bounded resource limit was reached")                                 \
  X(QueueFull, "queue-full", "the bounded ingest queue is full")                                                   \
  X(Cancelled, "cancelled", "the operation was cancelled")                                                         \
  X(ShuttingDown, "shutting-down", "the runtime is shutting down")                                                  \
  X(UpgradeRefused, "upgrade-refused", "a read guard cannot be upgraded to a write guard")                          \
  X(LockOrderViolation, "lock-order-violation", "lock acquisition violated the documented lock order")               \
  X(WorkerFailed, "worker-failed", "a background worker terminated abnormally")                                     \
  X(NotSupported, "not-supported", "the capability is not supported on this platform or build")                      \
  X(ParseError, "parse-error", "JSON text could not be parsed")                                                     \
  X(TrailingGarbage, "trailing-garbage", "bytes remained after the end of a complete value")                        \
  X(DuplicateKey, "duplicate-key", "a JSON object repeated a key")                                                  \
  X(UnknownCommand, "unknown-command", "the command line named an unknown command")                                 \
  X(MissingOption, "missing-option", "a required command line option is absent")                                     \
  X(InvalidOptionValue, "invalid-option-value", "a command line option value is invalid")                            \
  X(Usage, "usage", "command line usage is incorrect")                                                              \
  X(InvariantViolation, "invariant-violation", "an internal invariant was violated")                                \
  X(InternalError, "internal-error", "an unexpected internal failure occurred")

namespace co {

enum class ReasonCode : std::uint16_t {
#define CO_REASON_ENUM(name, text, desc) name,
  CO_REASON_CODE_LIST(CO_REASON_ENUM)
#undef CO_REASON_ENUM
};

// Coarse classification used for deterministic exit codes and callers that
// only need to know whether to retry, refuse, or fail.
enum class ReasonClass : std::uint8_t {
  Ok = 0,            // success
  Refused = 1,       // a deliberate, explainable refusal of the request
  Indeterminate = 2, // the truth is unknown/stale/conflicting - not an error
  Failed = 3         // integrity, I/O, or internal failure
};

[[nodiscard]] constexpr std::string_view reason_text(ReasonCode code) noexcept {
  switch (code) {
#define CO_REASON_TEXT(name, text, desc) case ReasonCode::name: return text;
    CO_REASON_CODE_LIST(CO_REASON_TEXT)
#undef CO_REASON_TEXT
  }
  return "unknown-reason";
}

[[nodiscard]] constexpr std::string_view reason_description(ReasonCode code) noexcept {
  switch (code) {
#define CO_REASON_DESC(name, text, desc) case ReasonCode::name: return desc;
    CO_REASON_CODE_LIST(CO_REASON_DESC)
#undef CO_REASON_DESC
  }
  return "unrecognized reason code";
}

[[nodiscard]] constexpr ReasonClass reason_class(ReasonCode code) noexcept {
  switch (code) {
    case ReasonCode::Ok:
      return ReasonClass::Ok;
    case ReasonCode::EvidenceNotYetValid:
    case ReasonCode::EvidenceExpired:
    case ReasonCode::StaleGeneration:
    case ReasonCode::StaleEpoch:
    case ReasonCode::ConflictingEvidence:
    case ReasonCode::GenerationGap:
    case ReasonCode::RecoveredNotFresh:
    case ReasonCode::EvidenceRefused:
    case ReasonCode::UnknownNameplate:
    case ReasonCode::UnknownInstalled:
    case ReasonCode::UnknownCommitted:
    case ReasonCode::UnknownReserved:
    case ReasonCode::UnknownExclusions:
    case ReasonCode::UndeterminedCapacity:
    case ReasonCode::AmountUnknown:
      return ReasonClass::Indeterminate;
    case ReasonCode::Overflow:
    case ReasonCode::Underflow:
    case ReasonCode::InteriorCorruption:
    case ReasonCode::IntegrityMismatch:
    case ReasonCode::ShortWrite:
    case ReasonCode::FlushFailure:
    case ReasonCode::PublishFailure:
    case ReasonCode::IoFailure:
    case ReasonCode::WorkerFailed:
    case ReasonCode::InvariantViolation:
    case ReasonCode::InternalError:
      return ReasonClass::Failed;
    default:
      return ReasonClass::Refused;
  }
}

}  // namespace co
