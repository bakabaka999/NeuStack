# Integration Guide

How to use NeuStack as a library in your own project.

## Option 1: CMake `find_package` (Recommended)

### From Source (add_subdirectory)

The simplest way — add NeuStack as a subdirectory in your project:

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(MyApp LANGUAGES CXX)

add_subdirectory(third_party/NeuStack)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE neustack_lib)
```

```
my-project/
├── CMakeLists.txt
├── main.cpp
└── third_party/
    └── NeuStack/       # git clone or git submodule
```

### From Install (find_package)

If you've installed NeuStack (via `cmake --install` or from a release archive):

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyApp LANGUAGES CXX)

find_package(NeuStack 1.4 REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE NeuStack::neustack_lib)
```

Point CMake to the install location:

```bash
cmake -B build -DCMAKE_PREFIX_PATH=/path/to/neustack
```

## Option 2: Using Release Archives

Download the pre-built archive for your platform from [GitHub Releases](https://github.com/bakabaka999/NeuStack/releases).

### Archive Contents

```
neustack-<platform>-v1.4.0/
├── lib/
│   ├── libneustack_lib.a          # Static library (Unix)
│   ├── neustack_lib.lib           # Static library (Windows)
│   └── cmake/NeuStack/            # CMake package config
│       ├── NeuStackConfig.cmake
│       ├── NeuStackConfigVersion.cmake
│       └── NeuStackTargets.cmake
├── include/
│   └── neustack/                  # All public headers
└── models/                        # Pre-trained ONNX models (optional)
    ├── orca_actor.onnx
    ├── bandwidth_predictor.onnx
    ├── anomaly_detector.onnx
    └── security_anomaly.onnx
```

### Setup

```bash
# Extract
tar xzf neustack-linux-x64-v1.4.0.tar.gz

# Use in your CMake project
cmake -B build -DCMAKE_PREFIX_PATH=$(pwd)/neustack-linux-x64-v1.4.0
```

Then in your `CMakeLists.txt`:

```cmake
find_package(NeuStack 1.4 REQUIRED)
target_link_libraries(my_app PRIVATE NeuStack::neustack_lib)
```

### Manual Linking (without CMake)

If you're not using CMake:

```bash
# Compile
g++ -std=c++20 -I/path/to/neustack/include -c main.cpp

# Link
g++ main.o -L/path/to/neustack/lib -lneustack_lib -o my_app

# Windows (MSVC)
cl /std:c++20 /I path\to\neustack\include main.cpp /link path\to\neustack\lib\neustack_lib.lib ws2_32.lib iphlpapi.lib
```

## Option 3: Git Submodule

```bash
git submodule add https://github.com/bakabaka999/NeuStack.git third_party/NeuStack
```

Then use `add_subdirectory` as shown in Option 1.

## Platform-Specific Notes

### macOS
- Requires `CoreFoundation` and `SystemConfiguration` frameworks (linked automatically by CMake)
- Needs root to create utun device

### Linux
- Needs `/dev/net/tun` (load module: `sudo modprobe tun`)
- Needs root to create TUN device

### Windows
- Needs `wintun.dll` in the working directory ([download](https://www.wintun.net/))
- Links `ws2_32` and `iphlpapi` automatically
- Needs Administrator to create Wintun adapter

## AI Features

To use AI features (Orca, bandwidth prediction, anomaly detection), you need ONNX Runtime:

```bash
# Download ONNX Runtime
./scripts/download/download_onnxruntime.sh

# Build with AI enabled
cmake -B build -DNEUSTACK_ENABLE_AI=ON
```

When AI is enabled, the `NEUSTACK_AI_ENABLED` macro is defined. You can check it in your code:

```cpp
neustack::StackConfig config;
config.orca_model_path = "models/orca_actor.onnx";
config.bandwidth_model_path = "models/bandwidth_predictor.onnx";
config.anomaly_model_path = "models/anomaly_detector.onnx";
config.security_model_path = "models/security_anomaly.onnx";

auto stack = neustack::NeuStack::create(config);

#ifdef NEUSTACK_AI_ENABLED
if (stack && stack->ai_enabled()) {
    const auto& agent = stack->tcp().agent();
    std::printf("AI state: %s\n", neustack::agent_state_name(agent.state()));
}
#endif
```

ONNX model files are passed individually at runtime through `StackConfig`:

```cpp
neustack::StackConfig config;
config.orca_model_path = "models/orca_actor.onnx";
config.bandwidth_model_path = "models/bandwidth_predictor.onnx";
config.anomaly_model_path = "models/anomaly_detector.onnx";
config.security_model_path = "models/security_anomaly.onnx";
auto stack = neustack::NeuStack::create(config);
```

## Minimal Example

```cpp
#include "neustack/neustack.hpp"

int main() {
    auto stack = neustack::NeuStack::create();
    if (!stack) return 1;

    stack->http_server().get("/hello", [](const auto&) {
        return neustack::HttpResponse()
            .content_type("text/plain")
            .set_body("Hello from NeuStack!\n");
    });
    stack->http_server().listen(8080);

    stack->run();
}
```
