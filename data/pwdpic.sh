#!/bin/bash
export TEXTDOMAIN=piclone

. gettext.sh

zenity --password --title "$(gettext "Password Required")"

