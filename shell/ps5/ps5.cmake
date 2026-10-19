# PSFlyCast: the platform layer, and the link into a signed eboot.
# Included by the top-level CMakeLists.txt when FLYCAST_PS5 is set (the
# toolchain file shell/ps5/ps5-toolchain.cmake sets it).

target_compile_definitions(${PROJECT_NAME} PRIVATE USE_PS5 FLYCAST_BIGPICTURE)
target_include_directories(${PROJECT_NAME} PRIVATE shell/ps5)
target_sources(${PROJECT_NAME} PRIVATE
	shell/ps5/ps5_main.cpp
	shell/ps5/ps5_pad.cpp
	shell/ps5/ps5_pad.h
	shell/ps5/ps5_audio.cpp
	shell/ps5/ps5_libc.cpp
	shell/ps5/ps5_diag.cpp
	shell/ps5/ps5_threads.cpp
	shell/ps5/ps5_stdio.cpp
	shell/ps5/ps5_pipelines.cpp
	shell/ps5/ps5_covers.cpp
	shell/ps5/ps5_tls.cpp
	shell/ps5/ps5_library.cpp
	shell/ps5/ps5_smb.cpp
	shell/ps5/ps5_disc.cpp
	shell/ps5/ps5_video.cpp
	shell/ps5/ps5_archive.cpp
	shell/ps5/ps5_rar.cpp
	shell/ps5/ps5_drive_cam.cpp
	shell/ps5/ps5_drive_usb.cpp
	shell/ps5/ps5_cheats.cpp
	shell/ps5/ps5_patches.cpp
	shell/ps5/ps5_mods.cpp
	shell/ps5/ps5_tools.cpp
	shell/ps5/ps5_clock.c
	shell/ps5/ps5_clock.h
	shell/ps5/ps5_drawdist.cpp
	shell/ps5/ps5_framegen.cpp
	shell/ps5/ps5_framegen_ext.cpp
	shell/ps5/ps5_mihawkgen_model.cpp
	shell/ps5/third_party/ps5-framegen/src/ps5_framegen.cpp
	shell/ps5/third_party/mihawkgen/src/fg_common.cpp
	shell/ps5/third_party/mihawkgen/src/fg_vulkan.cpp
	shell/ps5/third_party/mihawkgen/src/fg_neural.cpp
	shell/ps5/third_party/mihawkgen/src/fg_splat.cpp
	shell/ps5/ps5_perf.cpp
	shell/ps5/ps5_framegen.h
	shell/ps5/ps5_border.cpp
	shell/ps5/ps5_fsr.cpp
	shell/ps5/ps5_update.cpp
	shell/ps5/ps5_rewind.cpp
	shell/ps5/ps5_rewind.h
	shell/ps5/ps5_vmu.cpp
	shell/ps5/ps5_vmu.h
	shell/ps5/ps5_usbinput.cpp
	shell/ps5/ps5_texdump.cpp
	shell/ps5/ps5_ime.cpp
	shell/ps5/ps5_fsr.h
	shell/ps5/ps5_frontend.h
	shell/ps5/elevation/elevation.cpp
	shell/ps5/elevation/ps5_elevate.cpp
	shell/ps5/ps5_diag.h
	shell/ps5/bigpicture.cpp
	shell/ps5/bigpicture.h)

# FSR 1's two headers (shell/ps5/fsr, AMD's, MIT) are compiled at run time as
# GLSL: each is wrapped in a string literal for ps5_fsr.cpp to include.
foreach(header ffx_a ffx_fsr1 ffx_cas)
	file(READ ${CMAKE_CURRENT_SOURCE_DIR}/shell/ps5/fsr/${header}.h FSR_TEXT)
	file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ps5_generated/${header}.inc.new "R\"FFX(${FSR_TEXT})FFX\"")
	execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${CMAKE_CURRENT_BINARY_DIR}/ps5_generated/${header}.inc.new ${CMAKE_CURRENT_BINARY_DIR}/ps5_generated/${header}.inc)
