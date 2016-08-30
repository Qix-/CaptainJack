#!/usr/bin/env bash
set -e

cd "$(dirname "${0}")"

DRIVER_NAME=CaptainJack.driver

[ -d "/Library/Audio/Plug-Ins/HAL/${DRIVER_NAME}" ] && echo "removing existing installation" && sudo rm -rvf "/Library/Audio/Plug-Ins/HAL/${DRIVER_NAME}" && sudo pkill coreaudio

echo "installing"
sudo cp -vr "./${DRIVER_NAME}" "/Library/Audio/Plug-Ins/HAL/${DRIVER_NAME}"

echo "chowning"
sudo chown "$(whoami)":admin "/Library/Audio/Plug-Ins/HAL/${DRIVER_NAME}"

echo "killing core audio"
sudo pkill coreaudio || echo "coreaudio wasn't running"

echo "installed successfully"
