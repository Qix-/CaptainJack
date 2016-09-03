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
#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/types.h>
#include <unistd.h>

#include "xmit.h"

static int              gSocket                  = -1;
static int              gPeerSocket              = -1;
static const uint16_t   gBindPort                = 24638;

typedef enum {
	XMPC_NEW_CLIENT,
} Proto_MessageId;

typedef struct {
	Proto_MessageId                          proto_id;
	pid_t                                    pid;
} Proto_NewClient;

static void InitializeBindAddr(struct sockaddr_in *addr) {
	memset(addr, 0, sizeof(*addr));
	addr->sin_family = PF_INET;
	addr->sin_addr.s_addr = INADDR_ANY;
	addr->sin_port = htons(gBindPort);
}

static bool AssertAccepted(void) {
	if (gSocket < 0) {
		syslog(LOG_NOTICE, "AssertConnected: noticed the socket was down; will attempt to bring it online");

		gSocket = socket(PF_INET, SOCK_STREAM, 0);
		if (gSocket < 0) {
			syslog(LOG_ERR, "AssertConnected: could not create a new socket: %s", strerror(errno));
			gSocket = -1;
			return false;
		}

		struct sockaddr_in addr;
		InitializeBindAddr(&addr);
		if (bind(gSocket, (const struct sockaddr *) &addr, sizeof(addr)) != 0) {
			syslog(LOG_ERR, "AssertConnected: could not bind to 0.0.0.0:%d: %s", gBindPort, strerror(errno));
			gSocket = -1;
			return false;
		}

		if (listen(gSocket, 2) != 0) {
			syslog(LOG_ERR, "AssertConnected: could not listen on 0.0.0.0:%d: %s", gBindPort, strerror(errno));
			gSocket = -1;
			return false;
		}

		bool value = true;
		setsockopt(gSocket, SOL_SOCKET, SO_KEEPALIVE, &value, sizeof(value));
		setsockopt(gSocket, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
		fcntl(gSocket, F_SETFL, O_NONBLOCK);

		gPeerSocket = -1;
	}

	if (gPeerSocket < 0) {
		syslog(LOG_NOTICE, "AssertConnected: attempting to accept");

		struct sockaddr_in peerAddr;
		socklen_t peerAddrLen = 0;
		gPeerSocket = accept(gSocket, (struct sockaddr *) &peerAddr, &peerAddrLen);
		if (gPeerSocket == EAGAIN || gPeerSocket == EWOULDBLOCK) {
			gPeerSocket = -1;
			return false;
		}

		syslog(LOG_NOTICE, "AssertConnected: client connected from %s", inet_ntoa(peerAddr.sin_addr));
	}

	return true;
}

static void SendMessage(void *message, size_t length) {
	if (!AssertAccepted()) {
		return;
	}

	size_t offset = 0;
	while (offset < length) {
		int sent = send(gPeerSocket, &message[offset], length - offset, 0);
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
