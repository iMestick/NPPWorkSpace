cmake_minimum_required(VERSION 3.20)

project(NPPWorkSpace VERSION 1.5.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# OpenSSL is installed statically by vcpkg. libssh itself is fetched from the
# upstream 0.12.2 security release because the vcpkg port can lag behind the
# latest libssh patch release.
find_package(OpenSSL REQUIRED)
include(FetchContent)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WITH_SERVER OFF CACHE BOOL "" FORCE)
set(WITH_GSSAPI OFF CACHE BOOL "" FORCE)
set(WITH_ZLIB OFF CACHE BOOL "" FORCE)
set(WITH_NACL OFF CACHE BOOL "" FORCE)
set(WITH_PCAP OFF CACHE BOOL "" FORCE)
set(WITH_SYMBOL_VERSIONING OFF CACHE BOOL "" FORCE)
set(UNIT_TESTING OFF CACHE BOOL "" FORCE)
set(CLIENT_TESTING OFF CACHE BOOL "" FORCE)
set(SERVER_TESTING OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    nppworkspace_libssh
    URL https://www.libssh.org/files/0.12/libssh-0.12.2.tar.xz
    URL_HASH SHA256=49560f677d96e3706a904ac2de1116e25f3680937d51e5c92198fcba4a1c1e9f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nppworkspace_libssh)

if(TARGET ssh::static)
    set(NPPWORKSPACE_LIBSSH_TARGET ssh::static)
elseif(TARGET ssh-static)
    set(NPPWORKSPACE_LIBSSH_TARGET ssh-static)
elseif(TARGET ssh_static)
    set(NPPWORKSPACE_LIBSSH_TARGET ssh_static)
elseif(TARGET ssh)
    # Fallback para revisoes do libssh que respeitam BUILD_SHARED_LIBS=OFF
    # e geram apenas o target principal como biblioteca estatica.
    set(NPPWORKSPACE_LIBSSH_TARGET ssh)
else()
    message(FATAL_ERROR "A compilacao do libssh nao criou o target estatico esperado.")
endif()

add_library(NPPWorkSpace SHARED
    src/dllmain.cpp
    src/PluginDefinition.cpp
    src/QuickOpen.cpp
    src/RemoteSsh.cpp
)

target_compile_definitions(NPPWorkSpace PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    LIBSSH_STATIC
)

target_compile_options(NPPWorkSpace PRIVATE
    /utf-8
    /W4
    /permissive-
)

target_link_libraries(NPPWorkSpace PRIVATE
    user32
    gdi32
    shell32
    comctl32
    comdlg32
    crypt32
    bcrypt
    ole32
    ws2_32
    OpenSSL::Crypto
    ${NPPWORKSPACE_LIBSSH_TARGET}
)

set_target_properties(NPPWorkSpace PROPERTIES
    OUTPUT_NAME "NPPWorkSpace"
    PREFIX ""
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)
