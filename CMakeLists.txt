cmake_minimum_required(VERSION 3.20)

project(agent_sched_sim VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(agent_sched_sim
  src/main.cpp
  src/sim/config.cpp
  src/sim/random.cpp
  src/sim/provider.cpp
  src/sim/workflow.cpp
)

target_compile_features(agent_sched_sim PRIVATE cxx_std_20)
target_include_directories(agent_sched_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(MSVC)
  target_compile_options(agent_sched_sim PRIVATE /W4 /permissive-)
else()
  target_compile_options(agent_sched_sim PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()
