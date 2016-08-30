cmake_minimum_required (VERSION 3.0)
project (captian-jack)

set (SRC
	captain-jack-device.c
)

add_executable (captain-jack ${SRC})
set_target_properties (captain-jack PROPERTIES LINK_FLAGS -bundle)
target_link_libraries (captain-jack "-framework CoreFoundation")
target_link_libraries (captain-jack "-framework CoreAudio")
