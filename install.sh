#!/usr/bin/env bash
set -e

DRIVER_NAME=NullAudio.driver

[ -d "/Library/Audio/Plug-Ins/HAL/${DRIVER_NAME}" ] && echo "removing existing installation" && sudo rm -rvf "/Library/Audio/Plug-Ins/HAL/${DRIVER_NAME}"

echo "installing"
sudo cp -vr "./${DRIVER_NAME}" "/Library/Audio/Plug-Ins/HAL/${DRIVER_NAME}"

echo "chowning"
sudo chown "$(whoami)":admin "/Library/Audio/Plug-Ins/HAL/${DRIVER_NAME}"

echo "killing core audio"
sudo pkill coreaudio || echo "coreaudio wasn't running"

echo "installed successfully"