endforeach()
# NVIDIA Image Scaling's shader (shell/ps5/nis, MIT) the same way.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/shell/ps5/nis/NIS_Scaler.h NIS_TEXT)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ps5_generated/NIS_Scaler.inc.new "R\"NIS(${NIS_TEXT})NIS\"")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
	${CMAKE_CURRENT_BINARY_DIR}/ps5_generated/NIS_Scaler.inc.new ${CMAKE_CURRENT_BINARY_DIR}/ps5_generated/NIS_Scaler.inc)
target_include_directories(${PROJECT_NAME} PRIVATE shell/ps5/nis)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/ps5_generated)

# CMake gives the executable's objects and archives, in link order, to
# ps5-link.sh, which links them with RADV and the console's runtime, then
# converts and signs the result (the recipe of PS5_Vulkan's RADV title).
set(CMAKE_CXX_LINK_EXECUTABLE
	"bash ${CMAKE_CURRENT_SOURCE_DIR}/shell/ps5/ps5-link.sh <TARGET> <OBJECTS> -- <LINK_LIBRARIES>")

# The other two frame generators (third_party/, unmodified): their Vulkan calls go through the table of ps5_vkc.h.
# ps5-framegen has a namespace ps5::framegen like the title's own: it is built with `ps5` renamed ps5fg.
set_source_files_properties(shell/ps5/third_party/ps5-framegen/src/ps5_framegen.cpp PROPERTIES COMPILE_OPTIONS
	"-std=c++17;-w;-Dps5=ps5fg;-include;${CMAKE_CURRENT_SOURCE_DIR}/shell/ps5/ps5_vkc.h"
	COMPILE_DEFINITIONS "PS5_FRAMEGEN_VULKAN_HEADER=\"ps5_vkc.h\"")
set_source_files_properties(
	shell/ps5/third_party/mihawkgen/src/fg_common.cpp shell/ps5/third_party/mihawkgen/src/fg_vulkan.cpp
	shell/ps5/third_party/mihawkgen/src/fg_neural.cpp shell/ps5/third_party/mihawkgen/src/fg_splat.cpp
	PROPERTIES COMPILE_OPTIONS "-std=c++20;-w;-include;${CMAKE_CURRENT_SOURCE_DIR}/shell/ps5/ps5_vkc.h")
