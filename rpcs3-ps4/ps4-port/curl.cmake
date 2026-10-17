# curl performs executable link checks even for a static cross build. Use a
# separate cache with the native ELF linker and the already built TLS archive.
# This also keeps curl's HAVE_* checks out of other dependencies' caches.
include(ExternalProject)
set(_ps4_curl_build "${CMAKE_CURRENT_BINARY_DIR}/deps/curl-native")
if(CMAKE_HOST_WIN32)
    set(_ps4_curl_linker "${CMAKE_CURRENT_BINARY_DIR}/rpcs3-ps4-ld.exe")
else()
    set(_ps4_curl_linker "${RPCS3_PS4_LLD}")
endif()
set(_ps4_curl_flags
    "${CMAKE_C_FLAGS} -mcx16 -I\"${RPCS3_PS4_ORBIS_COMPAT}/include\" -I\"${CMAKE_CURRENT_BINARY_DIR}/deps/wolfssl/wolfssl\" -include \"${CMAKE_CURRENT_SOURCE_DIR}/compat/ps4_curl_socket.h\" -DWOLFSSL_USE_OPTIONS_H -DWOLFSSL_DES_ECB -DHAVE_WRITE_DUP -DFP_MAX_BITS=8192")
set(_ps4_curl_link_flags
    "-fuse-ld=\"${_ps4_curl_linker}\" -nostdlib -pie -Wl,-m,elf_x86_64 -Wl,-T,\"${RPCS3_PS4_SDK}/link.x\" -Wl,--eh-frame-hdr -Wl,--no-dynamic-linker")
set(_ps4_curl_libs
    "\"${RPCS3_PS4_SDK}/lib/crt1.o\" -L\"${RPCS3_PS4_SDK}/lib\" -lc -lm -lkernel")
ExternalProject_Add(rpcs3_ps4_curl_build
    SOURCE_DIR "${CMAKE_SOURCE_DIR}/3rdparty/curl/curl"
    BINARY_DIR "${_ps4_curl_build}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    CMAKE_GENERATOR Ninja
    CMAKE_ARGS
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/toolchain.cmake"
        "-DRPCS3_PS4_SDK=${RPCS3_PS4_SDK}"
        "-DRPCS3_PS4_LLVM_BIN=${RPCS3_PS4_LLVM_BIN}"
        "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        -DRPCS3_PS4_EXECUTABLE_CHECKS=ON
        "-DCMAKE_BUILD_TYPE=Release"
        "-DCMAKE_C_FLAGS=${_ps4_curl_flags}"
        "-DCMAKE_EXE_LINKER_FLAGS=${_ps4_curl_link_flags}"
        "-DCMAKE_C_STANDARD_LIBRARIES=${_ps4_curl_libs}"
        "-DWOLFSSL_INCLUDE_DIR=${CMAKE_SOURCE_DIR}/3rdparty/wolfssl/wolfssl"
        "-DWOLFSSL_LIBRARY=$<TARGET_FILE:wolfssl>"
        -DCURL_USE_WOLFSSL=ON -DCURL_USE_OPENSSL=OFF
        -DBUILD_CURL_EXE=OFF -DBUILD_STATIC_CURL=OFF
        -DBUILD_STATIC_LIBS=ON -DBUILD_SHARED_LIBS=OFF
        -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF -DHTTP_ONLY=ON
        -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF
        -DCURL_USE_PKGCONFIG=OFF -DCURL_ZLIB=OFF
        -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF
        -DCURL_DISABLE_INSTALL=ON -DCURL_ENABLE_EXPORT_TARGET=OFF
        # SDK stropts.h declares ioctl differently from sys/ioctl.h. curl
        # needs the latter for sockets; it does not need a STREAMS interface.
        -DHAVE_STROPTS_H=OFF
        # OpenOrbis libc eventfd() is an ENOSYS stub (errno 78) and curl 8.22 treats a failed
        # wakeup pair as fatal: curl_easy_perform returned CURLE_OUT_OF_MEMORY before connecting.
        # The easy interface does not need a wakeup pair; this also removes the resolver's pair.
        -DCURL_DISABLE_SOCKETPAIR=ON -DHAVE_EVENTFD=OFF
        # The kernel refuses fcntl(F_SETFL, O_NONBLOCK) and FIONBIO (startup.log 'network ABI'); use
        # curl's Orbis branch, setsockopt(SOL_SOCKET, SO_NONBLOCK), with SO_NONBLOCK = SO_NBIO 0x1200
        # (compat/ps4_curl_socket.h).
        -DHAVE_FCNTL_O_NONBLOCK=OFF -DHAVE_IOCTL_FIONBIO=OFF -DHAVE_SETSOCKOPT_SO_NONBLOCK=1
        # compat/ps4_curl_socket.h hides the SDK's Linux SOCK_NONBLOCK/SOCK_CLOEXEC bits; the accept4()
        # path (cf-socket.c, FTP active mode) needs SOCK_NONBLOCK, the accept()+curlx_nonblock path does not.
        -DHAVE_ACCEPT4=OFF
        # The port pins IPv4 with CURLOPT_RESOLVE; no IPv6 probe socket at multi creation.
        -DENABLE_IPV6=OFF
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target libcurl_static -j 4
    BUILD_BYPRODUCTS "${_ps4_curl_build}/lib/libcurl.a"
    INSTALL_COMMAND ""
    DEPENDS wolfssl)
add_library(libcurl_static STATIC IMPORTED GLOBAL)
set_target_properties(libcurl_static PROPERTIES
    IMPORTED_LOCATION "${_ps4_curl_build}/lib/libcurl.a"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/3rdparty/curl/curl/include"
    INTERFACE_LINK_LIBRARIES wolfssl)
add_dependencies(libcurl_static rpcs3_ps4_curl_build)
