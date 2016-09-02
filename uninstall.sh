#!/usr/bin/env bash

set -e

echo "killing any launchd daemons"
sudo launchctl stop me.junon.CaptainJack || echo 'no daemons to stop'
sudo launchctl remove me.junon.CaptainJack || echo 'no launch daemons to remove'

echo "removing files"
sudo rm -vrf /opt/captain-jack
sudo rm -vrf /Library/LaunchDaemons/me.junon.CaptainJack.plist
sudo rm -vrf /Library/Audio/Plug-Ins/HAL/CaptainJack.driver

echo "restarting Core Audio"
sudo pkill coreaudiod

echo "uninstalled"
