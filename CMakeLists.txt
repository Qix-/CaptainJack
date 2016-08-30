cmake_minimum_required (VERSION 3.0)
project (captian-jack)

option (CAPTAINJACK_DEBUG "Whether or not to build captain jack with debug messages" OFF)

set (SRC
	captain-jack-device.c
)

if (CAPTAINJACK_DEBUG)
	add_definitions (-DDEBUG)
endif ()

add_executable (captain-jack ${SRC})
set_target_properties (captain-jack PROPERTIES LINK_FLAGS -bundle)
target_link_libraries (captain-jack "-framework CoreFoundation")
target_link_libraries (captain-jack "-framework CoreAudio")
