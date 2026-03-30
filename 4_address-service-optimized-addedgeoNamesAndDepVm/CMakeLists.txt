cmake_minimum_required(VERSION 3.16)
project(address-service VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(OPT_FLAGS "-O3 -march=native -funroll-loops -ffast-math")
set(CMAKE_CXX_FLAGS_RELEASE        "${CMAKE_CXX_FLAGS_RELEASE} ${OPT_FLAGS}")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} ${OPT_FLAGS}")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# ==============================================================================
#  Options
# ==============================================================================
option(ENABLE_LLM      "Enable LLM fallback via llama.cpp"     OFF)
option(ENABLE_TESTS    "Build test suite"                       ON)
option(ENABLE_REDIS    "Enable Redis L2 cache"                  OFF)
option(ENABLE_MIMALLOC "Use mimalloc as global allocator"       ON)
option(ENABLE_OPENMP   "Enable OpenMP for batch parallelism"    ON)

# ==============================================================================
#  Dependencies
# ==============================================================================
find_package(Drogon REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPOSTAL REQUIRED libpostal)

# Arrow CSV reader (built from source)
set(ARROW_ROOT "$ENV{HOME}/FAISS-Actual-26June25/arrow/cpp")
set(ARROW_INCLUDE_DIR      "${ARROW_ROOT}/src")
set(ARROW_GENERATED_INCLUDE "${ARROW_ROOT}/build/src")
set(ARROW_LIB_DIR           "${ARROW_ROOT}/build/release")
find_library(ARROW_LIB arrow HINTS ${ARROW_LIB_DIR} REQUIRED)
message(STATUS "Arrow lib : ${ARROW_LIB}")

if(ENABLE_MIMALLOC)
  find_package(mimalloc QUIET)
  if(mimalloc_FOUND)
    message(STATUS "mimalloc found")
    add_definitions(-DUSE_MIMALLOC=1)
  else()
    pkg_check_modules(MIMALLOC mimalloc)
    if(MIMALLOC_FOUND)
      add_definitions(-DUSE_MIMALLOC=1)
    else()
      message(WARNING "mimalloc not found — using system malloc")
      set(ENABLE_MIMALLOC OFF)
    endif()
  endif()
endif()

if(ENABLE_OPENMP)
  find_package(OpenMP QUIET)
  if(OpenMP_CXX_FOUND)
    message(STATUS "OpenMP found")
    add_definitions(-D_OPENMP=1)
  else()
    message(STATUS "OpenMP not found — using std::async")
    set(ENABLE_OPENMP OFF)
  endif()
endif()

if(ENABLE_LLM)
  find_library(LLAMA_LIB llama HINTS /usr/local/lib)
  find_path(LLAMA_INCLUDE llama.h HINTS /usr/local/include)
  if(LLAMA_LIB AND LLAMA_INCLUDE)
    message(STATUS "llama.cpp found: ${LLAMA_LIB}")
    add_definitions(-DENABLE_LLM=1)
  else()
    message(WARNING "llama.cpp not found, LLM fallback disabled")
    set(ENABLE_LLM OFF)
  endif()
endif()

if(ENABLE_REDIS)
  pkg_check_modules(HIREDIS hiredis)
  if(HIREDIS_FOUND)
    add_definitions(-DENABLE_REDIS=1)
  else()
    message(WARNING "hiredis not found, Redis cache disabled")
    set(ENABLE_REDIS OFF)
  endif()
endif()

# ==============================================================================
#  Sources
#  NOTE: LLMFallback.cc is ALWAYS compiled — headers are included everywhere.
#        When ENABLE_LLM=OFF, the .cc compiles to a no-op stub (guarded by
#        #ifdef ENABLE_LLM inside the file). Removing it from SOURCES would
#        cause linker errors for LLMFallback symbols used in controllers.
# ==============================================================================
set(SOURCES
  src/main.cc
  src/services/AddressParser.cc
  src/services/PreProcessor.cc
  src/services/RuleEngine.cc
  src/services/ConfidenceScorer.cc
  src/services/CacheManager.cc
  src/services/MetricsCollector.cc
  src/services/LLMFallback.cc
  src/controllers/ParseController.cc
  src/controllers/BatchController.cc
  src/controllers/HealthController.cc
  src/controllers/MetricsController.cc
  src/controllers/ApiKeyFilter.cc
  src/controllers/JwtAuthFilter.cc
  src/controllers/AuthFilter.cc
  src/controllers/EnrichController.cc
  src/services/GeoNamesDB.cc
  src/controllers/GeoEnrichController.cc
)

