# XXH_INLINE_ALL is used privately by the key mapper; no shared library is needed.
find_path(KV_CLIENT_XXHASH_INCLUDE_DIR NAMES xxhash.h)
if(NOT KV_CLIENT_XXHASH_INCLUDE_DIR)
    if(NOT DOWNLOAD_DEPENDENCE)
        message(FATAL_ERROR "Set KV_CLIENT_XXHASH_INCLUDE_DIR to the directory containing xxhash.h")
    endif()
    include(FetchContent)
    FetchContent_Declare(kv_client_xxhash
        GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
        GIT_TAG v0.8.3
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(kv_client_xxhash)
    set(KV_CLIENT_XXHASH_INCLUDE_DIR ${kv_client_xxhash_SOURCE_DIR})
endif()
