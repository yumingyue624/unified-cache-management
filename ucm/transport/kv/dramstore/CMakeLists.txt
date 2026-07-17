find_package(Threads REQUIRED)

set(DRAMPOOL_SOURCE_DIR
    ${CMAKE_SOURCE_DIR}/ucm/store/dram/cc/drampool)

add_library(drampool_core STATIC
    ${DRAMPOOL_SOURCE_DIR}/completion_poller.cpp
    ${DRAMPOOL_SOURCE_DIR}/drampool_buffer.cpp
    ${DRAMPOOL_SOURCE_DIR}/drampool_launch_config.cpp
    ${DRAMPOOL_SOURCE_DIR}/drampool_daemon.cpp
    ${DRAMPOOL_SOURCE_DIR}/drampool_yaml_config.cpp
    ${DRAMPOOL_SOURCE_DIR}/drampool_server.cpp
    ${DRAMPOOL_SOURCE_DIR}/kv_protocol.cpp
    ${DRAMPOOL_SOURCE_DIR}/task_worker.cpp
)

target_include_directories(drampool_core
    PUBLIC
        ${DRAMPOOL_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/ucm/store/dram/cc
        ${CMAKE_SOURCE_DIR}/ucm/store/detail
)

target_link_libraries(drampool_core
    PUBLIC
        infra_status
        infra_logger
        infra_template
        infra_thread
        buffer_pool
        p2p_transport
        trans
        dramstore
        Threads::Threads
)

add_executable(drampool
    ${DRAMPOOL_SOURCE_DIR}/main.cpp
)

target_link_libraries(drampool
    PRIVATE
        drampool_core
)

install(TARGETS drampool RUNTIME DESTINATION bin COMPONENT ucm)

if(BUILD_UNIT_TESTS)
    include(GoogleTest)
    option(BUILD_DRAMPOOL_RUNTIME_INTEGRATION_TESTS
           "Run DramPool startup tests that require a real Hixl runtime" OFF)
    add_executable(drampool_startup.test
        test/drampool_startup_test.cpp
        test/drampool_yaml_config_test.cpp
    )
    target_link_libraries(drampool_startup.test
        PRIVATE
            drampool_core
            gtest_main
            gtest
    )
    if(BUILD_DRAMPOOL_RUNTIME_INTEGRATION_TESTS)
        target_compile_definitions(drampool_startup.test
            PRIVATE UCM_DRAMPOOL_RUNTIME_INTEGRATION_TESTS=1)
    endif()
    if(MSVC)
        target_link_options(drampool_startup.test PRIVATE /PROFILE)
    endif()
    gtest_discover_tests(drampool_startup.test)

    add_executable(completion_poller.integration.test EXCLUDE_FROM_ALL
            test/completion_poller_test.cpp
    )
    target_link_libraries(completion_poller.integration.test
        PRIVATE
            drampool_core
            gtest_main
            gtest
    )
    if(MSVC)
        target_link_options(completion_poller.integration.test PRIVATE /PROFILE)
    endif()
    gtest_discover_tests(completion_poller.integration.test PROPERTIES LABELS integration)
endif()
