if(BUILD_UNIT_TESTS)
    include(GoogleTest)
    file(GLOB_RECURSE UCMSTORE_TEST_SOURCE_FILES "./case/*.cc")
    if(NOT BUILD_UCM_ASU)
        list(FILTER UCMSTORE_TEST_SOURCE_FILES EXCLUDE REGEX ".*[/\\\\]asu[/\\\\].*")
        list(FILTER UCMSTORE_TEST_SOURCE_FILES EXCLUDE REGEX ".*[/\\\\]delegator[/\\\\].*")
    endif()
    if(NOT RUNTIME_ENVIRONMENT STREQUAL "ascend")
        list(FILTER UCMSTORE_TEST_SOURCE_FILES EXCLUDE REGEX ".*[/\\\\]dram[/\\\\].*")
    else()
        list(APPEND UCMSTORE_TEST_SOURCE_FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/case/dram/drampool/completion_poller_test.cpp")
    endif()
    add_executable(ucmstore.test ${UCMSTORE_TEST_SOURCE_FILES})
    target_include_directories(ucmstore.test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/case)
    target_link_libraries(ucmstore.test PRIVATE
        posixstore cachestore fakestore pcstore
        gtest_main gtest gmock
    )
    if(RUNTIME_ENVIRONMENT STREQUAL "ascend")
        set(DRAMPOOL_TEST_P2P_ROOT "${CMAKE_SOURCE_DIR}/ucm/transport/p2p")
        set(DRAMPOOL_TEST_HIXL_ROOT "$ENV{HIXL_HOME}")
        find_path(DRAMPOOL_TEST_ASCEND_INCLUDE_DIR NAMES acl/acl.h
            HINTS ${ASCEND_ROOT}/include ${ASCEND_ROOT}/aarch64-linux/include)
        find_path(DRAMPOOL_TEST_HIXL_INCLUDE_DIR NAMES hixl/hixl.h
            HINTS ${DRAMPOOL_TEST_HIXL_ROOT}/include ${ASCEND_ROOT}/include)
        find_library(DRAMPOOL_TEST_ASCENDCL_LIB NAMES ascendcl
            HINTS ${ASCEND_ROOT}/lib64 ${ASCEND_ROOT}/aarch64-linux/lib64)
        find_library(DRAMPOOL_TEST_HIXL_LIB NAMES hixl cann_hixl
            HINTS ${DRAMPOOL_TEST_HIXL_ROOT}/lib64 ${DRAMPOOL_TEST_HIXL_ROOT}/lib)
        find_library(DRAMPOOL_TEST_METADEF_LIB NAMES metadef
            HINTS ${ASCEND_ROOT}/lib64 ${ASCEND_ROOT}/aarch64-linux/lib64)
        if(NOT DRAMPOOL_TEST_ASCEND_INCLUDE_DIR OR NOT DRAMPOOL_TEST_HIXL_INCLUDE_DIR OR
           NOT DRAMPOOL_TEST_ASCENDCL_LIB OR NOT DRAMPOOL_TEST_HIXL_LIB OR
           NOT DRAMPOOL_TEST_METADEF_LIB)
            message(FATAL_ERROR "Cannot find ACL/HIXL dependencies for DramPool real transport tests")
        endif()

        add_library(drampool_test_real_transport STATIC
            ${DRAMPOOL_TEST_P2P_ROOT}/src/one_sided/common/acl_runtime_context.cpp
            ${DRAMPOOL_TEST_P2P_ROOT}/src/one_sided/common/metadata_codec.cpp
            ${DRAMPOOL_TEST_P2P_ROOT}/src/one_sided/control/metadata_channel.cpp
            ${DRAMPOOL_TEST_P2P_ROOT}/src/one_sided/core/transport_manager.cpp
            ${DRAMPOOL_TEST_P2P_ROOT}/src/one_sided/hixl/hixl_instance.cpp
            ${DRAMPOOL_TEST_P2P_ROOT}/src/one_sided/hixl/hixl_transport.cpp)
        target_include_directories(drampool_test_real_transport PUBLIC
            ${DRAMPOOL_TEST_P2P_ROOT}/include
            ${DRAMPOOL_TEST_P2P_ROOT}/include/one_sided
            ${DRAMPOOL_TEST_P2P_ROOT}/src/one_sided
            ${CMAKE_SOURCE_DIR}/ucm/shared/infra
            ${DRAMPOOL_TEST_ASCEND_INCLUDE_DIR}
            ${DRAMPOOL_TEST_HIXL_INCLUDE_DIR})
        target_compile_definitions(drampool_test_real_transport PUBLIC UCM_P2P_HAS_HIXL=1)
        find_package(Threads REQUIRED)
        target_link_libraries(drampool_test_real_transport PUBLIC
            infra_logger
            Threads::Threads
            ${DRAMPOOL_TEST_ASCENDCL_LIB}
            ${DRAMPOOL_TEST_HIXL_LIB}
            ${DRAMPOOL_TEST_METADEF_LIB})

        target_sources(ucmstore.test PRIVATE
            ${CMAKE_SOURCE_DIR}/ucm/store/dram/cc/drampool/completion_poller.cpp
            ${CMAKE_SOURCE_DIR}/ucm/store/dram/cc/kv_protocol.cpp)
        target_link_libraries(ucmstore.test PRIVATE dramstore drampool_test_real_transport)

    endif()
    if(BUILD_UCM_ASU)
        target_link_libraries(ucmstore.test PRIVATE
            asu_client
            delegator
            buffer_pool
            asu_ascend_deps
        )
        target_compile_definitions(ucmstore.test PRIVATE ASU_BUILD_TESTS)
    endif()
    gtest_discover_tests(ucmstore.test)
endif()
