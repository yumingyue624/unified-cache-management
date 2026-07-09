add_subdirectory(common)

if(BUILD_UCM_DRAMPOOL)
    add_subdirectory(dramstore)
endif()

if(BUILD_UCM_ASU)
    add_subdirectory(asu)
    add_subdirectory(kv-test)
endif()
