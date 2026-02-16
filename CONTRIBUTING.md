# Contributing to NeuStack

Thanks for your interest in NeuStack! Here's how to get started.

## Development Environment

```bash
# Dependencies
# - CMake >= 3.20
# - C++20 compiler (Clang >= 14 / GCC >= 11 / MSVC 2019+)
# - Optional: ONNX Runtime (AI inference), Catch2 v3 (tests)

# Clone & build
git clone https://github.com/bakabaka999/NeuStack.git
cd NeuStack
cmake -B build -G Ninja
cmake --build build

# Enable AI
./scripts/download/download_onnxruntime.sh
cmake -B build -DNEUSTACK_ENABLE_AI=ON
cmake --build build

# Run tests
cd build && ctest --output-on-failure
```

## Commit Convention

Commit message format: `<type>(<scope>): <description>`

| type | Usage |
|------|-------|
| `feat` | New feature |
| `fix` | Bug fix |
| `refactor` | Refactoring (no behavior change) |
| `test` | Test-related |
| `docs` | Documentation |
| `perf` | Performance improvement |
| `chore` | Build/toolchain |

Examples:
- `feat(firewall): add auto-escalation cooldown`
- `fix(tcp): correct RST handling in TIME_WAIT`
- `test(ai): add NetworkAgent state transition tests`

## Code Style

- **C++20**, follow existing project conventions
- Naming: `snake_case` for functions/variables, `PascalCase` for classes, `_prefix` for private members
- Headers in `include/neustack/`, implementations in `src/`
- Public APIs require Doxygen comments
- Avoid dynamic allocation on hot paths (use `FixedPool`, `SPSCQueue`, etc.)

## Branch Strategy

- `main` — stable branch
- Feature branches from `main`: `feat/xxx`, `fix/xxx`
- Ensure tests pass before submitting a PR

## Testing

New features must include corresponding tests:

```bash
# Unit tests
ctest -R "unit"

# Integration tests
ctest -R "Integration"

# Benchmarks
ctest -R "Benchmark"
```

Test files go in the `tests/` directory, using Catch2 v3.

## Project Structure Quick Reference

```
include/neustack/     # Public headers
src/                  # Implementations
tests/                # Tests
training/             # Python training code
models/               # ONNX models
scripts/              # Shell scripts
docs/api/             # API documentation
```

## Questions?

Open an issue to discuss.
