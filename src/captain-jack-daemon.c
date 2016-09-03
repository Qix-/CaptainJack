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

#include <libproc.h>
#include <stdbool.h>
#include <sys/syslog.h>
#include <unistd.h>

#include "xmit.h"

static void on_ready(void) {
	syslog(LOG_NOTICE, "device has signaled it's ready");
}

static void on_new_client(unsigned int cid, pid_t pid) {
	char buffer[1024];
	if (proc_name(pid, &buffer[0], sizeof(buffer)) == -1) {
		syslog(LOG_NOTICE, "could not get name of client: %u (%u)", cid, pid);
	} else {
		syslog(LOG_NOTICE, "client connected: %u (%s %u)", cid, &buffer[0], pid);
	}
}

static void on_client_disconnect(unsigned int cid, pid_t pid) {
	syslog(LOG_NOTICE, "client disconnected: %u (%d)", cid, pid);
}

static void on_client_enables_io(unsigned int cid) {
	syslog(LOG_NOTICE, "client enabled IO: %u", cid);
}

static void on_client_disables_io(unsigned int cid) {
	syslog(LOG_NOTICE, "client disabled IO: %u", cid);
}

static CaptainJack_Xmitter xmitterClient = {
	&on_ready,
	&on_new_client,
	&on_client_disconnect,
	&on_client_enables_io,
	&on_client_disables_io,
};

int main(void) {
	openlog("CaptainJack", LOG_NDELAY | LOG_PERROR | LOG_PID, LOG_DAEMON);
	setlogmask(0);
	syslog(LOG_NOTICE, "Captain Jack is portside at ye embarcadero");

	CaptainJack_RegisterXmitterClient(&xmitterClient);

	bool shouldRunAgain = true;
	while (shouldRunAgain) {
		usleep(10000);

		if (!CaptainJack_TickXmitter()) {
			shouldRunAgain = false;
		}
	}

	syslog(LOG_CRIT, "terminating");

	return EXIT_FAILURE;
}
