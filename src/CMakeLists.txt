cmake_minimum_required (VERSION 3.0)
project (captian-jack)

set (COMMON_SRC
	xmit.c
)

add_definitions (-Wall -Wextra -Werror -g3 -std=c99)
add_definitions (-mmacosx-version-min=10.9)

add_executable (captain-jack captain-jack-device.c ${COMMON_SRC})
set_target_properties (captain-jack PROPERTIES LINK_FLAGS -bundle)
target_link_libraries (captain-jack "-framework CoreFoundation")
target_link_libraries (captain-jack "-framework CoreAudio")

add_executable (captain-jack-daemon captain-jack-daemon.c ${COMMON_SRC})
target_link_libraries (captain-jack-daemon "-framework CoreFoundation")
