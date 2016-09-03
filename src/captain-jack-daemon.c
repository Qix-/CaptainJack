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

static void on_new_client(pid_t pid) {
	char buffer[1024];
	if (proc_name(pid, &buffer[0], sizeof(buffer)) == -1) {
		syslog(LOG_NOTICE, "could not get name of client with pid %u", pid);
	} else {
		syslog(LOG_NOTICE, "client connected: %s (%u)", &buffer[0], pid);
	}
}

static void on_client_disconnect(pid_t pid) {
	syslog(LOG_NOTICE, "client disconnected: %u", pid);
}

static CaptainJack_Xmitter xmitterClient = {
	&on_ready,
	&on_new_client,
	&on_client_disconnect,
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
