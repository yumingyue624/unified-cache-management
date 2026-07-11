if(NOT BUILD_UCM_DRAMPOOL)
    return()
endif()

if(NOT UNIX)
    if(BUILD_UNIT_TESTS)
        add_library(p2p_transport STATIC
            src/one_sided/common/metadata_codec.cpp
            test/metadata_channel_test_stub.cpp
            src/one_sided/core/transport_manager.cpp)
        target_include_directories(p2p_transport
            PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include/one_sided
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/one_sided
                    ${CMAKE_SOURCE_DIR}/ucm/shared/infra)
        target_link_libraries(p2p_transport PUBLIC infra_logger)
    else()
        # Runtime symbols are Linux-only; headers remain available for compile checks.
        add_library(p2p_transport INTERFACE)
        target_include_directories(p2p_transport INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/include/one_sided)
    endif()
    return()
endif()

set(P2P_TRANSPORT_SOURCES
    src/one_sided/common/metadata_codec.cpp
    src/one_sided/control/metadata_channel.cpp
    src/one_sided/core/transport_manager.cpp
)

set(P2P_PRIVATE_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src/one_sided
                             ${CMAKE_SOURCE_DIR}/ucm/shared/infra)
set(P2P_PRIVATE_LIBRARIES)

if(RUNTIME_ENVIRONMENT STREQUAL "ascend" AND NOT DEFINED ASCEND_ROOT)
    if(DEFINED ENV{ASCEND_HOME_PATH})
        set(ASCEND_ROOT "$ENV{ASCEND_HOME_PATH}" CACHE PATH "Path to Ascend root directory")
    elseif(DEFINED ENV{ASCEND_TOOLKIT_HOME})
        set(ASCEND_ROOT "$ENV{ASCEND_TOOLKIT_HOME}" CACHE PATH "Path to Ascend root directory")
    else()
        set(ASCEND_ROOT "/usr/local/Ascend/ascend-toolkit/latest" CACHE PATH
            "Path to Ascend root directory")
    endif()
endif()

if(RUNTIME_ENVIRONMENT STREQUAL "ascend")
    set(HIXL_ROOT "$ENV{HIXL_HOME}" CACHE PATH "Path to HIXL installation")
    find_path(P2P_ASCEND_INCLUDE_DIR NAMES acl/acl.h
        HINTS ${ASCEND_ROOT}/include ${ASCEND_ROOT}/aarch64-linux/include
              ${ASCEND_ROOT}/arm64-linux/include)
    find_library(P2P_ASCENDCL_LIB NAMES ascendcl
        HINTS ${ASCEND_ROOT}/lib64 ${ASCEND_ROOT}/aarch64-linux/lib64
              ${ASCEND_ROOT}/arm64-linux/lib64)
    find_path(P2P_HIXL_INCLUDE_DIR NAMES hixl/hixl.h
        HINTS ${HIXL_ROOT}/include ${ASCEND_ROOT}/include)
    find_library(P2P_HIXL_LIB NAMES hixl
        HINTS ${HIXL_ROOT}/lib64 ${HIXL_ROOT}/lib ${ASCEND_ROOT}/lib64)

    if(NOT P2P_ASCEND_INCLUDE_DIR OR NOT P2P_ASCENDCL_LIB OR
       NOT P2P_HIXL_INCLUDE_DIR OR NOT P2P_HIXL_LIB)
        message(FATAL_ERROR "Cannot find the ACL/HIXL dependencies required by p2p transport")
    endif()
    list(APPEND P2P_TRANSPORT_SOURCES
        src/one_sided/common/acl_runtime_context.cpp
        src/one_sided/hixl/hixl_transport.cpp)
    list(APPEND P2P_PRIVATE_INCLUDE_DIRS ${P2P_ASCEND_INCLUDE_DIR} ${P2P_HIXL_INCLUDE_DIR})
    list(APPEND P2P_PRIVATE_LIBRARIES ${P2P_ASCENDCL_LIB} ${P2P_HIXL_LIB})
endif()

find_package(Threads REQUIRED)
add_library(p2p_transport STATIC ${P2P_TRANSPORT_SOURCES})
target_include_directories(p2p_transport
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include/one_sided
    PRIVATE
        ${P2P_PRIVATE_INCLUDE_DIRS}
)
if(RUNTIME_ENVIRONMENT STREQUAL "ascend")
    target_compile_definitions(p2p_transport PRIVATE UCM_P2P_HAS_HIXL=1)
endif()
target_link_libraries(p2p_transport
    PUBLIC
        infra_logger
        Threads::Threads
    PRIVATE
        ${P2P_PRIVATE_LIBRARIES}
)
