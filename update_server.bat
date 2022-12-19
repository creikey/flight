@echo off
ssh astris "cd flight || exit 1 ; git pull || exit 1 ; ./linux_server_install.sh || exit 1 ; echo Successfully updated" || exit /b %errorlevel%