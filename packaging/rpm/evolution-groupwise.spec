#
# spec file for package evolution-groupwise
#
# Copyright (c) 2026 bond Software Entwicklung GmbH
#
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Builds on openSUSE (Tumbleweed, Leap 16) and Fedora: evolution-data-server
# and Evolution 3.52 or later are needed (Leap 15 has 3.42: too old).
#

Name:           evolution-groupwise
Version:        0.9.2
Release:        0
Summary:        GroupWise mailboxes in Evolution
License:        LGPL-2.1-or-later
Group:          Productivity/Networking/Email/Clients
# The repository address is still to be decided
URL:            https://github.com/rabackes/evolution-groupwise
Source0:        %{url}/archive/refs/tags/v%{version}.tar.gz#/%{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.16
BuildRequires:  gcc
BuildRequires:  pkgconfig
BuildRequires:  python3
BuildRequires:  openssl
%if 0%{?suse_version}
BuildRequires:  gettext-tools
%else
BuildRequires:  gettext
%endif
BuildRequires:  pkgconfig(camel-1.2) >= 3.52
BuildRequires:  pkgconfig(libedataserver-1.2) >= 3.52
BuildRequires:  pkgconfig(libebackend-1.2) >= 3.52
BuildRequires:  pkgconfig(libebook-1.2) >= 3.52
BuildRequires:  pkgconfig(libebook-contacts-1.2) >= 3.52
BuildRequires:  pkgconfig(libedata-book-1.2) >= 3.52
BuildRequires:  pkgconfig(libecal-2.0) >= 3.52
BuildRequires:  pkgconfig(libedata-cal-2.0) >= 3.52
BuildRequires:  pkgconfig(evolution-mail-3.0) >= 3.52
BuildRequires:  pkgconfig(evolution-shell-3.0) >= 3.52
BuildRequires:  pkgconfig(evolution-calendar-3.0) >= 3.52
BuildRequires:  pkgconfig(libsoup-3.0)
BuildRequires:  pkgconfig(libxml-2.0)
BuildRequires:  pkgconfig(libical-glib)
BuildRequires:  pkgconfig(gio-unix-2.0)

Requires:       evolution >= 3.52
Requires:       evolution-data-server >= 3.52

%description
Access to GroupWise mailboxes from Evolution through the SOAP interface of
the GroupWise Post Office Agent: mail, address books, calendar, tasks and
notes, with invitations answered and sent by GroupWise itself.

Contains the Camel provider, the address book and calendar modules for
evolution-data-server, the collection module of the source registry, the
Evolution module with the GroupWise settings and a diagnostic command
line tool.

%if 0%{?suse_version}
%lang_package
%endif

%prep
%autosetup

%build
%cmake
%cmake_build

%install
%cmake_install
%find_lang %{name}

%check
# The tests start a mock POA on 127.0.0.1
%ctest

%if 0%{?suse_version}
%files
%else
%files -f %{name}.lang
%endif
%license COPYING
%doc README.md README.de.md
%{_libdir}/evolution-data-server/camel-providers/libcamelgroupwise.so
%{_libdir}/evolution-data-server/camel-providers/libcamelgroupwise.urls
%{_libdir}/evolution-data-server/addressbook-backends/libebookbackendgroupwise.so
%{_libdir}/evolution-data-server/calendar-backends/libecalbackendgroupwise.so
%{_libdir}/evolution-data-server/registry-modules/module-groupwise-backend.so
%{_libdir}/evolution/modules/module-groupwise-configuration.so
%dir %{_libdir}/evolution-groupwise
%{_libdir}/evolution-groupwise/libegroupwise.so
%{_libdir}/evolution-groupwise/libegroupwise-eds.so
%{_libdir}/evolution-groupwise/gw-cli

%if 0%{?suse_version}
%files lang -f %{name}.lang
%endif

%changelog
* Sat Oct 10 2026 Rainer Backes <rbackes@bond.de> - 0.9.2-0
- Calendar: deleting an own meeting "only for me" takes it out of the own
  calendar and leaves it to the attendees; it stays in the Sent Items with the
  answers and can still be retracted. The button of Evolution's question says
  so for GroupWise calendars.
- The dialog to pick recipients and attendees gets the entry "Automatic", which
  searches all address books marked for autocompletion at once.
- New meetings, tasks and memos made outside their view go to the main account
  instead of a proxy account whose calendar happens to be selected; the main
  account's calendar and lists become Evolution's defaults where those were
  still "On This Computer". The organizer shows with the name.
- Phone messages show caller, company, number and what the caller wants above
  the text. New all-day appointments are free, as in the GroupWise client.
- The lookup of a place among the attendees no longer blocks Evolution. Manual:
  the names of the items in GroupWise and Evolution.

* Sat Oct 10 2026 Rainer Backes <rbackes@bond.de> - 0.9.1-0
- Calendars, task and memo lists follow the events of the mailbox: what is
  made, changed, deleted, accepted or completed in another client shows within
  seconds while Evolution runs, with the account option for the events.
- The background tasks of the Evolution module (signatures, options of proxy
  accounts, rules, the question about the port) start with whichever view
  Evolution starts in, not only with the mail view.
