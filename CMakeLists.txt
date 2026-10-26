cmake_minimum_required(VERSION 3.24)

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  if(NOT CMAKE_OSX_ARCHITECTURES)
    execute_process(
      COMMAND uname -m
      OUTPUT_VARIABLE LANAUDIO_HOST_ARCH
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(CMAKE_OSX_ARCHITECTURES "${LANAUDIO_HOST_ARCH}" CACHE STRING
      "macOS target architecture (arm64 or x86_64)" FORCE)
  endif()
  if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING
      "Minimum supported macOS version" FORCE)
  endif()
endif()

project(LanAudio VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LANAUDIO_BUILD_GUI "Build the Qt desktop application" ON)
option(LANAUDIO_ENABLE_RTAUDIO "Enable physical audio I/O" ON)
option(LANAUDIO_FETCH_DEPS "Fetch dependencies that are not installed" ON)
option(LANAUDIO_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(LANAUDIO_MACOS_DISTRIBUTION
  "Require a complete architecture-specific macOS distribution" OFF)
option(LANAUDIO_ENABLE_SECURITY
  "Build the authenticated control channel and SRTP media protection" ON)
option(LANAUDIO_ALLOW_PLAINTEXT
  "Development only: permit unauthenticated plaintext RTP" OFF)
option(LANAUDIO_BUILD_FUZZERS
  "Build libFuzzer harnesses for the network parsers" OFF)

if(LANAUDIO_ALLOW_PLAINTEXT AND LANAUDIO_MACOS_DISTRIBUTION)
  message(FATAL_ERROR
    "LANAUDIO_ALLOW_PLAINTEXT cannot be enabled for a distribution build. "
    "Released packages must never be able to fall back to unauthenticated "
    "plaintext RTP.")
endif()
if(LANAUDIO_MACOS_DISTRIBUTION AND NOT LANAUDIO_ENABLE_SECURITY)
  message(FATAL_ERROR
    "LANAUDIO_MACOS_DISTRIBUTION requires LANAUDIO_ENABLE_SECURITY=ON.")
endif()

# The pinned dependency manifest is authoritative. Security-relevant archives
# are verified by SHA-256 instead of trusting a mutable git tag.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.lock.json"
  LANAUDIO_LOCK_JSON)

function(lanaudio_locked_dependency dependency out_url out_sha)
  string(JSON entry_count LENGTH "${LANAUDIO_LOCK_JSON}" fetched)
  math(EXPR last_index "${entry_count} - 1")
  foreach(index RANGE ${last_index})
    string(JSON entry GET "${LANAUDIO_LOCK_JSON}" fetched ${index})
    string(JSON entry_name GET "${entry}" name)
    if(entry_name STREQUAL dependency)
      string(JSON entry_url GET "${entry}" url)
      string(JSON entry_sha GET "${entry}" sha256)
      set(${out_url} "${entry_url}" PARENT_SCOPE)
      set(${out_sha} "${entry_sha}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  message(FATAL_ERROR
    "Dependency '${dependency}' is missing from dependencies.lock.json.")
endfunction()

if(DEFINED ENV{GITHUB_RUN_NUMBER})
  set(LANAUDIO_DEFAULT_BUILD_NUMBER "$ENV{GITHUB_RUN_NUMBER}")
else()
  set(LANAUDIO_DEFAULT_BUILD_NUMBER "1")
endif()
set(LANAUDIO_BUILD_NUMBER "${LANAUDIO_DEFAULT_BUILD_NUMBER}" CACHE STRING
  "Numeric application bundle build number")

