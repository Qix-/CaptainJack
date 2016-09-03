#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <sys/syslog.h>

int main(void) {
	openlog("CaptainJack", LOG_NDELAY | LOG_PERROR | LOG_PID, LOG_DAEMON);
	setlogmask(0);
	syslog(LOG_NOTICE, "Captain Jack is portside at ye embarcadero");
	return EXIT_FAILURE;
}
