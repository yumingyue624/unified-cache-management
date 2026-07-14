find_package(Threads REQUIRED)

set(DRAMPOOL_BUFFER_MANAGER_SOURCE_DIR
    ${CMAKE_SOURCE_DIR}/ucm/transport/kv/asu/trans/src)

add_library(drampool_core STATIC
    src/completion_poller.cpp
    src/drampool_buffer.cpp
    src/drampool_config.cpp
    src/drampool_daemon.cpp
    src/drampool_runtime_config.cpp
    src/drampool_server.cpp
    src/kv_control_protocol.cpp
    src/kv_protocol.cpp
    src/task_worker.cpp
)

target_include_directories(drampool_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/ucm/store/dram/cc
        ${CMAKE_SOURCE_DIR}/ucm/store/detail
    PRIVATE
        ${DRAMPOOL_BUFFER_MANAGER_SOURCE_DIR}
)

target_link_libraries(drampool_core
    PUBLIC
        infra_status
        infra_logger
        infra_template
        infra_thread
        p2p_transport
        asu_transport
        asu_ascend_deps
        Threads::Threads
)

add_executable(drampool
    src/main.cpp
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
        test/kv_control_protocol_test.cpp
    )
    target_link_libraries(drampool_startup.test
        PRIVATE
            drampool_core
            gtest_main
            gtest
    )
    target_include_directories(drampool_startup.test
        PRIVATE ${DRAMPOOL_BUFFER_MANAGER_SOURCE_DIR})
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
    target_include_directories(completion_poller.integration.test
        PRIVATE ${DRAMPOOL_BUFFER_MANAGER_SOURCE_DIR})
    if(MSVC)
        target_link_options(completion_poller.integration.test PRIVATE /PROFILE)
    endif()
    gtest_discover_tests(completion_poller.integration.test PROPERTIES LABELS integration)
endif()
