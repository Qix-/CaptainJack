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

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/types.h>
#include <unistd.h>

#include "xmit.h"

typedef enum {
	XMPC_NONE = 0,
	XMPC_NEW_CLIENT,
} Proto_MessageId;

typedef struct {
	Proto_MessageId                          proto_id;
	pid_t                                    pid;
} Proto_NewClient;

static int                  gSocket              = -1;
static int                  gPeerSocket          = -1;
static const uint16_t       gBindPort            = 24638;
static CaptainJack_Xmitter *gXmitterClient       = NULL;
static Proto_MessageId      gTickHeader          = XMPC_NONE;

static void InitializeBindAddr(struct sockaddr_in *addr) {
	memset(addr, 0, sizeof(*addr));
	addr->sin_family = PF_INET;
	addr->sin_addr.s_addr = INADDR_ANY;
	addr->sin_port = htons(gBindPort);
}

static bool AssertAccepted(void) {
	if (gSocket < 0) {
		syslog(LOG_NOTICE, "AssertAccepted: noticed the socket was down; will attempt to bring it online");

		gSocket = socket(PF_INET, SOCK_STREAM, 0);
		if (gSocket < 0) {
			syslog(LOG_ERR, "AssertAccepted: could not create a new socket: %s", strerror(errno));
			gSocket = -1;
			return false;
		}

		struct sockaddr_in addr;
		InitializeBindAddr(&addr);
		if (bind(gSocket, (const struct sockaddr *) &addr, sizeof(addr)) != 0) {
			syslog(LOG_ERR, "AssertAccepted: could not bind to 0.0.0.0:%d: %s", gBindPort, strerror(errno));
			gSocket = -1;
			return false;
		}

		if (listen(gSocket, 2) != 0) {
			syslog(LOG_ERR, "AssertAccepted: could not listen on 0.0.0.0:%d: %s", gBindPort, strerror(errno));
			gSocket = -1;
			return false;
		}

		bool value = true;
		setsockopt(gSocket, SOL_SOCKET, SO_KEEPALIVE, &value, sizeof(value));
		setsockopt(gSocket, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));

		gPeerSocket = -1;
	}

	if (gPeerSocket < 0) {
		syslog(LOG_NOTICE, "AssertAccepted: attempting to accept");

		struct sockaddr_in peerAddr;
		socklen_t peerAddrLen = 0;
		if ((gPeerSocket = accept(gSocket, (struct sockaddr *) &peerAddr, &peerAddrLen)) == -1) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				syslog(LOG_ERR, "AssertAccepted: error when accepting: %s", strerror(errno));
			}

			gPeerSocket = -1;
			return false;
		}

		syslog(LOG_NOTICE, "AssertAccepted: client connected from %s on %d", inet_ntoa(peerAddr.sin_addr), (int) gPeerSocket);
	}

	return true;
}

static int AssertConnected(void) {
	if (gSocket < 0) {
		// note, we take a different approach here; we're a launch daemon in this call,
		// so if we can't connect we're going to crash and burn by returning false here.
		// we'll gracefully shut down any JACKd registries and tell launchd that we're basically
		// useless, and let it reschedule us as necessary.
		syslog(LOG_NOTICE, "AssertConnected: noticed I wasn't connected anymore; I'll try to connect now.");

		gSocket = socket(PF_INET, SOCK_STREAM, 0);
		if (gSocket < 0) {
			syslog(LOG_ERR, "AssertConnected: could not create a new socket: %s", strerror(errno));
			gSocket = -1;
			return false;
		}

		struct sockaddr_in addr;
		InitializeBindAddr(&addr);
		if (connect(gSocket, (const struct sockaddr *) &addr, sizeof(addr)) != 0) {
			syslog(LOG_ERR, "AssertConnected: connect failed: %s", strerror(errno));
			gSocket = -1;
			return false;
		}

		bool value = true;
		setsockopt(gSocket, SOL_SOCKET, SO_KEEPALIVE, &value, sizeof(value));
		fcntl(gSocket, F_SETFL, O_NONBLOCK);

		gTickHeader = true;

		syslog(LOG_NOTICE, "AssertConnected: connected to device. Yargh!");
	}

	return true;
}

static void SendMessage(void *message, size_t length) {
	if (!AssertAccepted()) {
		return;
	}

	size_t offset = 0;
	while (offset < length) {
		ssize_t sent = send(gPeerSocket, &message[offset], length - offset, 0);
		if (sent == -1) {
			syslog(LOG_NOTICE, "SendMessage: could not transmit message (%d): %s", *(Proto_MessageId*)message, strerror(errno));
			close(gPeerSocket);
			gPeerSocket = -1;
			return;
		}

		if (sent == 0) {
			// XXX DEBUG
			syslog(LOG_NOTICE, "SendMessage: sent message %d of %zu size", *(Proto_MessageId*)message, length);
			return;
		}

		offset += sent;
	}
}

static void Send_NewClient(pid_t pid) {
	Proto_NewClient msg = {
		XMPC_NEW_CLIENT,
		pid
	};

	SendMessage(&msg, sizeof(msg));
}

static CaptainJack_Xmitter gXmitterServer = {
	&Send_NewClient
};

CaptainJack_Xmitter * CaptainJack_GetXmitterServer(void) {
	return &gXmitterServer;
}

void CaptainJack_RegisterXmitterClient(CaptainJack_Xmitter *xmitter) {
	if (gXmitterClient != NULL) {
		syslog(LOG_NOTICE, "CaptainJack:RegisterXmitterClient: warning, you're overwriting a previously specified xmitter client");
	}

	gXmitterClient = xmitter;
}

/*
	so admittedly the xmitter tick function is a little complex.
	the goal is to be able to run this intermittently, and interleave
	calls to this function with anything JACK requires, so as to keep
	latency down if there's a lag in the driver.

	that's why we support a two-state read mechanism with the ability to
	look at the number of avialble bytes. Since our data isn't going to be
	huge, we can pretty much guarantee this will succeed unless you're
	on a super strange version of OS/X.

	this might need to be changed a bit in the future to have a running
	buffer to switch between, but right now I'm lazy and this will get
	us there.
*/

size_t GetBytesAvailable(void) {
	size_t available = 0;
	ioctl(gSocket, FIONREAD, &available);
	return available;
}

bool CaptainJack_TickXmitter(void) {
	if (gXmitterClient == NULL) {
		syslog(LOG_ERR, "CaptainJack_TickXmitter: cannot tick; you haven't specified a client yet");
		return false;
	}

	if (!AssertConnected()) {
		return false;
	}

	size_t available = GetBytesAvailable();

	if (gTickHeader == XMPC_NONE) {
		if (available < sizeof(gTickHeader)) {
			return true;
		}

		if (read(gSocket, &gTickHeader, sizeof(gTickHeader)) == -1) {
			syslog(LOG_ERR, "CaptainJack_TickXmitter: problem when reading message header: %s", strerror(errno));
			return false;
		}

		available -= sizeof(gTickHeader);
	}

	if (available <= 0) { // hopefully it's never negative, but eh...
		return true;
	}

	bool result = true;
	switch (gTickHeader) {
	default:
		syslog(LOG_NOTICE, "CaptainJack_TickXmitter: encountered unknown xmit message header: %d", gTickHeader);
		result = false;
	}

	return result;
}
