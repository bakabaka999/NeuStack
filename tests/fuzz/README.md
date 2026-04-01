# Parser Fuzzing

NeuStack ships four libFuzzer targets for parser hot paths:

- `fuzz_http_parser`
- `fuzz_dns_parser`
- `fuzz_ipv4_parser`
- `fuzz_tcp_parser`

## Configure

```bash
cmake -B build-fuzz \
  -DNEUSTACK_BUILD_FUZZERS=ON \
  -DNEUSTACK_BUILD_TESTS=OFF \
  -DNEUSTACK_BUILD_EXAMPLES=OFF \
  -DNEUSTACK_BUILD_TOOLS=OFF
```

Notes:

- `NEUSTACK_BUILD_FUZZERS=ON` enables libFuzzer targets
- parser fuzzing automatically enables `address` + `undefined` sanitizers for the build
- Clang / AppleClang is required because the targets link with `-fsanitize=fuzzer`

## Build

```bash
cmake --build build-fuzz --target \
  fuzz_http_parser fuzz_dns_parser fuzz_ipv4_parser fuzz_tcp_parser
```

## Run

```bash
./build-fuzz/fuzz/fuzz_http_parser build-fuzz/fuzz-corpus/http
./build-fuzz/fuzz/fuzz_dns_parser build-fuzz/fuzz-corpus/dns
./build-fuzz/fuzz/fuzz_ipv4_parser build-fuzz/fuzz-corpus/ipv4
./build-fuzz/fuzz/fuzz_tcp_parser build-fuzz/fuzz-corpus/tcp
```

Quick smoke runs:

```bash
./build-fuzz/fuzz/fuzz_http_parser -runs=100
./build-fuzz/fuzz/fuzz_dns_parser -runs=100
./build-fuzz/fuzz/fuzz_ipv4_parser -runs=100
./build-fuzz/fuzz/fuzz_tcp_parser -runs=100
```
