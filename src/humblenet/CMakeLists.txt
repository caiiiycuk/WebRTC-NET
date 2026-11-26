CMAKE_MINIMUM_REQUIRED(VERSION 3.5)


if(HUMBLENET_NATIVE)
	list(APPEND PLATFORM_LIBRARIES
		webrtc_microstack websockets ${LWS_SSL_LIBS}
		${CAP_LIB}
		${CMAKE_THREAD_LIBS_INIT}
	)
endif()
if(NOT WIN32)
	list(APPEND PLATFORM_DEFINES
		_POSIX
	)
endif()

if(HUMBLENET_DESKTOP)
	add_custom_command(OUTPUT exported_osx.map
		COMMAND APIBuilder osx ${CMAKE_CURRENT_SOURCE_DIR}/HumbleNet_API.json ${CMAKE_CURRENT_BINARY_DIR}/exported_osx.map
		MAIN_DEPENDENCY HumbleNet_API.json
		DEPENDS APIBuilder
		COMMENT "Process API to OS X exported symbol list"
	)
	add_custom_command(OUTPUT exported_linux.map
		COMMAND APIBuilder linux ${CMAKE_CURRENT_SOURCE_DIR}/HumbleNet_API.json ${CMAKE_CURRENT_BINARY_DIR}/exported_linux.map
		MAIN_DEPENDENCY HumbleNet_API.json
		DEPENDS APIBuilder
		COMMENT "Process API to Linux exported symbol list"
	)
	add_custom_command(OUTPUT cert_pem.h
		COMMAND APIBuilder include ${CMAKE_CURRENT_SOURCE_DIR}/cert.pem ${CMAKE_CURRENT_BINARY_DIR}/cert_pem.h
		MAIN_DEPENDENCY cert.pem
		DEPENDS APIBuilder
		COMMENT "Converting cert.pem into a header"
	)
	if(APPLE)
		set(PLATFORM_EXPORT "-exported_symbols_list ${CMAKE_CURRENT_BINARY_DIR}/exported_osx.map")
		set(PLATFORM_EXPORT_FILE ${CMAKE_CURRENT_BINARY_DIR}/exported_osx.map)
	elseif(LINUX)
		set(PLATFORM_EXPORT "-Wl,--version-script=${CMAKE_CURRENT_BINARY_DIR}/exported_linux.map")
		set(PLATFORM_EXPORT_FILE ${CMAKE_CURRENT_BINARY_DIR}/exported_linux.map)
	endif()

	if(PLATFORM_EXPORT_FILE)
		set_source_files_properties(${PLATFORM_EXPORT_FILE}
			PROPERTIES
				HEADER_FILE_ONLY TRUE
		)
	endif()
	set(CERT_PEM_H ${CMAKE_CURRENT_BINARY_DIR}/cert_pem.h)
endif()

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/humblepeer_generated.h"
    COMMAND "${BIN_FLATC}" --cpp --scoped-enums
            -o "."
            "${CMAKE_CURRENT_SOURCE_DIR}/humblepeer/humblepeer.fbs"
    DEPENDS "${FLATC_DEPENDS}")

message(STATUS CMAKE_CURRENT_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})

