#!/bin/sh
# Development helper: a GroupWise account for Evolution without the account
# editor (that comes with the UI module). Not installed.
#
#   tools/dev-account.sh add HOST USER EMAIL "FULL NAME" [read-only]
#   tools/dev-account.sh remove USER
#   tools/dev-account.sh remove-all
#
# "add" makes the evolution-data-server services load the modules from the
# development prefix (EDS_EXTRA_PREFIXES in the systemd user environment),
# writes the account sources and restarts the source registry. Start
# Evolution afterwards with:
#   EDS_EXTRA_PREFIXES=$HOME/.local/opt/evolution-groupwise evolution
# "remove" deletes the account of USER, "remove-all" every account made by
# this script and the environment setting.
#
# Copyright (C) 2026 bond Software Entwicklung GmbH
# SPDX-License-Identifier: LGPL-2.1-or-later

set -eu

PREFIX="${GW_DEV_PREFIX:-$HOME/.local/opt/evolution-groupwise}"
SOURCES="$HOME/.config/evolution/sources"

set_uids () {
	COLLECTION="groupwise-dev-$1-collection"
	ACCOUNT="groupwise-dev-$1-account"
	IDENTITY="groupwise-dev-$1-identity"
	TRANSPORT="groupwise-dev-$1-transport"
}

restart_registry () {
	systemctl --user restart evolution-source-registry.service
	# The factories cache the source list; they restart on demand
	systemctl --user stop evolution-calendar-factory.service evolution-addressbook-factory.service 2>/dev/null || true
}

case "${1:-}" in
add)
	[ $# -eq 5 ] || { [ $# -eq 6 ] && [ "$6" = read-only ]; } ||
		{ echo "usage: $0 add HOST USER EMAIL \"FULL NAME\" [read-only]" >&2; exit 2; }
	host=$2 user=$3 email=$4 name=$5
	read_only=false
	[ $# -eq 6 ] && read_only=true
	set_uids "$user"
	ls "$PREFIX"/lib*/evolution-data-server/camel-providers/libcamelgroupwise.so >/dev/null 2>&1 ||
		{ echo "not installed in $PREFIX: run cmake --install build first" >&2; exit 1; }
	mkdir -p "$SOURCES"

	# The collection: one login for the address books (and later the calendars)
	cat > "$SOURCES/$COLLECTION.source" <<EOF
[Data Source]
DisplayName=GroupWise ($user)
Enabled=true
Parent=

[Collection]
BackendName=groupwise
Identity=$user
MailEnabled=false
ContactsEnabled=true
CalendarEnabled=true

[Groupwise Backend]
Host=$host
Port=7191
User=$user
SecurityMethod=none

[Authentication]
Host=$host
Port=7191
User=$user
Method=
RememberPassword=true

[WebDAV Backend]
EOF

	cat > "$SOURCES/$ACCOUNT.source" <<EOF
[Data Source]
DisplayName=GroupWise ($user)
Enabled=true
Parent=

[Mail Account]
BackendName=groupwise
IdentityUid=$IDENTITY

[Groupwise Backend]
Host=$host
Port=7191
User=$user
SecurityMethod=none
StaySynchronized=false
ReadOnly=$read_only

[Authentication]
Host=$host
Port=7191
User=$user
Method=
RememberPassword=true
ProxyUid=system-proxy

[Security]
Method=none

[Offline]
StaySynchronized=false

[Refresh]
Enabled=true
IntervalMinutes=1
EOF

	cat > "$SOURCES/$IDENTITY.source" <<EOF
[Data Source]
DisplayName=$email
Enabled=true
Parent=$ACCOUNT

[Mail Identity]
Address=$email
Name=$name

[Mail Submission]
TransportUid=$TRANSPORT
UseSentFolder=false

[Mail Composition]
DraftsFolder=folder://$ACCOUNT/Work%20In%20Progress
EOF

	cat > "$SOURCES/$TRANSPORT.source" <<EOF
[Data Source]
DisplayName=$email
Enabled=true
Parent=$ACCOUNT

[Mail Transport]
BackendName=groupwise

[Groupwise Backend]
Host=$host
Port=7191
User=$user
SecurityMethod=none

[Authentication]
Host=$host
Port=7191
User=$user
Method=

[Security]
Method=none
EOF

	systemctl --user set-environment EDS_EXTRA_PREFIXES="$PREFIX"
	restart_registry
	echo "Account added. Start Evolution with:"
	echo "  EDS_EXTRA_PREFIXES=$PREFIX evolution"
	;;
remove)
	[ $# -eq 2 ] || { echo "usage: $0 remove USER" >&2; exit 2; }
	set_uids "$2"
	# the address books the registry made below the collection go along
	rm -rf "$HOME/.cache/evolution/sources/$COLLECTION"
	rm -f "$SOURCES/$COLLECTION.source" "$SOURCES/$ACCOUNT.source" "$SOURCES/$IDENTITY.source" "$SOURCES/$TRANSPORT.source"
	restart_registry
	echo "Account removed; Evolution's mail data of it stays in ~/.local/share/evolution/mail/$ACCOUNT"
	echo "and ~/.cache/evolution/mail/$ACCOUNT until deleted by hand."
	;;
remove-all)
	rm -rf "$HOME"/.cache/evolution/sources/groupwise-dev-*-collection
	rm -f "$SOURCES"/groupwise-dev-*.source
	systemctl --user unset-environment EDS_EXTRA_PREFIXES
	restart_registry
	echo "All development accounts removed, EDS_EXTRA_PREFIXES unset."
	;;
*)
	echo "usage: $0 add HOST USER EMAIL \"FULL NAME\" | remove USER | remove-all" >&2
	exit 2
	;;
esac
