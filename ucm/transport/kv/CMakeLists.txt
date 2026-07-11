add_subdirectory(common)

if(BUILD_UCM_DRAMPOOL)
    if(TARGET p2p_transport)
        add_subdirectory(dramstore)
    else()
        message(STATUS "Skipping DramPool: formal p2p transport is unavailable")
    endif()
endif()

if(BUILD_UCM_ASU)
    add_subdirectory(asu)
    add_subdirectory(kv-test)
endif()
