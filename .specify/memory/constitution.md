<!--
Sync Impact Report:
- Version change: N/A (initial) → 1.0.0
- Modified principles: N/A (initial creation)
- Added sections:
  - Core Principles: I-VI (Test-Driven Development, Concurrency Safety, Code Quality, Best Practices, Documentation Standards, Bug Prevention)
  - Quality Standards
  - Development Workflow
  - Governance
- Removed sections: N/A (initial creation)
- Templates requiring updates:
  - ✅ plan-template.md (Constitution Check section compatible - no changes needed)
  - ✅ spec-template.md (requirements and test scenarios align - no changes needed)
  - ✅ tasks-template.md (updated to enforce TDD: changed tests from "OPTIONAL" to "REQUIRED per TDD principle")
- Follow-up TODOs: None
-->

# CppGrpcDb2 Constitution

## Core Principles

### I. Test-Driven Development (NON-NEGOTIABLE)

TDD MUST be followed for all feature development and bug fixes:
- Tests MUST be written before implementation code
- Tests MUST fail initially (Red phase)
- Implementation MUST make tests pass (Green phase)
- Code MUST be refactored while keeping tests passing (Refactor phase)
- User approval MUST be obtained on test design before implementation begins

**Rationale**: TDD ensures testability by design, prevents untested code paths, provides living documentation, and catches regressions early. The red-green-refactor cycle enforces disciplined development and creates confidence in modifications.

### II. Concurrency Safety

All code touching shared state MUST be explicitly reviewed for concurrency issues:
- Resource pools, connection management, and shared data structures MUST use appropriate synchronization primitives (mutexes, condition variables, atomics)
- Lock acquisition order MUST be documented and consistent to prevent deadlocks
- RAII patterns MUST be used for lock management (std::lock_guard, std::unique_lock)
- Thread-safe interfaces MUST be clearly documented
- Race conditions, data races, and deadlocks MUST be identified and resolved in code reviews

**Rationale**: Multi-threaded C++ applications with database connections and gRPC servers face inherent concurrency challenges. Explicit attention prevents production crashes, data corruption, and hard-to-reproduce bugs.

### III. Code Quality

Every code change MUST undergo careful review before merging:
- Code reviews MUST check for correctness, clarity, and maintainability
- Complex algorithms or non-obvious logic MUST include explanatory comments
- Memory safety MUST be verified (no leaks, dangling pointers, buffer overflows)
- Error handling MUST be comprehensive and appropriate (exceptions, error codes, validation)
- Performance implications MUST be considered for hot paths and resource usage

**Rationale**: C++ allows low-level control but requires discipline. Careful review catches memory safety issues, undefined behavior, and logic errors that compilers cannot detect.

### IV. Best Practices

Modern C++ best practices MUST be adopted throughout the codebase:
- Use smart pointers (std::unique_ptr, std::shared_ptr) instead of raw pointers for ownership
- Prefer RAII for resource management (files, locks, database connections)
- Use standard library containers and algorithms over manual implementations
- Apply const correctness and avoid mutable global state
- Follow naming conventions and code style consistently (as defined in project guidelines)
- Avoid premature optimization; measure before optimizing

**Rationale**: Best practices reduce bugs, improve readability, and leverage the C++ standard library's battle-tested implementations. Consistency enables team efficiency and code maintainability.

### V. Documentation Standards

Code and architectural decisions MUST be documented:
- Public APIs MUST have clear documentation of parameters, return values, and exceptions
- Complex modules MUST have accompanying design documents (e.g., doc/resource_pool.md)
- Architecture decisions MUST be documented with rationale
- Documentation MUST be updated whenever functionality changes
- Examples MUST be provided for non-trivial usage patterns

**Rationale**: C++ lacks runtime introspection; good documentation is essential for understanding intent, usage, and constraints. Design documents prevent knowledge loss and enable faster onboarding.

### VI. Bug Prevention

Bug-free code MUST be the standard, achieved through multiple verification layers:
- Static analysis tools MUST be run regularly (compiler warnings at -Wall -Wextra, clang-tidy, cppcheck)
- Integration tests MUST cover critical paths (database operations, gRPC services, resource pooling)
- Edge cases and error paths MUST be explicitly tested
- Memory safety tools MUST be used during development (AddressSanitizer, ThreadSanitizer, Valgrind)
- Code coverage MUST be tracked and improved over time

**Rationale**: Bugs in C++ can cause crashes, security vulnerabilities, and data loss. Multiple verification layers catch issues that individual techniques miss.

## Quality Standards

All code submitted to the repository MUST meet these quality gates:

- **Compilation**: Code MUST compile without errors on supported platforms (macOS, Linux)
- **Warnings**: Code MUST compile without warnings when using -Wall -Wextra -Werror
- **Tests**: All existing tests MUST pass; new features MUST have accompanying tests
- **Memory Safety**: Code MUST pass AddressSanitizer and ThreadSanitizer checks
- **Performance**: Performance-critical code MUST not introduce regressions (benchmark where applicable)
- **Documentation**: Public interfaces and design changes MUST be documented

**Enforcement**: Quality gates MUST be verified before code review approval. Automated CI checks SHOULD be established for compilation, tests, and static analysis.

## Development Workflow

The following workflow MUST be followed for all development:

1. **Feature Specification**: Clearly define requirements and test scenarios before coding
2. **Test Design**: Write test cases that specify expected behavior; get user approval
3. **Red Phase**: Verify tests fail for the right reasons (missing functionality, not broken tests)
4. **Green Phase**: Implement minimal code to make tests pass; prioritize correctness over optimization
5. **Refactor Phase**: Improve code structure, readability, and performance while keeping tests passing
6. **Code Review**: Submit for peer review with focus on TDD compliance, concurrency safety, code quality, and best practices
7. **Documentation Update**: Update or create documentation reflecting changes (code comments, design docs, README sections)
8. **Integration Verification**: Run full test suite and verify integration with existing features

**Checkpoints**:
- Tests designed and approved → Begin Red phase
- Tests failing correctly → Begin Green phase
- Tests passing → Begin Refactor phase
- Code reviewed and approved → Begin Documentation update
- Documentation complete → Ready to merge

## Governance

This constitution supersedes all other development practices and conventions.

**Amendment Procedure**:
- Amendments MUST be proposed in writing with rationale and impact analysis
- Amendments MUST be reviewed by project maintainers
- Amendments MUST document affected templates and workflows
- Version number MUST be incremented according to semantic versioning:
  - MAJOR: Backward incompatible governance or principle removals/redefinitions
  - MINOR: New principle/section added or materially expanded guidance
  - PATCH: Clarifications, wording fixes, non-semantic refinements

**Compliance Review**:
- All pull requests MUST verify compliance with constitution principles
- Code reviews MUST explicitly check TDD adherence, concurrency safety, and documentation
- Complexity and deviations from best practices MUST be justified and documented
- Regular retrospectives SHOULD assess adherence and identify improvement opportunities

**Continuous Improvement**:
- Constitution SHOULD be revisited quarterly or after major project milestones
- Team members SHOULD propose improvements based on lessons learned
- Changes MUST be captured in version history with clear rationale

**Version**: 1.0.0 | **Ratified**: 2025-11-21 | **Last Amended**: 2025-11-21
