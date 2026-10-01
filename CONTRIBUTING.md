# Contributing to Capacity Observatory

Capacity Observatory is a C++20 facility capacity observability runtime maintained by
Summon Software Labs. This document describes how changes to this repository are
submitted, reviewed, and validated.

## Licensing of contributions

Contributions are accepted under the Apache License, Version 2.0. The terms are
inbound = outbound: the license you receive the project under is the license you
submit your changes under, with no additional conditions attached to either side.

There is no Contributor License Agreement and no copyright assignment. You retain
the copyright on your contribution, and submitting it does not transfer ownership to
Summon Software Labs or to any other party. By submitting a change you confirm that
you have the right to license that work under the Apache License, Version 2.0.

## Commit messages

Commit messages are public-facing and permanent. Keep them concise and neutral, and
describe what changed and why.

- Do not add `Co-authored-by` trailers.
- Do not add AI attribution of any kind.
- Do not add generated-by trailers.
- Do not include internal workflow notes, agent transcripts, planning text, or review
  commentary. A commit message describes the change itself.

## Build requirements

- CMake 3.25 or newer.
- A C++20 compiler. The build uses `CMAKE_CXX_STANDARD 20` with compiler extensions
  disabled, so the compiler must support C++20 in conforming mode.
- Ninja is the generator used by the commands below.

## Build and test

Release build:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Debug build:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Both configurations are expected to build cleanly and pass the full test suite.

## Code quality expectations

A change is ready to review when all of the following hold.

**Warnings.** Release and Debug builds must be warning-clean with warnings treated as
errors: `/W4 /WX` on MSVC, or
`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror` on GCC and Clang.
The library and command line targets additionally enable `-Wshadow` and
`-Wold-style-cast` under GCC and Clang.

**Tests.** The full test suite must pass, and no test may introduce a timeout. Tests
must be deterministic and must not depend on wall-clock timing; time enters through
the injected `co::Clock` interface so that a test supplies the instants it needs
rather than reading the system clock. New behaviour needs tests, using unit,
integration, or property style coverage as appropriate to the change.

**Explicit outcomes.** Public APIs return `co::Result` values carrying a stable
`co::ReasonCode` instead of throwing for domain failures. Exceptions are not the
mechanism for reporting expected outcomes, and reason codes are part of the observable
contract: callers depend on their meaning, so an existing code keeps its meaning and
new codes are added deliberately.

**Checked arithmetic.** All capacity magnitudes use checked integer arithmetic.
Overflow, underflow, division by zero, and inexact unit conversion are refused and
reported rather than wrapped, truncated, or rounded.

**Explicit unknown states.** Unknown, stale, and conflicting states remain explicit
and must never be coerced to zero. An absent magnitude is not zero, an undetermined
derived value is not zero, and a stale or conflicting observation is reported as such
instead of being folded into a numeric result. Derived capacity that cannot be
established is indeterminate, and the corresponding result says so.

## Documentation

Documentation must describe the repository that actually exists. Describe the
behaviour, commands, and interfaces that are present in the tree, and keep the text
consistent with the code in the same change.

Documentation in this repository is public-facing and must not contain internal agent
or workflow directives: no instructions addressed to automated tools, no process
prompts, and no internal task routing.
