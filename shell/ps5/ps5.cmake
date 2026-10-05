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
	shell/ps5/ps5_smb.cpp
	shell/ps5/ps5_cheats.cpp
	shell/ps5/ps5_patches.cpp
	shell/ps5/ps5_drawdist.cpp
	shell/ps5/ps5_fsr.cpp
	shell/ps5/ps5_update.cpp
	shell/ps5/ps5_usbinput.cpp
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
foreach(header ffx_a ffx_fsr1)
	file(READ ${CMAKE_CURRENT_SOURCE_DIR}/shell/ps5/fsr/${header}.h FSR_TEXT)
	file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ps5_generated/${header}.inc.new "R\"FFX(${FSR_TEXT})FFX\"")
	execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${CMAKE_CURRENT_BINARY_DIR}/ps5_generated/${header}.inc.new ${CMAKE_CURRENT_BINARY_DIR}/ps5_generated/${header}.inc)
endforeach()
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/ps5_generated)

# CMake gives the executable's objects and archives, in link order, to
# ps5-link.sh, which links them with RADV and the console's runtime, then
# converts and signs the result (the recipe of PS5_Vulkan's RADV title).
set(CMAKE_CXX_LINK_EXECUTABLE
	"bash ${CMAKE_CURRENT_SOURCE_DIR}/shell/ps5/ps5-link.sh <TARGET> <OBJECTS> -- <LINK_LIBRARIES>")

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
