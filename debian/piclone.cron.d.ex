#
# Regular cron jobs for the piclone package
#
0 4	* * *	root	[ -x /usr/bin/piclone_maintenance ] && /usr/bin/piclone_maintenance
