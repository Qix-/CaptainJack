cmake_minimum_required (VERSION 3.0)
project (captian-jack)
option (CAPTAINJACK_DEBUG "Whether or not to build captain jack with debug messages" ON)

if (CAPTAINJACK_DEBUG)
	add_definitions (-DDEBUG)
endif ()

add_definitions (-Wall -Wextra -Werror -g3)
add_definitions (-mmacosx-version-min=10.9)

add_executable (captain-jack captain-jack-device.c)
set_target_properties (captain-jack PROPERTIES LINK_FLAGS -bundle)
target_link_libraries (captain-jack "-framework CoreFoundation")
target_link_libraries (captain-jack "-framework CoreAudio")

add_executable (captain-jack-daemon captain-jack-daemon.c)
target_link_libraries (captain-jack-daemon "-framework CoreFoundation")
