#ifndef CAPTAIN_JACK_XMIT_H__
#define CAPTAIN_JACK_XMIT_H__
/*
	,---.         .              ,-_/
	|  -' ,-. ,-. |- ,-. . ,-.   '  | ,-. ,-. . ,
	|   . ,-| | | |  ,-| | | |      | ,-| |   |/
	`---' `-^ |-' `' `-^ ' ' '      | `-^ `-' |\
	          |                  /  |         ' `
	          '                  `--'
	          captain jack audio device
	         github.com/qix-/captainjack

	        copyright (c) 2016 josh junon
	        released under the MIT license
*/

/*
	this subsystem supplies both the driver and the
	daemon with a means for RPC and IPC via loopback
	sockets.

	this is because the only other means of IPC was
	to use XPC, which is god awful and requires a main
	loop, which a bundle does not really facilitate.

	however, thanks to Apple giving 'just enough'
	wiggle room, we are allowed the luxuries of net-
	work comms and thus can have a pretty robust IPC
	mechanism; with Captain Jack, it's the xmit sub-
	system.

	the driver received an implementation of functions
	it can call, and the daemon implements such methods
	in the same struct. when the driver calls the
	method, the message is transmitted and processed
	automatically by the daemon which invokes a callback
	in the daemon to process the message.
*/

#include <stdbool.h>
#include <stdlib.h>

typedef struct {
	/*
		called when a new audio client connects
	*/
	void (*do_client_connect)(pid_t);
} CaptainJack_Xmitter;

/*
	gets an xmitter server for the driver device

	NOTE: this is for the device driver!
*/
CaptainJack_Xmitter * CaptainJack_GetXmitterServer(void);


/*
	registers an xmit client with the subsystem

	NOTE: this is for the daemon!
*/
void CaptainJack_RegisterXmitterClient(CaptainJack_Xmitter *);

/*
	processes any and all pending messages.
	call this ~f r e q u e n t l y~.

	NOTE: this is for the daemon!
*/
bool CaptainJack_TickXmitter(void);

#endif
