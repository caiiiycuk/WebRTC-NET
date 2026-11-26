CMAKE_MINIMUM_REQUIRED(VERSION 3.5)

set(HUMBLENET_SERVER_URL "ws://localhost:8080/ws" CACHE STRING "The peer server to use for the test apps")

add_custom_command(
		OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/test_peer_flatbuffers.h"
		COMMAND "${BIN_FLATC}" --cpp --scoped-enums
		-o "."
		"${CMAKE_CURRENT_SOURCE_DIR}/test_peer.fbs"
		DEPENDS "${FLATC_DEPENDS}")

CreateTool(humblenet_test_peer
FILES
	test_peer.cpp
	"${CMAKE_CURRENT_BINARY_DIR}/test_peer_flatbuffers.h"
REFERENCE
	test_peer.fbs
DEFINES
	HUMBLENET_SERVER_URL=\"${HUMBLENET_SERVER_URL}\"
FEATURES
	cxx_auto_type cxx_range_for
INCLUDES
	${CMAKE_CURRENT_BINARY_DIR}
LINK
	humblenet
	flatbuffers
	${CAP_LIB}
	-g
PROPERTIES
	FOLDER WebRTC-NET/Tests
)

list(APPEND TEST_TARGETS
	humblenet_test_peer
)

if(HUMBLENET_DESKTOP)
	CreateTool(humblenet_test_webrtc
	FILES
		test_webrtc.cpp
	ASM_FLAGS
		ASSERTIONS=2
		DEMANGLE_SUPPORT=1
	DEFINES
		HUMBLENET_SERVER_URL=\"${HUMBLENET_SERVER_URL}\"
	FEATURES
		cxx_auto_type cxx_range_for
	LINK
		humblenet
		${CAP_LIB}
		-g
	PROPERTIES
		FOLDER WebRTC-NET/Tests
	)
	list(APPEND TEST_TARGETS
		humblenet_test_webrtc
	)

	CreateTool(humblenet_test_loader
	FILES
		test_loader.cpp
	LINK
		humblenet_loader
	PROPERTIES
		FOLDER WebRTC-NET/Tests
	)
	list(APPEND TEST_TARGETS
		humblenet_test_loader
	)

	add_dependencies(humblenet_test_loader humblenet_shared)
	CreateTool(humblenet_test_portal
	FILES
		portal_abi.cpp
	LINK
		humblenet_shared
	)
	list(APPEND TEST_TARGETS humblenet_test_portal)
endif()

CreateTool(humblenet_hello_world
FILES
	hello_world.cpp
DEFINES
	HUMBLENET_SERVER_URL=\"${HUMBLENET_SERVER_URL}\"
FEATURES
	cxx_auto_type cxx_range_for
LINK
	humblenet
	-g
	${CAP_LIB}
PROPERTIES
	FOLDER WebRTC-NET/Tests
)
list(APPEND TEST_TARGETS
	humblenet_hello_world
)

if(TEST_TARGETS)
	add_custom_target(all_tests)
	set_target_properties(all_tests PROPERTIES FOLDER WebRTC-NET/Tests)
	add_dependencies(all_tests ${TEST_TARGETS})
endif()