set(LANAUDIO_PACKAGE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE)
  set(LANAUDIO_MACOS_SUPPORTED_ARCHS arm64 x86_64)
  list(LENGTH CMAKE_OSX_ARCHITECTURES LANAUDIO_MACOS_ARCH_COUNT)
  if(NOT LANAUDIO_MACOS_ARCH_COUNT EQUAL 1)
    message(FATAL_ERROR
      "LanAudio macOS packages must target exactly one architecture. "
      "Configure separate builds with -DCMAKE_OSX_ARCHITECTURES=arm64 or "
      "-DCMAKE_OSX_ARCHITECTURES=x86_64. Universal packaging is disabled "
      "until every bundled native dependency is verified universal.")
  endif()
  list(GET CMAKE_OSX_ARCHITECTURES 0 LANAUDIO_PACKAGE_ARCH)
  if(NOT LANAUDIO_PACKAGE_ARCH IN_LIST LANAUDIO_MACOS_SUPPORTED_ARCHS)
    message(FATAL_ERROR
      "Unsupported macOS architecture '${LANAUDIO_PACKAGE_ARCH}'. "
      "Supported release architectures are arm64 and x86_64.")
  endif()
  message(STATUS
    "LanAudio macOS target: ${LANAUDIO_PACKAGE_ARCH}, minimum macOS "
    "${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

function(lanaudio_verify_macos_target_arch target label)
  if(NOT APPLE OR NOT TARGET ${target})
    return()
  endif()
  get_target_property(LANAUDIO_TARGET_IMPORTED ${target} IMPORTED)
  if(NOT LANAUDIO_TARGET_IMPORTED)
    return()
  endif()
  foreach(property IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION_RELWITHDEBINFO
                   IMPORTED_LOCATION)
    get_target_property(LANAUDIO_IMPORTED_FILE ${target} ${property})
    if(LANAUDIO_IMPORTED_FILE AND EXISTS "${LANAUDIO_IMPORTED_FILE}")
      execute_process(
        COMMAND /usr/bin/lipo "${LANAUDIO_IMPORTED_FILE}"
                -verify_arch "${LANAUDIO_PACKAGE_ARCH}"
        RESULT_VARIABLE LANAUDIO_LIPO_RESULT
        ERROR_VARIABLE LANAUDIO_LIPO_ERROR)
      if(NOT LANAUDIO_LIPO_RESULT EQUAL 0)
        execute_process(
          COMMAND /usr/bin/lipo -archs "${LANAUDIO_IMPORTED_FILE}"
          OUTPUT_VARIABLE LANAUDIO_AVAILABLE_ARCHS
          OUTPUT_STRIP_TRAILING_WHITESPACE
          ERROR_QUIET)
        message(FATAL_ERROR
          "${label} does not contain the requested "
          "${LANAUDIO_PACKAGE_ARCH} architecture. File: "
          "${LANAUDIO_IMPORTED_FILE}; available architectures: "
          "${LANAUDIO_AVAILABLE_ARCHS}. Install a matching dependency; "
          "an architecture-specific package cannot reuse a binary for a "
          "different CPU.")
      endif()
      return()
    endif()
  endforeach()
  if(LANAUDIO_MACOS_DISTRIBUTION)
    message(FATAL_ERROR
      "Cannot locate the native binary for imported target ${target}; "
      "its ${LANAUDIO_PACKAGE_ARCH} architecture cannot be verified.")
  endif()
endfunction()

function(lanaudio_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  if(LANAUDIO_WARNINGS_AS_ERRORS)
    if(MSVC)
      target_compile_options(${target} PRIVATE /WX)
    else()
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# Dependency-free control vocabulary: negotiation, framing, and abuse limits.
# Keeping it free of OpenSSL lets every CI job compile and fuzz the parsers.
add_library(lanaudio_protocol STATIC
    protocol/src/negotiation.cpp
    protocol/src/control_messages.cpp
    protocol/src/guard.cpp)
target_include_directories(lanaudio_protocol PUBLIC protocol/include)
target_compile_features(lanaudio_protocol PUBLIC cxx_std_20)
lanaudio_apply_warnings(lanaudio_protocol)

add_library(lanaudio_core STATIC
    core/src/rtp.cpp
    core/src/jitter_buffer.cpp
    core/src/drift_controller.cpp
    core/src/udp_socket.cpp
    core/src/stream.cpp)
target_include_directories(lanaudio_core PUBLIC core/include)
target_compile_features(lanaudio_core PUBLIC cxx_std_20)
target_link_libraries(lanaudio_core PUBLIC lanaudio_protocol)
lanaudio_apply_warnings(lanaudio_core)

# Winsock is needed by every Windows toolchain (MSVC and MinGW alike).
if(WIN32)
  target_link_libraries(lanaudio_core PUBLIC ws2_32)
endif()

set(LANAUDIO_HAS_RTAUDIO OFF)
if(LANAUDIO_ENABLE_RTAUDIO)
  find_package(RtAudio CONFIG QUIET NO_CMAKE_PACKAGE_REGISTRY)
  if(NOT RtAudio_FOUND AND LANAUDIO_FETCH_DEPS)
    set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(RTAUDIO_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(RTAUDIO_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
    set(RTAUDIO_API_JACK OFF CACHE BOOL "" FORCE)
    set(RTAUDIO_API_PULSE OFF CACHE BOOL "" FORCE)
    set(RTAUDIO_API_OSS OFF CACHE BOOL "" FORCE)
    set(RTAUDIO_API_DS OFF CACHE BOOL "" FORCE)
    set(RTAUDIO_API_ASIO OFF CACHE BOOL "" FORCE)
    if(APPLE)
      set(RTAUDIO_API_CORE ON CACHE BOOL "" FORCE)
      set(RTAUDIO_API_ALSA OFF CACHE BOOL "" FORCE)
      set(RTAUDIO_API_WASAPI OFF CACHE BOOL "" FORCE)
    elseif(WIN32)
      set(RTAUDIO_API_CORE OFF CACHE BOOL "" FORCE)
      set(RTAUDIO_API_ALSA OFF CACHE BOOL "" FORCE)
      set(RTAUDIO_API_WASAPI ON CACHE BOOL "" FORCE)
    else()
      set(RTAUDIO_API_CORE OFF CACHE BOOL "" FORCE)
      set(RTAUDIO_API_ALSA ON CACHE BOOL "" FORCE)
      set(RTAUDIO_API_WASAPI OFF CACHE BOOL "" FORCE)
    endif()
    lanaudio_locked_dependency(rtaudio LANAUDIO_RTAUDIO_URL
      LANAUDIO_RTAUDIO_SHA256)
    FetchContent_Declare(rtaudio
      URL "${LANAUDIO_RTAUDIO_URL}"
      URL_HASH "SHA256=${LANAUDIO_RTAUDIO_SHA256}"
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(CMAKE_SKIP_INSTALL_RULES_SAVED ${CMAKE_SKIP_INSTALL_RULES})
    set(CMAKE_SKIP_INSTALL_RULES ON)
    FetchContent_MakeAvailable(rtaudio)
    set(CMAKE_SKIP_INSTALL_RULES ${CMAKE_SKIP_INSTALL_RULES_SAVED})
    file(WRITE "${rtaudio_BINARY_DIR}/cmake_install.cmake"
      "# RtAudio is linked statically; dependency installation is disabled.\n")
  endif()
  if(TARGET RtAudio::rtaudio)
    set(LANAUDIO_RTAUDIO_TARGET RtAudio::rtaudio)
    set(LANAUDIO_HAS_RTAUDIO ON)
  elseif(TARGET rtaudio)
    set(LANAUDIO_RTAUDIO_TARGET rtaudio)
    set(LANAUDIO_HAS_RTAUDIO ON)
  endif()
endif()
if(LANAUDIO_HAS_RTAUDIO)
  lanaudio_verify_macos_target_arch(
    ${LANAUDIO_RTAUDIO_TARGET} "RtAudio")
elseif(APPLE AND LANAUDIO_MACOS_DISTRIBUTION)
  message(FATAL_ERROR
    "A macOS distribution requires RtAudio/CoreAudio, but no matching "
    "RtAudio target is available.")
endif()

add_library(lanaudio_audio STATIC backends/rtaudio/rtaudio_backend.cpp)
target_link_libraries(lanaudio_audio PUBLIC lanaudio_core)
target_include_directories(lanaudio_audio PUBLIC backends/rtaudio)
if(LANAUDIO_HAS_RTAUDIO)
  target_compile_definitions(lanaudio_audio PUBLIC LANAUDIO_HAS_RTAUDIO=1)
  target_link_libraries(lanaudio_audio PUBLIC ${LANAUDIO_RTAUDIO_TARGET})
endif()

# ---------------------------------------------------------------------------
# Security stack: device identity, trust, pairing, TLS control, SRTP media.
# ---------------------------------------------------------------------------
set(LANAUDIO_HAS_SECURITY OFF)
if(LANAUDIO_ENABLE_SECURITY)
  find_package(OpenSSL 3.0 COMPONENTS SSL Crypto)
  if(NOT OpenSSL_FOUND)
    if(LANAUDIO_MACOS_DISTRIBUTION)
      message(FATAL_ERROR
        "OpenSSL 3.0 or newer is required for the authenticated control "
        "channel. Set OPENSSL_ROOT_DIR to a matching installation.")
    endif()
    message(STATUS "OpenSSL 3.0+ not found; security stack disabled")
  else()
    lanaudio_verify_macos_target_arch(OpenSSL::SSL "OpenSSL libssl")
    lanaudio_verify_macos_target_arch(OpenSSL::Crypto "OpenSSL libcrypto")

    if(NOT TARGET srtp2 AND LANAUDIO_FETCH_DEPS)
      lanaudio_locked_dependency(libsrtp LANAUDIO_SRTP_URL
        LANAUDIO_SRTP_SHA256)
      set(ENABLE_OPENSSL ON CACHE BOOL "" FORCE)
      set(LIBSRTP_TEST_APPS OFF CACHE BOOL "" FORCE)
      set(TEST_APPS OFF CACHE BOOL "" FORCE)
      set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
      FetchContent_Declare(libsrtp
        URL "${LANAUDIO_SRTP_URL}"
        URL_HASH "SHA256=${LANAUDIO_SRTP_SHA256}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
      set(CMAKE_SKIP_INSTALL_RULES_SAVED ${CMAKE_SKIP_INSTALL_RULES})
      set(CMAKE_SKIP_INSTALL_RULES ON)
      FetchContent_MakeAvailable(libsrtp)
      set(CMAKE_SKIP_INSTALL_RULES ${CMAKE_SKIP_INSTALL_RULES_SAVED})
      if(DEFINED libsrtp_BINARY_DIR)
        file(WRITE "${libsrtp_BINARY_DIR}/cmake_install.cmake"
          "# libsrtp is linked statically; installation is disabled.\n")
      endif()
    endif()

    if(NOT TARGET srtp2)
      if(LANAUDIO_MACOS_DISTRIBUTION)
        message(FATAL_ERROR
          "libsrtp2 is required for encrypted media but was not available.")
      endif()
      message(STATUS "libsrtp2 not available; security stack disabled")
    else()
      set(LANAUDIO_HAS_SECURITY ON)
    endif()
  endif()
endif()

if(LANAUDIO_HAS_SECURITY)
  set(LANAUDIO_SECURITY_SOURCES
      security/src/identity.cpp
      security/src/trust_store.cpp
      security/src/credential_store.cpp
      security/src/pairing.cpp)
  if(APPLE)
    list(APPEND LANAUDIO_SECURITY_SOURCES
      security/src/credential_store_keychain.cpp)
  endif()
  add_library(lanaudio_security STATIC ${LANAUDIO_SECURITY_SOURCES})
  target_include_directories(lanaudio_security PUBLIC security/include)
  target_link_libraries(lanaudio_security
    PUBLIC lanaudio_protocol OpenSSL::SSL OpenSSL::Crypto)
  lanaudio_apply_warnings(lanaudio_security)
  if(APPLE)
    target_compile_definitions(lanaudio_security PUBLIC
      LANAUDIO_HAS_KEYCHAIN=1)
    target_link_libraries(lanaudio_security PUBLIC
      "-framework Security" "-framework CoreFoundation")
  endif()

  add_library(lanaudio_srtp STATIC srtp/src/srtp_protector.cpp)
  target_include_directories(lanaudio_srtp PUBLIC srtp/include)
  target_link_libraries(lanaudio_srtp PUBLIC lanaudio_core PRIVATE srtp2)
  lanaudio_apply_warnings(lanaudio_srtp)

  set(LANAUDIO_CONTROL_SOURCES
      control/src/tls_channel.cpp
      control/src/control_session.cpp
      control/src/secure_session.cpp
      control/src/discovery.cpp)
  add_library(lanaudio_control STATIC ${LANAUDIO_CONTROL_SOURCES})
  target_include_directories(lanaudio_control PUBLIC control/include)
  target_link_libraries(lanaudio_control
    PUBLIC lanaudio_security lanaudio_srtp lanaudio_core)
  lanaudio_apply_warnings(lanaudio_control)
  target_compile_definitions(lanaudio_control PUBLIC LANAUDIO_HAS_SECURITY=1)
  if(APPLE)
    target_compile_definitions(lanaudio_control PUBLIC LANAUDIO_HAS_BONJOUR=1)
  endif()

  target_link_libraries(lanaudio_audio PUBLIC lanaudio_control)
endif()

if(LANAUDIO_ALLOW_PLAINTEXT)
  message(WARNING
    "LANAUDIO_ALLOW_PLAINTEXT is enabled. This build can transmit "
    "unauthenticated plaintext RTP and must not be distributed.")
  target_compile_definitions(lanaudio_core PUBLIC LANAUDIO_ALLOW_PLAINTEXT=1)
endif()

add_executable(lanaudio apps/cli/main.cpp)
target_link_libraries(lanaudio PRIVATE lanaudio_audio)
target_compile_definitions(lanaudio PRIVATE
  LANAUDIO_VERSION="${PROJECT_VERSION}")
if(LANAUDIO_HAS_SECURITY)
  target_compile_definitions(lanaudio PRIVATE LANAUDIO_HAS_SECURITY=1)
endif()
if(APPLE)
  set_target_properties(lanaudio PROPERTIES
    BUILD_RPATH "@executable_path/../Frameworks"
    INSTALL_RPATH "@executable_path/../Frameworks")
endif()

if(LANAUDIO_BUILD_GUI)
  find_package(Qt6 6.5 COMPONENTS Widgets Network QUIET)
  if(Qt6_FOUND)
    if(APPLE AND Qt6_VERSION VERSION_GREATER_EQUAL 6.12 AND
       CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 14.4)
      message(FATAL_ERROR
        "Qt ${Qt6_VERSION} supports macOS 14.4 or newer. Configure with "
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=14.4 (or use a Qt release whose "
        "documented runtime floor matches your intended package).")
    endif()
    lanaudio_verify_macos_target_arch(Qt6::Core "Qt Core")
    lanaudio_verify_macos_target_arch(Qt6::Gui "Qt GUI")
    lanaudio_verify_macos_target_arch(Qt6::Widgets "Qt Widgets")
    lanaudio_verify_macos_target_arch(Qt6::Network "Qt Network")
    qt_standard_project_setup()
    qt_add_executable(lanaudio-desktop WIN32 MACOSX_BUNDLE
      apps/desktop/main.cpp)
    target_link_libraries(lanaudio-desktop PRIVATE
      lanaudio_audio Qt6::Widgets Qt6::Network)
    target_compile_definitions(lanaudio-desktop PRIVATE
      LANAUDIO_VERSION="${PROJECT_VERSION}")
    if(LANAUDIO_HAS_SECURITY)
      target_compile_definitions(lanaudio-desktop PRIVATE
        LANAUDIO_HAS_SECURITY=1)
    endif()
    if(APPLE)
      find_package(Python3 REQUIRED COMPONENTS Interpreter)
      foreach(property IMPORTED_LOCATION_RELEASE
                       IMPORTED_LOCATION_RELWITHDEBINFO IMPORTED_LOCATION)
        get_target_property(LANAUDIO_QT_CORE_FILE Qt6::Core ${property})
        if(LANAUDIO_QT_CORE_FILE AND EXISTS "${LANAUDIO_QT_CORE_FILE}")
          break()
        endif()
      endforeach()
      if(NOT LANAUDIO_QT_CORE_FILE)
        message(FATAL_ERROR
          "Cannot determine the Qt library directory for deployment.")
      endif()
      string(REGEX REPLACE "/[^/]+\\.framework/.*$" ""
        LANAUDIO_QT_LIBRARY_DIR "${LANAUDIO_QT_CORE_FILE}")
      configure_file(
        packaging/macos/Info.plist.in
        "${CMAKE_CURRENT_BINARY_DIR}/LanAudio-Info.plist"
        @ONLY)
      configure_file(
        packaging/macos/BundleDependencies.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/LanAudio-BundleDependencies.cmake"
        @ONLY)
      configure_file(
        packaging/macos/AdHocSign.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/LanAudio-AdHocSign.cmake"
        @ONLY)
      if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64" AND
         LANAUDIO_PACKAGE_ARCH STREQUAL "x86_64" AND
         TARGET Qt6::macdeployqt)
        get_target_property(
          LANAUDIO_X86_HOST_TOOL Qt6::macdeployqt IMPORTED_LOCATION_RELEASE)
        if(NOT LANAUDIO_X86_HOST_TOOL)
          get_target_property(
            LANAUDIO_X86_HOST_TOOL Qt6::macdeployqt IMPORTED_LOCATION)
        endif()
        if(NOT LANAUDIO_X86_HOST_TOOL)
          message(FATAL_ERROR
            "Cannot locate macdeployqt for the Intel cross-package.")
        endif()
        set(LANAUDIO_X86_TOOL_WRAPPER
          "${CMAKE_CURRENT_BINARY_DIR}/macdeployqt-x86_64")
        configure_file(
          packaging/macos/RunX86Tool.sh.in
          "${LANAUDIO_X86_TOOL_WRAPPER}"
          @ONLY)
        file(CHMOD "${LANAUDIO_X86_TOOL_WRAPPER}"
          PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                      GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
        set_target_properties(Qt6::macdeployqt PROPERTIES
          IMPORTED_LOCATION "${LANAUDIO_X86_TOOL_WRAPPER}"
          IMPORTED_LOCATION_RELEASE "${LANAUDIO_X86_TOOL_WRAPPER}"
          IMPORTED_LOCATION_RELWITHDEBINFO "${LANAUDIO_X86_TOOL_WRAPPER}")
      endif()
    endif()
    set_target_properties(lanaudio-desktop PROPERTIES
      OUTPUT_NAME "LanAudio"
      MACOSX_BUNDLE_BUNDLE_NAME "LanAudio"
      MACOSX_BUNDLE_GUI_IDENTIFIER "com.lanaudio.desktop"
      MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
      MACOSX_BUNDLE_INFO_PLIST
        "${CMAKE_CURRENT_BINARY_DIR}/LanAudio-Info.plist")
  else()
    if(APPLE AND LANAUDIO_MACOS_DISTRIBUTION)
      message(FATAL_ERROR
        "A macOS distribution requires matching Qt 6.5+ Widgets and Network "
        "libraries for ${LANAUDIO_PACKAGE_ARCH}. Set CMAKE_PREFIX_PATH to an "
        "appropriate Qt installation.")
    endif()
    message(STATUS
      "Qt 6.5+ Widgets/Network not found; desktop target disabled")
  endif()
elseif(APPLE AND LANAUDIO_MACOS_DISTRIBUTION)
  message(FATAL_ERROR
    "LANAUDIO_MACOS_DISTRIBUTION requires LANAUDIO_BUILD_GUI=ON.")
endif()

if(BUILD_TESTING)
  add_executable(lanaudio_tests tests/unit/core_tests.cpp)
  target_link_libraries(lanaudio_tests PRIVATE lanaudio_core)
  add_test(NAME lanaudio_core_tests COMMAND lanaudio_tests)
  add_test(NAME lanaudio_cli_smoke COMMAND lanaudio diagnostics)
  if(LANAUDIO_HAS_SECURITY)
    add_executable(lanaudio_security_tests tests/unit/security_tests.cpp)
    target_link_libraries(lanaudio_security_tests PRIVATE lanaudio_control)
    lanaudio_apply_warnings(lanaudio_security_tests)
    add_test(NAME lanaudio_security_tests COMMAND lanaudio_security_tests)
    set_tests_properties(lanaudio_security_tests PROPERTIES
      LABELS "security" TIMEOUT 180)
    add_test(NAME lanaudio_no_plaintext_in_distribution
      COMMAND ${CMAKE_COMMAND} -E echo
              "LANAUDIO_ALLOW_PLAINTEXT remains OFF in this configuration")
    if(LANAUDIO_ALLOW_PLAINTEXT)
      message(FATAL_ERROR
        "Tests refuse to run when LANAUDIO_ALLOW_PLAINTEXT is enabled; "
        "unauthenticated RTP must never pass as a silent fallback.")
    endif()
  endif()
  if(LANAUDIO_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      message(FATAL_ERROR "LANAUDIO_BUILD_FUZZERS requires Clang/libFuzzer.")
    endif()
    add_executable(lanaudio_fuzz_control tests/fuzz/fuzz_control.cpp)
    target_link_libraries(lanaudio_fuzz_control PRIVATE lanaudio_protocol)
    target_compile_options(lanaudio_fuzz_control PRIVATE
      -fsanitize=fuzzer,address,undefined)
    target_link_options(lanaudio_fuzz_control PRIVATE
      -fsanitize=fuzzer,address,undefined)
    add_executable(lanaudio_fuzz_rtp tests/fuzz/fuzz_rtp.cpp)
    target_link_libraries(lanaudio_fuzz_rtp PRIVATE lanaudio_core)
    target_compile_options(lanaudio_fuzz_rtp PRIVATE
      -fsanitize=fuzzer,address,undefined)
    target_link_options(lanaudio_fuzz_rtp PRIVATE
      -fsanitize=fuzzer,address,undefined)
  endif()
  if(APPLE AND TARGET lanaudio-desktop)
    add_test(NAME lanaudio_desktop_smoke
      COMMAND "$<TARGET_FILE:lanaudio-desktop>" --smoke-test)
    add_test(NAME lanaudio_bundle_plist
      COMMAND /usr/bin/plutil -lint
              "$<TARGET_BUNDLE_DIR:lanaudio-desktop>/Contents/Info.plist")
    set_tests_properties(
      lanaudio_cli_smoke lanaudio_desktop_smoke lanaudio_bundle_plist
      PROPERTIES LABELS "packaging")
  endif()
endif()

install(TARGETS lanaudio RUNTIME DESTINATION bin)
if(TARGET lanaudio-desktop)
  install(TARGETS lanaudio-desktop
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin)
  if(APPLE)
    install(PROGRAMS "$<TARGET_FILE:lanaudio>"
      DESTINATION "LanAudio.app/Contents/MacOS"
      RENAME "lanaudio-cli")
  endif()
  set(LANAUDIO_QT_DEPLOY_OPTIONS)
  if(Qt6_VERSION VERSION_GREATER_EQUAL 6.10)
    list(APPEND LANAUDIO_QT_DEPLOY_OPTIONS
      INCLUDE_PLUGIN_TYPES platforms styles)
  endif()
  qt_generate_deploy_app_script(
    TARGET lanaudio-desktop
    OUTPUT_SCRIPT LANAUDIO_DEPLOY_SCRIPT
    NO_UNSUPPORTED_PLATFORM_ERROR
    ${LANAUDIO_QT_DEPLOY_OPTIONS})
  install(SCRIPT ${LANAUDIO_DEPLOY_SCRIPT})
  if(APPLE)
    install(SCRIPT
      "${CMAKE_CURRENT_BINARY_DIR}/LanAudio-BundleDependencies.cmake")
    install(SCRIPT
      "${CMAKE_CURRENT_BINARY_DIR}/LanAudio-AdHocSign.cmake")
  endif()
endif()
install(FILES README.md LICENSE STATUS.md THIRD_PARTY_NOTICES.md
  SENDER-INSTRUCTIONS-INTEL.txt
  DESTINATION share/lanaudio)
install(DIRECTORY docs/ DESTINATION share/lanaudio/docs)

set(CPACK_PACKAGE_NAME "LanAudio")
set(CPACK_PACKAGE_VENDOR "LanAudio contributors")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Low-latency RTP audio for local networks")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
if(APPLE)
  set(CPACK_SYSTEM_NAME "macOS-${LANAUDIO_PACKAGE_ARCH}")
  set(CPACK_PACKAGE_ARCHITECTURE "${LANAUDIO_PACKAGE_ARCH}")
  set(CPACK_PACKAGE_FILE_NAME
    "LanAudio-${PROJECT_VERSION}-macOS-${LANAUDIO_PACKAGE_ARCH}")
  set(CPACK_DMG_VOLUME_NAME
    "LanAudio ${PROJECT_VERSION} (${LANAUDIO_PACKAGE_ARCH})")
  set(CPACK_GENERATOR "DragNDrop;TGZ")
elseif(WIN32)
  set(CPACK_GENERATOR "ZIP")
else()
  set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)
