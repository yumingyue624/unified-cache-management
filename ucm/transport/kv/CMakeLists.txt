add_subdirectory(common)

if(BUILD_UCM_ASU)
    add_subdirectory(asu)
endif()

if(BUILD_UCM_DRAMPOOL)
    if(TARGET p2p_transport AND TARGET asu_transport)
        add_subdirectory(dramstore)
    else()
        message(STATUS "Skipping DramPool: formal p2p transport or ASU BufferManager is unavailable")
    endif()
endif()

if(BUILD_UCM_ASU)
    add_subdirectory(kv-test)
endif()
