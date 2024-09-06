#!/bin/sh

# Copyright 2011 Dvir Volk <dvirsk at gmail dot com>. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
#   1. Redistributions of source code must retain the above copyright notice,
#   this list of conditions and the following disclaimer.
#
#   2. Redistributions in binary form must reproduce the above copyright
#   notice, this list of conditions and the following disclaimer in the
#   documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
# WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
# EVENT SHALL Dvir Volk OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
# INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
# OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
# LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
# NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
# EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
################################################################################
#
# Service installer for redqueue server, runs interactively by default.
#
# To run this script non-interactively (for automation/provisioning purposes),
# feed the variables into the script. Any missing variables will be prompted!
# Tip: Environment variables also support command substitution (see REDQUEUE_EXECUTABLE)
#
# Example:
#
# sudo REDQUEUE_PORT=1234 \
# 		 REDQUEUE_CONFIG_FILE=/etc/redqueue/1234.conf \
# 		 REDQUEUE_LOG_FILE=/var/log/redqueue_1234.log \
# 		 REDQUEUE_DATA_DIR=/var/lib/redqueue/1234 \
# 		 REDQUEUE_EXECUTABLE=`command -v redqueue-server` ./utils/install_server.sh
#
# This generates a redqueue config file and an /etc/init.d script, and installs them.
#
# /!\ This script should be run as root
#
# NOTE: This script will not work on Mac OSX.
#       It supports Debian and Ubuntu Linux.
#
################################################################################

die () {
	echo "ERROR: $1. Aborting!"
	exit 1
}


#Absolute path to this script
SCRIPT=$(readlink -f $0)
#Absolute path this script is in
SCRIPTPATH=$(dirname $SCRIPT)

#Initial defaults
_REDQUEUE_PORT=6379
_MANUAL_EXECUTION=false

echo "Welcome to the RedQueue service installer"
echo "This script will help you easily set up a running RedQueue server"
echo

#check for root user
if [ "$(id -u)" -ne 0 ] ; then
	echo "You must run this script as root. Sorry!"
	exit 1
fi

#bail if this system is managed by systemd
_pid_1_exe="$(readlink -f /proc/1/exe)"
if [ "${_pid_1_exe##*/}" = systemd ]
then
	echo "This systems seems to use systemd."
	echo "Please take a look at the provided example service unit files in this directory, and adapt and install them. Sorry!"
	exit 1
fi
unset _pid_1_exe

if ! echo $REDQUEUE_PORT | egrep -q '^[0-9]+$' ; then
	_MANUAL_EXECUTION=true
	#Read the RedQueue port
	read  -p "Please select the RedQueue port for this instance: [$_REDQUEUE_PORT] " REDQUEUE_PORT
	if ! echo $REDQUEUE_PORT | egrep -q '^[0-9]+$' ; then
		echo "Selecting default: $_REDQUEUE_PORT"
		REDQUEUE_PORT=$_REDQUEUE_PORT
	fi
fi

if [ -z "$REDQUEUE_CONFIG_FILE" ] ; then
	_MANUAL_EXECUTION=true
	#read the RedQueue config file
	_REDQUEUE_CONFIG_FILE="/etc/redqueue/$REDQUEUE_PORT.conf"
	read -p "Please select the RedQueue config file name [$_REDQUEUE_CONFIG_FILE] " REDQUEUE_CONFIG_FILE
	if [ -z "$REDQUEUE_CONFIG_FILE" ] ; then
		REDQUEUE_CONFIG_FILE=$_REDQUEUE_CONFIG_FILE
		echo "Selected default - $REDQUEUE_CONFIG_FILE"
	fi
fi

if [ -z "$REDQUEUE_LOG_FILE" ] ; then
	_MANUAL_EXECUTION=true
	#read the RedQueue log file path
	_REDQUEUE_LOG_FILE="/var/log/redqueue_$redqueue_port.log"
	read -p "Please select the RedQueue log file name [$_REDQUEUE_LOG_FILE] " REDQUEUE_LOG_FILE
	if [ -z "$REDIS_LOG_FILE" ] ; then
		REDQUEUE_LOG_FILE=$_REDQUEUE_LOG_FILE
		echo "Selected default - $REDQUEUE_LOG_FILE"
	fi
fi

