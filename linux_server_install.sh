#!/usr/bin/env bash

systemctl stop flight || exit 1
./build_linux_server_release.sh || exit 1
cp flight.service /etc/systemd/system/ || exit 1
systemctl enable flight || exit 1
systemctl start flight || exit 1
systemctl restart flight || exit 1
