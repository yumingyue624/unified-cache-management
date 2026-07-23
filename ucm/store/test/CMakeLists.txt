if(BUILD_UNIT_TESTS)
    include(GoogleTest)
    file(GLOB_RECURSE UCMSTORE_TEST_SOURCE_FILES "./case/*.cc")
    if(NOT BUILD_UCM_ASU)
        list(FILTER UCMSTORE_TEST_SOURCE_FILES EXCLUDE REGEX ".*[/\\\\]asu[/\\\\].*")
        list(FILTER UCMSTORE_TEST_SOURCE_FILES EXCLUDE REGEX ".*[/\\\\]delegator[/\\\\].*")
    endif()
    if(NOT RUNTIME_ENVIRONMENT STREQUAL "ascend")
        list(FILTER UCMSTORE_TEST_SOURCE_FILES EXCLUDE REGEX ".*[/\\\\]dram[/\\\\].*")
    elseif(BUILD_UCM_DRAMPOOL)
        list(APPEND UCMSTORE_TEST_SOURCE_FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/case/dram/drampool/completion_poller_test.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/case/dram/drampool/drampool_startup_test.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/case/dram/drampool/drampool_yaml_config_test.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/case/dram/drampool/task_worker_test.cpp"
        )
    endif()
    add_executable(ucmstore.test ${UCMSTORE_TEST_SOURCE_FILES})
    target_include_directories(ucmstore.test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/case)
    target_link_libraries(ucmstore.test PRIVATE
        posixstore cachestore fakestore pcstore
        gtest_main gtest gmock
    )
    if(RUNTIME_ENVIRONMENT STREQUAL "ascend")
        target_link_libraries(ucmstore.test PRIVATE dramstore)
        if(BUILD_UCM_DRAMPOOL)
            target_link_libraries(ucmstore.test PRIVATE drampool_core)
        endif()
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