target_include_directories(${PROJECT_NAME} PRIVATE shell/ps5/third_party/mihawkgen/src shell/ps5/third_party/mihawkgen/include)
# MihawkGen's trained network (1.7 MB), assembled into the title.
set_source_files_properties(shell/ps5/ps5_mihawkgen_model.cpp PROPERTIES
	COMPILE_DEFINITIONS "MIHAWKGEN_MODEL_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/shell/ps5/third_party/mihawkgen/model/network.fgw\""
	OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shell/ps5/third_party/mihawkgen/model/network.fgw")

# RAR archives (shell/ps5/ps5_rar.cpp): libunarr's RAR reader (LGPL-3.0, third_party/unarr, unmodified).
set(UNARR_DIR shell/ps5/third_party/unarr)
target_sources(${PROJECT_NAME} PRIVATE
	${UNARR_DIR}/common/conv.c ${UNARR_DIR}/common/crc32.c ${UNARR_DIR}/common/stream.c ${UNARR_DIR}/common/unarr.c
	${UNARR_DIR}/rar/filter-rar.c ${UNARR_DIR}/rar/huffman-rar.c ${UNARR_DIR}/rar/parse-rar.c ${UNARR_DIR}/rar/rar.c
	${UNARR_DIR}/rar/rarvm.c ${UNARR_DIR}/rar/uncompress-rar.c
	${UNARR_DIR}/lzmasdk/Ppmd7.c ${UNARR_DIR}/lzmasdk/Ppmd7Dec.c ${UNARR_DIR}/lzmasdk/Ppmd7aDec.c)
set_source_files_properties(${UNARR_DIR}/common/conv.c ${UNARR_DIR}/common/crc32.c ${UNARR_DIR}/common/stream.c
	${UNARR_DIR}/common/unarr.c ${UNARR_DIR}/rar/filter-rar.c ${UNARR_DIR}/rar/huffman-rar.c ${UNARR_DIR}/rar/parse-rar.c
	${UNARR_DIR}/rar/rar.c ${UNARR_DIR}/rar/rarvm.c ${UNARR_DIR}/rar/uncompress-rar.c ${UNARR_DIR}/lzmasdk/Ppmd7.c
	${UNARR_DIR}/lzmasdk/Ppmd7Dec.c ${UNARR_DIR}/lzmasdk/Ppmd7aDec.c PROPERTIES COMPILE_OPTIONS "-w")
target_include_directories(${PROJECT_NAME} PRIVATE ${UNARR_DIR}/common)

# The sandbox elevation client (ps5-native-app-boilerplate's example) is C++20.
set_source_files_properties(shell/ps5/elevation/elevation.cpp shell/ps5/elevation/ps5_elevate.cpp
	PROPERTIES COMPILE_OPTIONS "-std=c++20")

# Games on SMB shares (shell/ps5/ps5_smb.cpp): libsmb2 (LGPL-2.1), compiled
# from a checkout beside this repository or the one LIBSMB2_DIR names, with
# shell/ps5/smb/config.h. Four libc names it uses are bound to
# shell/ps5/smb/ps5_smb_compat.c, and SOL_TCP is given so that it does not
# look the protocol up.
if(DEFINED ENV{LIBSMB2_DIR})
	set(LIBSMB2_DIR "$ENV{LIBSMB2_DIR}")
else()
	set(LIBSMB2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libsmb2")
endif()
if(NOT EXISTS "${LIBSMB2_DIR}/lib/libsmb2.c")
	message(FATAL_ERROR "libsmb2 was not found at ${LIBSMB2_DIR}: git clone https://github.com/sahlberg/libsmb2 there, or set LIBSMB2_DIR")
endif()
set(LIBSMB2_NAMES
	aes aes_reference aes128ccm alloc asn1-ber compat errors hmac hmac-md5 init libsmb2 md4c md5 ntlmssp pdu
	sha1 sha224-256 sha384-512 smb2-cmd-cancel smb2-cmd-close smb2-cmd-create smb2-cmd-echo smb2-cmd-error
	smb2-cmd-flush smb2-cmd-ioctl smb2-cmd-lock smb2-cmd-logoff smb2-cmd-negotiate smb2-cmd-notify-change
	smb2-cmd-oplock-break smb2-cmd-query-directory smb2-cmd-query-info smb2-cmd-read smb2-cmd-session-setup
	smb2-cmd-set-info smb2-cmd-tree-connect smb2-cmd-tree-disconnect smb2-cmd-write smb2-data-file-info
	smb2-data-filesystem-info smb2-data-security-descriptor smb2-data-reparse-point smb2-share-enum smb3-seal
	smb2-signing socket spnego-wrapper sync timestamps unicode usha)
set(LIBSMB2_SOURCES shell/ps5/smb/ps5_smb_compat.c)
foreach(name ${LIBSMB2_NAMES})
	list(APPEND LIBSMB2_SOURCES "${LIBSMB2_DIR}/lib/${name}.c")
endforeach()
add_library(smb2ps5 STATIC ${LIBSMB2_SOURCES})
target_include_directories(smb2ps5 PRIVATE shell/ps5/smb "${LIBSMB2_DIR}/include" "${LIBSMB2_DIR}/include/smb2" "${LIBSMB2_DIR}/lib")
target_compile_definitions(smb2ps5 PRIVATE HAVE_CONFIG_H "_U_=__attribute__((unused))" SOL_TCP=6
	getlogin_r=ps5_smb_getlogin_r gethostname=ps5_smb_gethostname readv=ps5_smb_readv writev=ps5_smb_writev
	getaddrinfo=ps5_smb_getaddrinfo freeaddrinfo=ps5_smb_freeaddrinfo
	fcntl=ps5_smb_fcntl connect=ps5_smb_connect)
target_compile_options(smb2ps5 PRIVATE -w)
target_include_directories(${PROJECT_NAME} PRIVATE "${LIBSMB2_DIR}/include")
target_link_libraries(${PROJECT_NAME} PRIVATE smb2ps5)

# HTTPS once the sandbox has been left (shell/ps5/ps5_tls.cpp, as PSSwanStation
# does it): mbedTLS 3.6 LTS (Apache-2.0), compiled from a checkout beside this
# repository (../deps-src/mbedtls-3.6.7) or the one MBEDTLS_DIR names, its
# library/ as it is, with shell/ps5/ps5_mbedtls_config.h over the default
# configuration.
if(DEFINED ENV{MBEDTLS_DIR})
	set(MBEDTLS_DIR "$ENV{MBEDTLS_DIR}")
else()
	set(MBEDTLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../deps-src/mbedtls-3.6.7")
endif()
if(NOT EXISTS "${MBEDTLS_DIR}/library/ssl_tls.c")
	message(FATAL_ERROR "mbedTLS was not found at ${MBEDTLS_DIR}: git clone --branch v3.6.7 https://github.com/Mbed-TLS/mbedtls there, or set MBEDTLS_DIR")
endif()
file(GLOB MBEDTLS_SOURCES "${MBEDTLS_DIR}/library/*.c")
add_library(mbedtlsps5 STATIC ${MBEDTLS_SOURCES})
target_include_directories(mbedtlsps5 PUBLIC "${MBEDTLS_DIR}/include" PRIVATE "${MBEDTLS_DIR}/library")
target_compile_definitions(mbedtlsps5 PUBLIC
	"MBEDTLS_USER_CONFIG_FILE=\"${CMAKE_CURRENT_SOURCE_DIR}/shell/ps5/ps5_mbedtls_config.h\"")
get_filename_component(MBEDTLS_REAL_DIR "${MBEDTLS_DIR}" REALPATH)
target_compile_options(mbedtlsps5 PRIVATE -w "-ffile-prefix-map=${MBEDTLS_REAL_DIR}/=mbedtls/"
	"-ffile-prefix-map=${MBEDTLS_DIR}/=mbedtls/")
target_link_libraries(${PROJECT_NAME} PRIVATE mbedtlsps5)

# Game videos (shell/ps5/ps5_video.cpp): FFmpeg's libavformat, libavcodec and
# libavutil (LGPL-2.1-or-later), built for the console by
# shell/ps5/tools/build-ffmpeg.sh into FFMPEG_ROOT (build.sh does it).
set(FFMPEG_ROOT "${CMAKE_BINARY_DIR}/ffmpeg" CACHE PATH "FFmpeg for the console: include/ and lib/")
if(NOT EXISTS "${FFMPEG_ROOT}/lib/libavformat.a")
	message(FATAL_ERROR "FFmpeg was not found at ${FFMPEG_ROOT}: shell/ps5/tools/build-ffmpeg.sh ps5 ${FFMPEG_ROOT}")
endif()
target_include_directories(${PROJECT_NAME} PRIVATE "${FFMPEG_ROOT}/include")
target_link_libraries(${PROJECT_NAME} PRIVATE
	"${FFMPEG_ROOT}/lib/libavformat.a" "${FFMPEG_ROOT}/lib/libavcodec.a" "${FFMPEG_ROOT}/lib/libavutil.a")

# The title's monotonic clock from the processor's counter (shell/ps5/ps5_clock.c).
set_source_files_properties(shell/ps5/ps5_clock.c PROPERTIES COMPILE_OPTIONS "-std=gnu11")
