#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <sys/syslog.h>
#include <xpc/xpc.h>

int main(void) {
	openlog("CaptainJack", LOG_NDELAY | LOG_PERROR | LOG_PID, LOG_DAEMON);
	setlogmask(0);
	syslog(LOG_NOTICE, "Captain Jack is portside at ye embarcadero");

	xpc_connection_t connection = xpc_connection_create_mach_service("me.junon.CaptainJack", NULL, XPC_CONNECTION_MACH_SERVICE_LISTENER);
	xpc_connection_set_event_handler(connection, ^(xpc_object_t peer) {
		syslog(LOG_NOTICE, "new connection");
		xpc_connection_set_event_handler(peer, ^(xpc_object_t event) {
			(void) event;
		});
		xpc_connection_resume(peer);
	});

	xpc_connection_resume(connection);

	syslog(LOG_NOTICE, "waiting for connection");

	dispatch_main();

	return EXIT_FAILURE;
}