- Manual: cloned machines share the machine ID and take each other's events.

* Sat Oct 10 2026 Rainer Backes <rbackes@bond.de> - 0.9.0-0
- Mail: changes of the mailbox from the events of the POA (GroupWise Web
  Services Events). New account option: Evolution asks the server every so many
  seconds what happened in the mailbox; new mail and what other clients do
  (read, delete, move) shows within the interval, also in proxy accounts.
- Second option: the server tells at once, on a port of the computer; changes
  show within a second or two. Evolution notices when the messages do not
  arrive and offers to open the port in the firewall.
- Proxy accounts take both options over from the main account.

* Fri Oct 09 2026 Rainer Backes <rbackes@bond.de> - 0.8.2-0
- Deleting an own meeting asks, as the GroupWise client does, whether it is
  retracted from the attendees' mailboxes too, with a retraction comment.

* Wed Sep 30 2026 Rainer Backes <rbackes@bond.de> - 0.8.1-0
- Rule editor: existing conditions show only the fields of their kind.
- Manual: screenshots of the GroupWise Settings, trusting the POA certificate
  as the normal way, the CA for administrators.

* Wed Sep 30 2026 Rainer Backes <rbackes@bond.de> - 0.8.0-0
- Sent Items: Delivery Status… shows what happened at each recipient
  (delivered, opened, deleted, accepted …); Retract… takes messages back from
  the recipients; Resend… opens a message to send it anew and asks whether the
  original is retracted.
- The follow-up flag of a mail is the GroupWise Tasklist, both ways, with due
  date and completion.
- Tasks: a completed task starts on the day it was given.
- Junk Mail settings: the context menu of a list moves entries onto the other
  lists.

* Wed Sep 30 2026 Rainer Backes <rbackes@bond.de> - 0.7.0-0
- Appointment editor: tab Travel Time with the red car of the GroupWise client
  — preparation and travel time before and after an appointment; set, changed
  and taken away as in the GroupWise client, the user's own (not sent to
  attendees). Moving an appointment takes it along.
- New meetings have the owner of the calendar as an accepted attendee, as in
  the GroupWise client; sent meetings show with their own copy.
- Saving an appointment warns about busy time it overlaps in the own calendars,
  travel times included.
- Meetings the user takes part in show once, not again in proxy calendars; the
  tooltip names the owner of proxy appointments and the attendees' answers.
- Resources are invited like attendees, with their name; a place gives a
  meeting without location its name as location.

* Tue Sep 29 2026 Rainer Backes <rbackes@bond.de> - 0.6.0-0
- GroupWise Settings: tab Rules — the rules of the mailbox with an editor of
  the GroupWise client's scope (events, item types, conditions on the client's
  fields, actions); run a rule now.
- Account options: Evolution runs the Startup/Exit and Open/Close Folder rules,
  which the POA leaves to the client (off by default).

* Tue Sep 29 2026 Rainer Backes <rbackes@bond.de> - 0.5.0-0
- Proxy accounts as accounts of their own, with the other user's calendars
  (unselected at first, writable by the granted rights), task and memo lists
  and personal address books.
- GroupWise Settings: tab Proxy Access (who may open the own mailbox, with the
  rights of the GroupWise client) and tab Signatures.
- Signatures: the GroupWise signatures in Evolution, edits go back to
  GroupWise; the GroupWise default is the account's signature.
- Folder list: system folders first in the GroupWise order; shared folders
  whose parent is gone are hidden.

* Tue Sep 29 2026 Rainer Backes <rbackes@bond.de> - 0.4.0-0
- GroupWise Settings window (account context menu) with the tabs Out of Office,
  Junk Mail and Proxy Access; replaces the Junk Mail page of the account
  editor.
- Junk mail submenu: trust, junk or block the sender or its domain.
- "Add Sender to Address Book" in the message context menu.
- Proxy accounts: other users' mailboxes as accounts of their own (grey),
  reading and sending in their name with the granted rights.
- HTML bodies go out as UTF-8 (umlauts in the GroupWise client).
- System folders under their localized names.

* Mon Sep 28 2026 Rainer Backes <rbackes@bond.de> - 0.3.0-0
- Junk: the Junk Mail folder of GroupWise is Evolution's junk folder; marking
  junk / not junk teaches GroupWise's junk and trust lists.
- Account editor page "Junk Mail" with the junk mail settings and lists.

* Mon Sep 28 2026 Rainer Backes <rbackes@bond.de> - 0.2.0-0
- Forward as attachment sends the GroupWise item itself.
- Subjects changed in GroupWise; SOAP API version 1.15.
- Trash: Restore, emptying drops still-linked messages.
- Subcalendars, proxy and shared calendars as calendars of their own; moving
  appointments between them; properties dialog; calendar order.
- GroupWise categories as labels (mail) and categories (calendar).
- New series get the right number of appointments.

* Sun Sep 27 2026 Rainer Backes <rbackes@bond.de> - 0.1.0-0
- First package: mail, contacts, calendar, tasks and notes; account assistant;
  German translation.