CreateLibrary(humblepeer
	DIRS
		humblepeer
	FILES
		${FLATBUFFER_SOURCES}
		"${CMAKE_CURRENT_BINARY_DIR}/humblepeer_generated.h"
	REFERENCE
		humblepeer/*.fbs
	INCLUDES
		${CMAKE_CURRENT_BINARY_DIR}
		../../3rdparty/flatbuffers/include/
		flatbuffers
	PUBLIC
			${CMAKE_CURRENT_BINARY_DIR}
			../../3rdparty/flatbuffers/include/
			include
			humblepeer
			flatbuffers
	INTERFACE
		flatbuffers
		src
	FLAGS
		PRIVATE
			$<$<BOOL:${LINUX_X86_64}>:-fPIC>
	FEATURES
		cxx_auto_type cxx_range_for cxx_strong_enums
	LINK
		PUBLIC
			flatbuffers
		PRIVATE
			crc
			sha1
	PROPERTIES
		FOLDER WebRTC-NET/Library
	DEPENDS
		"${CMAKE_CURRENT_BINARY_DIR}/humblepeer_generated.h"
	)

set(CORE_HUMBLENET
	DIRS
		src
		include
	FILES
		${CERT_PEM_H}
	REFERENCE
		*.json
	INCLUDES
		PUBLIC
			include
		PRIVATE
			src
			../../3rdparty/json-parser
	FEATURES
		cxx_auto_type cxx_range_for
	DEFINES
		${PLATFORM_DEFINES}
		PRIVATE
			$<$<BOOL:${HAVE_DLOPEN}>:HAVE_DLOPEN>
			$<$<AND:$<BOOL:${HAVE_DLOPEN}>,$<BOOL:${LINUX}>>:LOAD_WEBRTC_SO>
			HUMBLENET_LOAD_WEBRTC
	LINK
		humblepeer
		jsonparser
		${PLATFORM_LIBRARIES}
		$<$<BOOL:${DL_LIBRARY}>:${DL_LIBRARY}>
	PROPERTIES
		FOLDER WebRTC-NET/Library
)

set(CORE_HUMBLENET_LOADER
	DIRS
		loader
		include
	FILES
		${CMAKE_CURRENT_BINARY_DIR}/humblenet_loader_procs.h
	INCLUDES
		PUBLIC
			include
		PRIVATE
			loader
			${CMAKE_CURRENT_BINARY_DIR}
	DEFINES
		PRIVATE
			$<$<BOOL:${HAVE_DLOPEN}>:HAVE_DLOPEN>
	FEATURES
		cxx_auto_type cxx_range_for
	LINK
		$<$<BOOL:${DL_LIBRARY}>:${DL_LIBRARY}>
	PROPERTIES
		FOLDER WebRTC-NET/Library
)

CreateLibrary(humblenet
	${CORE_HUMBLENET}
	DEFINES
		PUBLIC
			HUMBLENET_STATIC
	ASM_FLAGS
		EXPORTED_RUNTIME_METHODS='["cwrap","out","stringToNewUTF8","lengthBytesUTF8","UTF8ToString","HEAPU8","HEAP32"]'
	PROPERTIES
		OUTPUT_NAME humblenet_static
)

if(HUMBLENET_DESKTOP)
	set(amalgam_output ${CMAKE_BINARY_DIR}/humblenet_asmjs_amalgam.cpp)

	add_custom_target(humblenet_asmjs_amalgamation
		COMMAND Amalgamate
			-i src -i humblepeer -i include -i ${CMAKE_CURRENT_BINARY_DIR}
			-i ../../3rdparty/flatbuffers/include
			-i ../../3rdparty/crc
			-i ../../3rdparty/sha1
			humblenet_asmjs_amalgam_template.cpp
			${amalgam_output}
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		BYPRODUCTS ${amalgam_output}
		SOURCES humblenet_asmjs_amalgam_template.cpp

		COMMENT "Building source amalgamation for Emscripten WebRTC-NET"
	)
	set_target_properties(humblenet_asmjs_amalgamation
		PROPERTIES
			FOLDER "WebRTC-NET/Library"
	)

	add_custom_command(OUTPUT humblenet_loader_procs.h
		COMMAND APIBuilder loader ${CMAKE_CURRENT_SOURCE_DIR}/HumbleNet_API.json ${CMAKE_CURRENT_BINARY_DIR}/humblenet_loader_procs.h
		MAIN_DEPENDENCY HumbleNet_API.json
		DEPENDS APIBuilder
		COMMENT "Process API to humblenet_loader_procs.h"
	)

	CreateLibrary(humblenet_loader
	${CORE_HUMBLENET_LOADER}
	DEFINES
		PUBLIC
			HUMBLENET_STATIC
	)
endif()

if(NOT EMSCRIPTEN)

	set(HUMBLENET_WEBRTC_SHARED_LINK_TARGET "")
	set(HUMBLENET_WEBRTC_SHARED_SOURCE "webrtc/webrtc_native_linux.cpp")
	set(HUMBLENET_WEBRTC_SHARED_FEATURES cxx_auto_type cxx_range_for cxx_std_20)

	if(HUMBLENET_EXTERNAL_WEBRTC AND TARGET humblenet_external_webrtc)
		set(HUMBLENET_WEBRTC_SHARED_LINK_TARGET humblenet_external_webrtc)
	endif()

	if(HUMBLENET_WEBRTC_SHARED_LINK_TARGET)
		if(APPLE)
			set(WEBRTC_PLATFORM_EXPORT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/webrtc/exported_osx.map")
			set(WEBRTC_PLATFORM_EXPORT "-exported_symbols_list ${WEBRTC_PLATFORM_EXPORT_FILE}")
		elseif(LINUX)
			set(WEBRTC_PLATFORM_EXPORT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/webrtc/exported_linux.map")
			set(WEBRTC_PLATFORM_EXPORT "-Wl,--version-script=${WEBRTC_PLATFORM_EXPORT_FILE}")
		elseif(WIN32)
			set(WEBRTC_PLATFORM_EXPORT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/webrtc/exported_win.def")
		endif()
		
		CreateSharedLibrary(webrtc_shared
		DIRS
			webrtc
		FILES
			${CMAKE_CURRENT_SOURCE_DIR}/${HUMBLENET_WEBRTC_SHARED_SOURCE}
			${WEBRTC_PLATFORM_EXPORT_FILE}
		EXCLUDE
			webrtc/webrtc_native_linux.cpp
		INCLUDES
			PRIVATE
				src
		DEFINES
				WEBRTC_DLL_BUILD
				$<$<BOOL:${HUMBLENET_EXTERNAL_WEBRTC}>:HUMBLENET_EXTERNAL_WEBRTC_PROVIDER>
				$<$<BOOL:${HUMBLENET_EXTERNAL_WEBRTC}>:WEBRTC_POSIX>
		FEATURES
			${HUMBLENET_WEBRTC_SHARED_FEATURES}
		PROPERTIES
			OUTPUT_NAME webrtc
			FOLDER WebRTC-NET/Library
			LINK
				${HUMBLENET_WEBRTC_SHARED_LINK_TARGET}
				${WEBRTC_PLATFORM_EXPORT}
			)

			set(_webrtc_copy_script "${CMAKE_CURRENT_BINARY_DIR}/copy_webrtc_shared_to_build_root.cmake")
			file(WRITE "${_webrtc_copy_script}"
				"get_filename_component(target_dir \"\${TARGET_FILE}\" DIRECTORY)\n"
				"if(NOT target_dir STREQUAL \"\${BUILD_DIR}\")\n"
				"  execute_process(COMMAND \"${CMAKE_COMMAND}\" -E copy_if_different \"\${TARGET_FILE}\" \"\${BUILD_DIR}/\")\n"
				"endif()\n"
			)
			add_custom_command(TARGET webrtc_shared
				POST_BUILD
				COMMAND "${CMAKE_COMMAND}"
					-DTARGET_FILE="$<TARGET_FILE:webrtc_shared>"
					-DBUILD_DIR="${CMAKE_BINARY_DIR}"
					-P "${_webrtc_copy_script}"
				COMMENT "Copying libwebrtc.so to the build root"
			)
		endif()
		
		CreateSharedLibrary(humblenet_shared
	${CORE_HUMBLENET}
	FILES
		${PLATFORM_EXPORT_FILE}
	DEFINES
		PRIVATE
			HUMBLENET_DLL_BUILD
	LINK
		${PLATFORM_EXPORT}
	PROPERTIES
		OUTPUT_NAME humblenet
	)
endif()
