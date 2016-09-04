.PHONY: default
default: all

DRIVER_NAME = CaptainJack.driver

CC         := $(CC)
CFLAGS      = -std=c99 -g3 -Wall -Wextra -Werror -Wno-unused-parameter -mmacosx-version-min=10.9
CPPFLAGS    =
LDFLAGS     =

CFLAGS_CJ  = -bundle -framework CoreAudio -framework CoreFoundation
CFLAGS_CJD = -framework CoreFoundation
DESTDIR     =
PREFIX      = /usr/local
PLUGINDIR   = /Library/Audio/Plug-Ins/HAL
BUILDDIR    = build

SRCS        = $(wildcard **/*.c)
DEPS        = $(patsubst src/%,$(BUILDDIR)/%,$(addsuffix .d,$(SRCS)))

-include $(BUILDDIR)/Makefile.dep

# Dependencies

$(BUILDDIR)/Makefile.dep: $(DEPS)
	@mkdir -p $(dir $(@))
	@cat $^ > $@

$(BUILDDIR)/%.d: src/%
	@mkdir -p $(dir $(@))
	@$(CC) $(CPPFLAGS) -MG -MM $^ -MF $@ -MQ $(subst .c.d,.o,$@)

$(BUILDDIR)/%.o: src/%.c
	@mkdir -p $(dir $(@))
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Targets

$(BUILDDIR)/captain-jack-daemon: $(BUILDDIR)/captain-jack-daemon.o $(BUILDDIR)/xmit.o
	$(CC) $(LDFLAGS) $(CFLAGS_CJD) $^ -o $@

$(BUILDDIR)/captain-jack: $(BUILDDIR)/captain-jack-device.o $(BUILDDIR)/xmit.o
	$(CC) $(LDFLAGS) $(CFLAGS_CJ) $^ -o $@

.PHONY: all
all: $(BUILDDIR)/captain-jack-daemon $(BUILDDIR)/captain-jack

.PHONY: clean
clean:
	rm -rf $(BUILDDIR)

# Install
.PHONY: stage
stage: $(BUILDDIR)/captain-jack
	install -d -m 0755 $(DRIVER_NAME)/Contents/MacOS/
	install -m 0755 $(BUILDDIR)/captain-jack $(DRIVER_NAME)/Contents/MacOS/CaptainJack

.PHONY: install
install: $(BUILDDIR)/captain-jack-daemon stage
	sudo install -d -m 0755 -o root -g wheel $(DESTDIR)$(PREFIX)/sbin/
	sudo install -d -m 0755 -o root -g wheel $(DESTDIR)$(PLUGINDIR)/$(DRIVER_NAME)/Contents/
	sudo install -d -m 0755 -o root -g wheel $(DESTDIR)$(PLUGINDIR)/$(DRIVER_NAME)/Contents/MacOS/
	sudo install -d -m 0755 -o root -g wheel $(DESTDIR)$(PLUGINDIR)/$(DRIVER_NAME)/Contents/Resources/
	sudo install -d -m 0755 -o root -g wheel $(DESTDIR)$(PLUGINDIR)/$(DRIVER_NAME)/Contents/Resources/English.lproj/
	sudo install -d -m 0755 -o root -g wheel $(DESTDIR)/Library/LaunchDaemons/
	sudo install -m 0755 -o root -g wheel $(BUILDDIR)/captain-jack-daemon $(DESTDIR)$(PREFIX)/sbin/captain-jack-daemon
	sudo install -m 0644 -o root -g wheel $(DRIVER_NAME)/Contents/Info.plist $(DESTDIR)$(PLUGINDIR)/$(DRIVER_NAME)/Contents/Info.plist
	sudo install -m 0755 -o root -g wheel $(DRIVER_NAME)/Contents/MacOS/CaptainJack $(DESTDIR)$(PLUGINDIR)/$(DRIVER_NAME)/Contents/MacOS/CaptainJack
	sudo install -m 0644 -o root -g wheel $(DRIVER_NAME)/Contents/Resources/DeviceIcon.icns $(DESTDIR)$(PLUGINDIR)/$(DRIVER_NAME)/Contents/Resources/DeviceIcon.icns
	sudo install -m 0644 -o root -g wheel $(DRIVER_NAME)/Contents/Resources/English.lproj/Localizable.strings $(DESTDIR)$(PLUGINDIR)/$(DRIVER_NAME)/Contents/Resources/English.lproj/Localizable.strings
	sudo install -m 0600 -o root -g wheel me.junon.CaptainJack.plist $(DESTDIR)/Library/LaunchDaemons/me.junon.CaptainJack.plist

.PHONY: install
uninstall: stop
	sudo rm -vf $(DESTDIR)$(PREFIX)/sbin/captain-jack-daemon
	sudo rm -vf $(DESTDIR)/Library/LaunchDaemons/me.junon.CaptainJack.plist
	sudo rm -vrf $(DESTDIR)$(PLUGINDIR)/CaptainJack.driver

.PHONY: kill_audio
kill_audio:
	ps aux | grep _coreaudiod | grep -v grep | awk '{print $$2}' | xargs sudo kill -9

.PHONY: stop
stop: kill_audio
	@echo "stopping any Captain Jack daemons"
	sudo launchctl stop me.junon.CaptainJack || echo "no daemons were running"
	@echo "removing old version of launch daemon"
	sudo launchctl bootout system /Library/LaunchDaemons/me.junon.CaptainJack.plist || echo "there was no previous launch daemon bootstrapped"
	$(MAKE) kill_audio

.PHONY: start
start: kill_audio
	sudo launchctl bootstrap system /Library/LaunchDaemons/me.junon.CaptainJack.plist
	$(MAKE) kill_audio

.PHONY: restart
restart: stop start

