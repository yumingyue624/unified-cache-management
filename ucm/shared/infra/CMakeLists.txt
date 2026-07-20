file(GLOB_RECURSE UCMINFRA_STATUS_SOURCE_FILES "status/*.*")
add_library(infra_status INTERFACE)
target_sources(infra_status INTERFACE ${UCMINFRA_STATUS_SOURCE_FILES})
target_include_directories(infra_status INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(infra_status INTERFACE fmt)

add_subdirectory(logger)

file(GLOB_RECURSE UCMINFRA_TEMPLATE_SOURCE_FILES "template/*.*")
add_library(infra_template INTERFACE)
target_sources(infra_template INTERFACE ${UCMINFRA_TEMPLATE_SOURCE_FILES})
target_include_directories(infra_template INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

file(GLOB_RECURSE UCMINFRA_THREAD_SOURCE_FILES "thread/*.*")
add_library(infra_thread INTERFACE)
target_sources(infra_thread INTERFACE ${UCMINFRA_THREAD_SOURCE_FILES})
target_include_directories(infra_thread INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

file(GLOB_RECURSE UCMINFRA_TIME_SOURCE_FILES "time/*.*")
add_library(infra_time INTERFACE)
target_sources(infra_time INTERFACE ${UCMINFRA_TIME_SOURCE_FILES})
target_include_directories(infra_time INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
