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

#include <stdbool.h>
#include <sys/syslog.h>
#include <unistd.h>

#include "xmit.h"

static void on_new_client(pid_t pid) {
	syslog(LOG_NOTICE, "hey look at that, a client: %d", pid);
}

static CaptainJack_Xmitter xmitterClient = {
	&on_new_client
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