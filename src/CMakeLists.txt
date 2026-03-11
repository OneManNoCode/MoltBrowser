# Copyright 2025 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
#
# CMakeLists.txt for standalone building and testing of MoltBrowser AI modules.
# This allows developing and testing AI components independently of the
# full Chromium build. For production, use the GN build system.

cmake_minimum_required(VERSION 3.16)
project(MoltBrowserAI VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
set(MOLT_AI_RUNTIME_SOURCES
    molt_ai/runtime/browser_ai_runtime.cc
    molt_ai/runtime/prompt_router.cc
)

set(MOLT_AI_DOM_SOURCES
    molt_ai/dom/dom_interpreter.cc
)

set(MOLT_AI_AGENTS_SOURCES
    molt_ai/agents/agent_engine.cc
)

set(MOLT_AI_SECURITY_SOURCES
    molt_ai/security/action_validator.cc
)

set(MOLT_AI_MEMORY_SOURCES
    molt_ai/memory/memory_engine.cc
)

set(MOLT_AI_PERSONAS_SOURCES
    molt_ai/personas/persona_system.cc
)

set(MOLT_AI_MODELS_SOURCES
    molt_ai/models/model_manager.cc
)

set(MOLT_AI_API_SOURCES
    molt_ai/api/browser_ai_api.cc
)

set(MOLTSHIELD_SOURCES
    moltshield/moltshield.cc
)

set(MOLTNET_SOURCES
    moltnet/moltnet.cc
)

# Include paths
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)

# Libraries
add_library(molt_ai_runtime STATIC ${MOLT_AI_RUNTIME_SOURCES})
add_library(molt_ai_dom STATIC ${MOLT_AI_DOM_SOURCES})
add_library(molt_ai_agents STATIC ${MOLT_AI_AGENTS_SOURCES})
add_library(molt_ai_security STATIC ${MOLT_AI_SECURITY_SOURCES})
add_library(molt_ai_memory STATIC ${MOLT_AI_MEMORY_SOURCES})
add_library(molt_ai_personas STATIC ${MOLT_AI_PERSONAS_SOURCES})
add_library(molt_ai_models STATIC ${MOLT_AI_MODELS_SOURCES})
add_library(molt_ai_api STATIC ${MOLT_AI_API_SOURCES})
add_library(moltshield STATIC ${MOLTSHIELD_SOURCES})
add_library(moltnet STATIC ${MOLTNET_SOURCES})

# Dependencies
target_link_libraries(molt_ai_agents molt_ai_runtime molt_ai_dom)
target_link_libraries(molt_ai_security molt_ai_agents)
target_link_libraries(molt_ai_models molt_ai_runtime)
target_link_libraries(molt_ai_api molt_ai_runtime)

# Platform-specific
if(APPLE)
    # macOS frameworks
    find_library(FOUNDATION_FRAMEWORK Foundation)
    find_library(METAL_FRAMEWORK Metal)
    target_link_libraries(molt_ai_runtime ${FOUNDATION_FRAMEWORK})
endif()

# Combined library
add_library(moltbrowser_ai INTERFACE)
target_link_libraries(moltbrowser_ai INTERFACE
    molt_ai_runtime
    molt_ai_dom
    molt_ai_agents
    molt_ai_security
    molt_ai_memory
    molt_ai_personas
    molt_ai_models
    molt_ai_api
    moltshield
    moltnet
)

# Print summary
message(STATUS "MoltBrowser AI modules configured")
message(STATUS "  Platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Architecture: ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
