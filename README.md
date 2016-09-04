```
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
```

---

# Captain Jack

Captain Jack is a [JACK](http://jackaudio.org)-enabled audio device for OS/X.

Through the use of the new AudioServerPlugin API (since 10.5), Captain Jack
provides a means of routing system audio into the JACK subsystem through
either a system mix port or one or more per-application (namely, per-process)
audio ports.

## Installing

You must have an implmementation of JACK installed on the system. You can
(should) do this by downloading and building the latest source from either
[JACK1](https://github.com/jackaudio/jack1.git) or
[JACK2](https://github.com/jackaudio/jack2.git), though it's worth mentioning
JACK2 does not currently build on OS/X.

To build Captain Jack:

```console
$ make
```

To install Captain Jack:

```console
$ sudo make install
```

To start Captain Jack:

```console
$ sudo make start
```

## Developing
Captain Jack is made up of two piece: the **device** and the **daemon**.

### Device
The Captain Jack Device is a CoreAudio HAL Plugin `.driver` bundle that uses
the latest APIs in lieu of the deprecated AudioHardwarePlugin APIs.

The device is a user-space plugin that requires no Kext signing nor does it
require disabling of SIP.

Since `coreaudiod`, the system service that manages audio and thus loads in
Captain Jack's device plugin, is in the
[System bootstrap](https://developer.apple.com/library/mac/technotes/tn2083/_index.html#//apple_ref/doc/uid/DTS10003794-CH1-SUBSECTION10)
space, and since `coreaudiod` runs everything inside of a sandbox, getting
audio *out* of `coreaudiod` and *into* JACK requires a little clever IPC.

XPC is Apple's version of IPC and is one of the two methods of IPC really
available to Mach services. Its major (and deal-breaking) drawback is that
it requires some sort of main dispatch loop, which isn't possible in an
AudioServerPlugin bundle since everything inside of the device is just a
callback and anything that blocks would effectively block not only `coreaudiod`
but other launch daemons as well (I notice my RGB key board freeze up if
Captain Jack blocks!).

Therefore, the network was the only other means of IPC (I heard that shared
memory regions were allowed, but I never could figure out how to get them
to work).

When a connection has been established to the daemon (a user-space `launchd`
daemon; see below), all callbacks are supplemented to an Xmit RPC call (see
`src/xmit.c`) that in turn passes the data along the loopback socket and into
the less-restrictive daemon process.

> The reason why the daemon is even necessary and why the device cannot simply
> connect to JACK directly is that the sandbox prevents any IPC other than
> network IPC. Since we can't really guarantee that JACK is going to support
> network communication this becomes unreliable. As well, things like RO
> filesystem calls, the inability to trigger window apps or even look up PID
> process names were very restrictive, and the device quickly turned into
> a trampoline for audio events.

### Daemon
The Captain Jack Daemon is a user-space `launchd` LaunchDaemon that relays
Xmit-RPC'd audio data from the device to JACK. It also manages a light state
for whatever might be necessary to track (e.g. client name => PID map, etc).

### Xmit
The device and daemon communicate over a very opaque and light network layer
dubbed Xmit (see `src/xmit.c`) that uses read buffer polling to achieve
asynchronicity. Since it uses the TCP protocol, there is little chance of
audio frames to be mixxed up during streaming.

Using the network also gives us nearly free IPC with almost zero added latency
(assuming the loopback interface is used, which it is in this case).

The only externalized Xmit calls are those that set up the callback functions.
All transportation specifics are statically defined and managed inside of
`xmit.c`.

## License
Captain Jack is licensed under the [MIT License](LICENSE).
All JACK works are licensed under their respective licenses.
