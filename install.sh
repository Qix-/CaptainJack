#!/usr/bin/env bash
set -e

cd "$(dirname "${0}")"

DRIVER_NAME=CaptainJack.driver

function kill_audio {
	sudo launchctl kill SIGTERM system/com.apple.audio.coreaudiod || sudo killall coreaudiod
	return $?
}

[ -d "/Library/Audio/Plug-Ins/HAL/${DRIVER_NAME}" ] \
	&& (echo "removing existing installation") \
	&& sudo rm -rvf "/Library/Audio/Plug-Ins/HAL/${DRIVER_NAME}" \
	&& (kill_audio || echo "coreaudiod wasn't running (you're going too fast, Marty!)")

echo "installing"
sudo cp -vr "./${DRIVER_NAME}" "/Library/Audio/Plug-Ins/HAL/${DRIVER_NAME}"

echo "chowning"
sudo chown -R root:wheel "/Library/Audio/Plug-Ins/HAL/${DRIVER_NAME}"

echo "killing core audio"
kill_audio || echo "coreaudio wasn't running"

echo "stopping any Captain Jack daemons"
sudo launchctl stop me.junon.CaptainJack || echo "no daemons were running"

echo "removing old version of launch daemon"
sudo launchctl bootout system /Library/LaunchDaemons/me.junon.CaptainJack.plist || echo "there was no previous launch daemon bootstrapped"

echo "installing captain jack daemon"
sudo mkdir -p /opt/captain-jack
sudo cp build/src/captain-jack-daemon /opt/captain-jack/daemon

echo "chowning daemon binary"
sudo chown -R root:wheel /opt/captain-jack/daemon

echo "installing launch daemon"
sudo cp ./me.junon.CaptainJack.plist /Library/LaunchDaemons/me.junon.CaptainJack.plist
sudo chown -R root:wheel /Library/LaunchDaemons/me.junon.CaptainJack.plist
sudo chmod 0600 /Library/LaunchDaemons/me.junon.CaptainJack.plist
sudo launchctl bootstrap system /Library/LaunchDaemons/me.junon.CaptainJack.plist

echo "installed successfully"
