find_package(Threads REQUIRED)

add_library(drampool_core STATIC
    src/completion_poller.cpp
    src/drampool_config.cpp
    src/drampool_daemon.cpp
    src/drampool_server.cpp
    src/kv_protocol.cpp
    src/task_worker.cpp
)

target_include_directories(drampool_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/ucm/store/detail
)

target_link_libraries(drampool_core
    PUBLIC
        infra_status
        infra_logger
        infra_template
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
    add_executable(drampool_startup.test
        test/drampool_startup_test.cpp
    )
    target_link_libraries(drampool_startup.test
        PRIVATE
            drampool_core
            gtest_main
            gtest
    )
    if(MSVC)
        target_link_options(drampool_startup.test PRIVATE /PROFILE)
    endif()
    gtest_discover_tests(drampool_startup.test)

    add_executable(completion_poller.test
        test/completion_poller_test.cpp
    )
    target_link_libraries(completion_poller.test
        PRIVATE
            drampool_core
            gtest_main
            gtest
    )
    if(MSVC)
        target_link_options(completion_poller.test PRIVATE /PROFILE)
    endif()
    gtest_discover_tests(completion_poller.test)
endif()
