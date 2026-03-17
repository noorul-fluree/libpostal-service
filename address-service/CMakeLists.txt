cmake_minimum_required(VERSION 3.16)
project(address-service VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ==============================================================================
#  Options
# ==============================================================================
option(ENABLE_TESTS  "Build test suite"      ON)
option(ENABLE_REDIS  "Enable Redis L2 cache" OFF)

# ==============================================================================
#  Dependencies
# ==============================================================================
find_package(Drogon REQUIRED)
find_package(PkgConfig REQUIRED)

# libpostal
pkg_check_modules(LIBPOSTAL REQUIRED libpostal)

# hiredis (optional Redis)
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
#  Source files
# ==============================================================================
set(SOURCES
    src/main.cc
    src/services/AddressParser.cc
    src/services/PreProcessor.cc
    src/services/RuleEngine.cc
    src/services/ConfidenceScorer.cc
    src/services/CacheManager.cc
    src/services/MetricsCollector.cc
    src/controllers/ParseController.cc
    src/controllers/BatchController.cc
    src/controllers/HealthController.cc
    src/controllers/MetricsController.cc
)

# ==============================================================================
#  Main executable
# ==============================================================================
add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${LIBPOSTAL_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Drogon::Drogon
    ${LIBPOSTAL_LIBRARIES}
    pthread
)

if(ENABLE_REDIS)
    target_include_directories(${PROJECT_NAME} PRIVATE ${HIREDIS_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${HIREDIS_LIBRARIES})
endif()

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
    add_test(NAME AllTests COMMAND address-service-tests)
endif()

# ==============================================================================
#  Install
# ==============================================================================
install(TARGETS ${PROJECT_NAME} DESTINATION bin)