if [ -z "$REDQUEUE_DATA_DIR" ] ; then
	_MANUAL_EXECUTION=true
	#get the RedQueue data directory
	_REDQUEUE_DATA_DIR="/var/lib/redqueue/$REDQUEUE_PORT"
	read -p "Please select the data directory for this instance [$_REDQUEUE_DATA_DIR] " REDQUEUE_DATA_DIR
	if [ -z "$REDQUEUE_DATA_DIR" ] ; then
		REDQUEUE_DATA_DIR=$_REDQUEUE_DATA_DIR
		echo "Selected default - $REDQUEUE_DATA_DIR"
	fi
fi

if [ ! -x "$REDQUEUE_EXECUTABLE" ] ; then
	_MANUAL_EXECUTION=true
	#get the RedQueue executable path
	_REDQUEUE_EXECUTABLE=`command -v redqueue-server`
	read -p "Please select the RedQueue executable path [$_REDQUEUE_EXECUTABLE] " REDQUEUE_EXECUTABLE
	if [ ! -x "$REDQUEUE_EXECUTABLE" ] ; then
		REDQUEUE_EXECUTABLE=$_REDQUEUE_EXECUTABLE

		if [ ! -x "$REDQUEUE_EXECUTABLE" ] ; then
			echo "Mmmmm...  it seems like you don't have a RedQueue executable. Did you run make install yet?"
			exit 1
		fi
	fi
fi

#check the default for RedQueue cli
CLI_EXEC=`command -v redqueue-cli`
if [ -z "$CLI_EXEC" ] ; then
	CLI_EXEC=`dirname $REDQUEUE_EXECUTABLE`"/redqueue-cli"
fi

echo "Selected config:"

echo "Port           : $REDQUEUE_PORT"
echo "Config file    : $REDQUEUE_CONFIG_FILE"
echo "Log file       : $REDQUEUE_LOG_FILE"
echo "Data dir       : $REDQUEUE_DATA_DIR"
echo "Executable     : $REDQUEUE_EXECUTABLE"
echo "Cli Executable : $CLI_EXEC"

if $_MANUAL_EXECUTION == true ; then
	read -p "Is this ok? Then press ENTER to go on or Ctrl-C to abort." _UNUSED_
fi

mkdir -p `dirname "$REDQUEUE_CONFIG_FILE"` || die "Could not create RedQueue config directory"
mkdir -p `dirname "$REDQUEUE_LOG_FILE"` || die "Could not create RedQueue log dir"
mkdir -p "$REDQUEUE_DATA_DIR" || die "Could not create RedQueue data directory"

#render the templates
TMP_FILE="/tmp/${REDQUEUE_PORT}.conf"
DEFAULT_CONFIG="${SCRIPTPATH}/../redqueue.conf"
INIT_TPL_FILE="${SCRIPTPATH}/redqueue_init_script.tpl"
INIT_SCRIPT_DEST="/etc/init.d/redqueue_${REDQUEUEPORT}"
PIDFILE="/var/run/redqueue_${REDQUEUE_PORT}.pid"

if [ ! -f "$DEFAULT_CONFIG" ]; then
	echo "Mmmmm... the default config is missing. Did you switch to the utils directory?"
	exit 1
fi

#Generate config file from the default config file as template
#changing only the stuff we're controlling from this script
echo "## Generated by install_server.sh ##" > $TMP_FILE

read -r SED_EXPR <<-EOF
s#^port .\+#port ${REDQUEUE_PORT}#; \
s#^logfile .\+#logfile ${REDQUEUE_LOG_FILE}#; \
s#^dir .\+#dir ${REDQUEUE_DATA_DIR}#; \
s#^pidfile .\+#pidfile ${PIDFILE}#; \
s#^daemonize no#daemonize yes#;
EOF
sed "$SED_EXPR" $DEFAULT_CONFIG >> $TMP_FILE

#cat $TPL_FILE | while read line; do eval "echo \"$line\"" >> $TMP_FILE; done
cp $TMP_FILE $REDQUEUE_CONFIG_FILE || die "Could not write RedQueue config file $REDQUEUE_CONFIG_FILE"

#Generate sample script from template file
rm -f $TMP_FILE