# ==============================================================================
#  Main executable
# ==============================================================================
add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${LIBPOSTAL_INCLUDE_DIRS}
  ${ARROW_INCLUDE_DIR}
  ${ARROW_GENERATED_INCLUDE}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
  Drogon::Drogon
  ${ARROW_LIB}
  ${LIBPOSTAL_LIBRARIES}
  pthread
)

# Tell linker where Arrow .so lives at runtime
set_target_properties(${PROJECT_NAME} PROPERTIES
  BUILD_RPATH  "${ARROW_LIB_DIR}"
  INSTALL_RPATH "${ARROW_LIB_DIR}"
)

if(ENABLE_MIMALLOC)
  if(mimalloc_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE mimalloc)
  elseif(MIMALLOC_FOUND)
    target_include_directories(${PROJECT_NAME} PRIVATE ${MIMALLOC_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${MIMALLOC_LIBRARIES})
  endif()
endif()

if(ENABLE_OPENMP AND OpenMP_CXX_FOUND)
  target_link_libraries(${PROJECT_NAME} PRIVATE OpenMP::OpenMP_CXX)
endif()

if(ENABLE_LLM)
  target_include_directories(${PROJECT_NAME} PRIVATE ${LLAMA_INCLUDE})
  target_link_libraries(${PROJECT_NAME} PRIVATE ${LLAMA_LIB})
endif()

if(ENABLE_REDIS)
  target_include_directories(${PROJECT_NAME} PRIVATE ${HIREDIS_INCLUDE_DIRS})
  target_link_libraries(${PROJECT_NAME} PRIVATE ${HIREDIS_LIBRARIES})
endif()

set_source_files_properties(
  src/services/CacheManager.cc
  src/services/PreProcessor.cc
  src/services/RuleEngine.cc
  src/services/ConfidenceScorer.cc
  src/services/MetricsCollector.cc
  src/controllers/BatchController.cc
  PROPERTIES COMPILE_FLAGS "-O3"
)

message(STATUS "")
message(STATUS "=== address-service optimization summary ===")
message(STATUS "  Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  LTO          : ${CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE}")
message(STATUS "  mimalloc     : ${ENABLE_MIMALLOC}")
message(STATUS "  OpenMP       : ${ENABLE_OPENMP}")
message(STATUS "  LLM fallback : ${ENABLE_LLM}")
message(STATUS "  Redis L2     : ${ENABLE_REDIS}")
message(STATUS "============================================")
message(STATUS "")

# ==============================================================================
#  Tests
# ==============================================================================
if(ENABLE_TESTS)
  enable_testing()
  add_executable(address-service-tests
    tests/test_main.cc
    tests/test_preprocessor.cc
    tests/test_parser.cc
    tests/test_rule_engine.cc
    tests/test_confidence.cc
    tests/test_cache.cc
    tests/test_validator.cc
    src/services/PreProcessor.cc
    src/services/AddressParser.cc
    src/services/RuleEngine.cc
    src/services/ConfidenceScorer.cc
    src/services/CacheManager.cc
    src/services/MetricsCollector.cc
  )
  target_include_directories(address-service-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${LIBPOSTAL_INCLUDE_DIRS}
  )
  target_link_libraries(address-service-tests PRIVATE
    ${LIBPOSTAL_LIBRARIES}
    pthread
  )
  if(ENABLE_MIMALLOC AND mimalloc_FOUND)
    target_link_libraries(address-service-tests PRIVATE mimalloc)
  endif()
  add_test(NAME AllTests COMMAND address-service-tests)
endif()

install(TARGETS ${PROJECT_NAME} DESTINATION bin)