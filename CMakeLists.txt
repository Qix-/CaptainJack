cmake_minimum_required (VERSION 3.0)
project (captian-jack)

option (CAPTAINJACK_DEBUG "Whether or not to build captain jack with debug messages" ON)

add_compile_options (-g3)

set (SRC
	captain-jack-device.c
)

set (JACK_SRC
	ext/jack/libjack/client.c
	ext/jack/libjack/intclient.c
	ext/jack/libjack/messagebuffer.c
	ext/jack/libjack/pool.c
	ext/jack/libjack/port.c
	ext/jack/libjack/metadata.c
	ext/jack/libjack/midiport.c
	ext/jack/libjack/ringbuffer.c
	ext/jack/libjack/shm.c
	ext/jack/libjack/thread.c
	ext/jack/libjack/time.c
	ext/jack/libjack/transclient.c
	ext/jack/libjack/unlock.c
	ext/jack/libjack/uuid.c
)

# JACK
execute_process (COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/patch-jack.sh")
include_directories ("${CMAKE_CURRENT_SOURCE_DIR}/ext/jack/libjack")
include_directories ("${CMAKE_CURRENT_SOURCE_DIR}/ext/jack/")
include_directories ("${CMAKE_CURRENT_SOURCE_DIR}/ext/jack/config")
include_directories ("${CMAKE_CURRENT_SOURCE_DIR}/ext/jack/include")
add_definitions (-DHAVE_CONFIG_H -D_REENTRANT -D_POSIX_PTHREAD_SEMANTICS)
add_definitions (-DJACK_LOCATION="/usr/local/bin" -DJACK_ASSUME_ROOT)
add_library (jack-static ${JACK_SRC})

# Captain Jack
if (CAPTAINJACK_DEBUG)
	add_definitions (-DDEBUG_SYSLOG)
endif ()
add_executable (captain-jack ${SRC})
set_target_properties (captain-jack PROPERTIES COMPILE_FLAGS "-Wall -Wextra -Werror")
set_target_properties (captain-jack PROPERTIES LINK_FLAGS -bundle)
target_link_libraries (captain-jack jack-static)
target_link_libraries (captain-jack "-framework CoreFoundation")
target_link_libraries (captain-jack "-framework CoreAudio")