#we hard code the configs here to avoid issues with templates containing env vars
#kinda lame but works!
REDQUEUE_INIT_HEADER=\
"#!/bin/sh\n
#Configurations injected by install_server below....\n\n
EXEC=$REDQUEUE_EXECUTABLE\n
CLIEXEC=$CLI_EXEC\n
PIDFILE=\"$PIDFILE\"\n
CONF=\"$REDQUEUE_CONFIG_FILE\"\n\n
REDQUEUEPORT=\"$REDQUEUE_PORT\"\n\n
###############\n\n"

REDQUEUE_CHKCONFIG_INFO=\
"# REDHAT chkconfig header\n\n
# chkconfig: - 58 74\n
# description: redqueue_${REDQUEUE_PORT} is the RedQueue daemon.\n
### BEGIN INIT INFO\n
# Provides: redqueue_6379\n
# Required-Start: \$network \$local_fs \$remote_fs\n
# Required-Stop: \$network \$local_fs \$remote_fs\n
# Default-Start: 2 3 4 5\n
# Default-Stop: 0 1 6\n
# Should-Start: \$syslog \$named\n
# Should-Stop: \$syslog \$named\n
# Short-Description: start and stop redqueue_${REDQUEUE_PORT}\n
# Description: RedQueue daemon\n
### END INIT INFO\n\n"

if command -v chkconfig >/dev/null; then
	#if we're a box with chkconfig on it we want to include info for chkconfig
	echo "$REDQUEUE_INIT_HEADER" "$REDQUEUE_CHKCONFIG_INFO" > $TMP_FILE && cat $INIT_TPL_FILE >> $TMP_FILE || die "Could not write init script to $TMP_FILE"
else
	#combine the header and the template (which is actually a static footer)
	echo "$REDQUEUE_INIT_HEADER" > $TMP_FILE && cat $INIT_TPL_FILE >> $TMP_FILE || die "Could not write init script to $TMP_FILE"
fi

###
# Generate sample script from template file
# - No need to check which system we are on. The init info are comments and
#   do not interfere with update_rc.d systems. Additionally:
#     Ubuntu/debian by default does not come with chkconfig, but does issue a
#     warning if init info is not available.

cat > ${TMP_FILE} <<EOT
#!/bin/sh
#Configurations injected by install_server below....

EXEC=$REDQUEUE_EXECUTABLE
CLIEXEC=$CLI_EXEC
PIDFILE=$PIDFILE
CONF="$REDQUEUE_CONFIG_FILE"
REDQUEUEPORT="$REDQUEUE_PORT"
###############
# SysV Init Information
# chkconfig: - 58 74
# description: redqueue_${REDQUEUE_PORT} is the RedQueue daemon.
### BEGIN INIT INFO
# Provides: redqueue_${REDQUEUE_PORT}
# Required-Start: \$network \$local_fs \$remote_fs
# Required-Stop: \$network \$local_fs \$remote_fs
# Default-Start: 2 3 4 5
# Default-Stop: 0 1 6
# Should-Start: \$syslog \$named
# Should-Stop: \$syslog \$named
# Short-Description: start and stop redqueue_${REDQUEUE_PORT}
# Description: RedQueue daemon
### END INIT INFO

EOT
cat ${INIT_TPL_FILE} >> ${TMP_FILE}

#copy to /etc/init.d
cp $TMP_FILE $INIT_SCRIPT_DEST && \
	chmod +x $INIT_SCRIPT_DEST || die "Could not copy redqueue init script to  $INIT_SCRIPT_DEST"
echo "Copied $TMP_FILE => $INIT_SCRIPT_DEST"

#Install the service
echo "Installing service..."
if command -v chkconfig >/dev/null 2>&1; then
	# we're chkconfig, so lets add to chkconfig and put in runlevel 345
	chkconfig --add redqueue_${REDQUEUE_PORT} && echo "Successfully added to chkconfig!"
	chkconfig --level 345 redqueue_${REDQUEUE_PORT} on && echo "Successfully added to runlevels 345!"
elif command -v update-rc.d >/dev/null 2>&1; then
	#if we're not a chkconfig box assume we're able to use update-rc.d
	update-rc.d redqueue_${REDQUEUE_PORT} defaults && echo "Success!"
else
	echo "No supported init tool found."
fi

/etc/init.d/redqueue_$REDQUEUE_PORT start || die "Failed starting service..."

#tada
echo "Installation successful!"
exit 0
