#!/usr/bin/env bash

./build_linux_server_release.sh
cp flight.service /etc/systemd/system/
systemctl enable flight
systemctl start flight
systemctl restart flight
