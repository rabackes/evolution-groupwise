# evolution-groupwise

*[Deutsche Fassung](README.de.md)*

GroupWise mailboxes in [Evolution](https://gitlab.gnome.org/GNOME/evolution): mail, address books,
calendar, tasks and notes, through the SOAP interface of the GroupWise Post Office Agent (POA).
Written for Evolution / evolution-data-server 3.52 and tested with GroupWise 26.2.

An account is set up in Evolution's account assistant ("GroupWise" as server type) and gives, with one
login:

- **Mail**: all folders, read and write, move/copy/delete, drafts, sending through GroupWise (the
  recipients get a native GroupWise message). Message bodies are loaded when a message is opened.
- **Address books**: the GroupWise Address Book (read-only), Frequent Contacts and personal address books.
- **Calendar, tasks, notes**: appointments (all-day, alarms, busy status), meetings with invitations,
  answers, changes and retraction done by GroupWise itself, free/busy lookup; own subcalendars as
  calendars of their own, proxy calendars and calendars shared to the user (read only).
- **Proxy accounts**: the mailboxes of users who granted the user proxy rights, as accounts of their
  own beside the user's (in grey), to read and to send in the other user's name.

## Building

Requirements: CMake, a C compiler, gettext, and the development files of evolution-data-server
(camel, libedataserver, libebackend, libebook, libedata-book, libecal, libedata-cal), Evolution
(evolution-mail, evolution-shell; only for the account assistant module), libsoup 3, libxml2 and
libical-glib.

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

`-DWITH_EVOLUTION_UI=OFF` builds without the account assistant module (no Evolution development files
needed). The tests start a mock POA on localhost (Python 3).

### Packages

- **Debian / Ubuntu**: `dpkg-buildpackage -us -uc -b` (see `debian/`).
- **openSUSE / Fedora**: `packaging/rpm/evolution-groupwise.spec`, see `packaging/rpm/README.md`.

### Development install without root

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=$HOME/.local/opt/evolution-groupwise
cmake --build build && cmake --install build
systemctl --user set-environment EDS_EXTRA_PREFIXES=$HOME/.local/opt/evolution-groupwise
systemctl --user restart evolution-source-registry
EDS_EXTRA_PREFIXES=$HOME/.local/opt/evolution-groupwise evolution
```

evolution-data-server and Evolution also load modules from the prefixes in `EDS_EXTRA_PREFIXES`.
Remove such an install before installing a package, or every module is loaded twice.

## Setting up an account

In Evolution: *File → New → Mail Account*, enter the e-mail address, choose **GroupWise** as server
type. Server, port (7191) and user are prefilled from the address (user@example.com → user "user",
server "mail.example.com"); correct them if the GroupWise user ID differs.

### Certificates

POA certificates signed by the GroupWise system CA are trusted without questions once the CA is known:

- system wide: `sudo cp GW-CA.crt /usr/local/share/ca-certificates/ && sudo update-ca-certificates`
  (Debian/Ubuntu) or `sudo cp GW-CA.crt /etc/pki/trust/anchors/ && sudo update-ca-certificates` (openSUSE);
- for one user only: put the CA (PEM) into `~/.config/evolution-groupwise/ca/`
  (`$GROUPWISE_CA_DIR` replaces the directory).

The CA is part of the certificate chain of the GroupWise admin service:
`openssl s_client -connect ADMINHOST:9710 -showcerts`. Without it, Evolution asks once to trust the
certificate, for mail and for the address books and calendars of the account.

## How GroupWise maps to Evolution

### Calendar

- Appointments, tasks and notes live together in the GroupWise calendar folder; Evolution shows them in
  three sources. What the user sent to others lives in the Sent Items and is read from there.
- GroupWise sends and answers meetings itself (sendItem, accept, decline, retract); Evolution sends no
  iTIP mail for these calendars. Answering an invitation in Evolution (in the message, or with
  *Send RSVP* in the calendar's context menu) becomes accept/decline. A changed meeting is sent again to
  the attendees. Deleting an own meeting asks (capability `retract-supported`) for a reason and whether
  the attendees are told: with notice the meeting comes as a CANCEL to `send_objects` and is retracted
  from all mailboxes with the reason as retraction comment; without, it only leaves the user's mailbox.
- When the organizer invited themselves (the GroupWise client does so), Evolution shows the sent meeting
  with the attendees' answers and the alarm of the organizer's own copy. Evolution does the same now: a
  new meeting in a GroupWise calendar gets the owner of the calendar as an attendee (accepted), and the
  user among the attendees is sent along, so the POA keeps the own copy (accepted) — the home of the
  organizer's alarm, categories and travel time. As in the GroupWise client, a sent meeting shows in
  the calendar only with its own copy there (without one it is only in the Sent Items); the own copy
  has an item ID of its own.
- A meeting the user takes part in shows once: calendars of proxy sessions (proxy accounts, proxy
  calendars) leave out what an own calendar of the same login has (one list per account in the calendar
  factory; a change there refreshes the proxy calendars).
- Evolution's tooltip of an appointment is fixed (no hook for modules); the backend fills what it shows:
  an appointment of a proxy session without organizer gets the owner as ORGANIZER (marked `X-GW-OWNER`,
  taken away again by the appointment editor, so the appointment stays no meeting), and the attendees of a
  meeting get their answer as `X-RESPONSE-COMMENT` (the tooltip's "Comments"), all up to eight, else only
  the users the account works for as proxy (from the proxy accounts and proxy calendars set up). Display
  only; the backend sends neither to GroupWise.
- Resources (CUTYPE RESOURCE/ROOM, which Evolution's "Resources" puts in the role NON-PARTICIPANT) go as
  TO, as the GroupWise client invites them. A meeting without location that invites a resource of the kind
  place gets its name as location, as in the GroupWise client: `resolve` tells which attendees are
  resources, the resource's entry in the system address book (`<uuid>@55`) has `<name>` and
  `<flags><place>`. Resources go with their name as displayName: without it the POA makes "Beamer
  Beamer" (first and last name), which the backend reads back as "Beamer". The appointment editor fills
  the location as soon as the attendees change; the address book backend marks places in their contacts
  (`X-GROUPWISE-PLACE:1`). A shortened attendee list in the tooltip ends in "… (+N)".
- Saved new or with a new time, an appointment of a GroupWise calendar is checked against the busy time of
  the own calendars of the account (travel times of others included, its own with it); on an overlap the
  editor asks, as the GroupWise client warns. Free, declined, cancelled and all-day items do not count.
- Unanswered invitations appear bold in the calendar, tentative ones in italics, declined ones struck
  through. An answered invitation leaves the Mailbox, as in the GroupWise client.
- All-day events run from local midnight of the first day to midnight after the last one (like DTEND);
  `<alarm>` counts seconds.
- Series: GroupWise does not store a recurrence rule, it keeps every instance as an item of its own;
  Evolution shows them one by one. A new series (daily, weekly, monthly, yearly) is created from its rule.
- Own subcalendars are calendars of their own, in the colors they have in GroupWise (the colors can be
  changed in Evolution). The main calendar shows only appointments of no subcalendar; an appointment of
  "Private" shows in "Private" only (in GroupWise it is linked into the main calendar as well). A new
  appointment in a subcalendar is created there. Moving an appointment to another calendar of the
  account (drag and drop onto a calendar, *Move to Calendar*, or the calendar in the editor) moves the
  GroupWise item: it stays the same appointment, a meeting is not sent again. Subscribed Internet
  calendars are left out.
- Proxy calendars (the calendars of other users the user has proxy rights for) and calendars shared to
  the user are calendars of their own as well, read only for now. A proxy calendar is read with a proxy
  login into the other mailbox, with the user's own password.
- Free/busy comes from the busy search of the POA (internal users).

### Changes of the mailbox at once (events)

Account option *Ask the server for changes of the mailbox every … seconds* (off by default, 60 s, at least
15): GroupWise Web Services Events. The store keeps an event configuration under a key of the installation
(`Evolution_<hash of the machine ID>_<login>`; in the other user's mailbox for a proxy account) for
FolderItemAdd, FolderItemMove, ItemDelete, ItemUndelete, ItemPurge, ItemMarkRead, ItemMarkUnread and
ItemModify, reads the records with `getEvents` and `remove`, and puts them into the open folders at once: read and
unread are set, what left a folder (deleted, purged, moved away) is taken out of it — the cheap check of a
folder (getQuickMessages) sees neither a deletion nor, it seems, a mail read in the GroupWise client. A
folder that got an item or whose item changed is refreshed (new mail opens its folder; the Sent Items and
the Trash as views). The
records are kept one day; switching the option off removes configuration and records. Found on the POA:
event types go as `<events><event>…</event></events>` (a plain list is accepted and records nothing); a
record names the item without type and container; read/unread and purge records name no folder; it works
in proxy sessions, which have no getQuickMessages. Other applications keep configurations there too
(GroupWise Mobility): only the own key is touched. The proxy accounts of the same login take the option and
the interval over from the main account (`e-groupwise-proxy-options.c`: when the main account changes,
when a proxy account is made, and at start).

The key carries a hash of `/etc/machine-id`: two machines on one mailbox each have their own records, but
*cloned* machines share the ID and take each other's records (and only one is told on the port). A clone
needs a new machine ID (`systemd-machine-id-setup` after removing `/etc/machine-id`); the manual says how.
A key whose machine is gone stays in the mailbox as a configuration; its records expire after a day.

Calendars, task and memo lists follow the same events. Their backend runs in another process (the calendar
factory), so the mail store only counts what concerns them — a record that names a folder outside the mail
tree (the Calendar, a subcalendar), or an accept, decline or completion — in an object datum
(`groupwise-events-calendar`); the Evolution module (`e-groupwise-calendar-events.c`) watches it and
refreshes the opened calendars and lists of that mailbox (`e_client_refresh`), those of the account and
the proxy calendars of the same user in other accounts. (A configuration of the backend's own, limited to
the item types Appointment, Task and Note, was tried first: the POA recorded nothing for it of what the
GroupWise client did.)

Second option *Let the server tell at once, on port … of this computer* (off by default, 5221): the
configuration then names the address this machine reaches the POA from and the port; the POA connects
there and sends `<notify xmlns='urn:novell:schemas:ns:events'><userid/><key/></notify>` for the first new
record, once, until `getEvents` is called with `notify` again (always, with the port). One listener per
port and process for all accounts (`GSocketService`); a line with the store's key starts the question at
once (not more often than every other second). The POA must reach the machine: a firewall or NAT in
between, and nothing arrives — the store notices records it was to be told and was not (twice in a row)
and sets `groupwise-events-port-unreachable` on itself; `e-groupwise-events-port.c` asks the user then
and opens the port with `firewall-cmd --permanent --add-port` and `--add-port` (polkit asks for the
password). Measured: told about 0.6 s after delivery, the message in the folder after 2 s.

### Junk mail

The Junk Mail folder of GroupWise is Evolution's junk folder. A message marked as junk goes there, one
marked as not junk back into the Mailbox. Marking an Internet sender's message also teaches GroupWise:
junk puts the address onto the GroupWise junk list (and off the trust list), not junk onto the trust
list (and off the junk and block lists). Evolution's own junk test only moves messages. The context
menu of a message has *GroupWise Junk Mail* with *Trust / Junk / Block the Sender…*, for the address or
the whole domain, and *Junk Mail Settings…*; below it Evolution's *Add Sender to Address Book*. In the settings, the context
menu of each list moves entries onto one of the other lists (removed there, added here on *OK*). The junk mail settings and the three lists of the server are in the window *GroupWise
Settings…* (context menu of the account in the folder list).

### GroupWise Settings, out of office

The context menu of the account in the folder list opens *GroupWise Settings…*: the settings the server
keeps for the mailbox, for every client. The tab *Out of Office* is the out of office rule of the
GroupWise client (on/off, a date range of whole days or with times, subject and text of the reply, a
reply of its own to external senders – everyone or contacts only); GroupWise puts an out of office
appointment into the calendar for the range. The tab *Junk Mail* has the junk mail settings and lists.
The tab *Proxy Accounts* lists the users who granted the user proxy rights (getProxyList) and adds or
removes proxy accounts for them; the tab *Proxy Access* manages who may open the user's mailbox.

### Proxy accounts

Where the GroupWise client switches its whole window to the other mailbox, Evolution shows it as an
account of its own: a collection with the login of the user's account and `Proxy=<address>` in its
`[Groupwise Backend]` section, and below it a mail account, an identity with the other user's name and
address and a transport. The registry module fills it like the user's account, in a proxy session: the
other user's Calendar and own subcalendars with task and memo lists (unselected at first, no reminders),
and the personal address books (no autocompletion; the system address book is the user's own anyway).
The user's collection leaves out the proxy calendars of users that have a proxy account. The user's
password is stored for the proxy collection when it is made. A collection of its own because Evolution
takes every child of a collection for the collection (renaming, enabling and removing act on it).
Proxy accounts of 0.3 and 0.4 (mail sources without collection) are made anew by the *Proxy Accounts*
tab.

The proxy login keeps to the rights granted there: without the right to write mail the mail account is
read-only, without the right to read mail it does not connect; calendars, task and memo lists are
writable with the appointment, task and note write rights. The transport sends through the proxy session,
so the message goes out in the other user's name and lands in the other user's Sent Items. The account and
its folders are grey in the folder list. *GroupWise Settings…* and the junk mail submenu act on the other
mailbox, with the proxy right to change settings, rules and folders (`<misc><setup>`). The POA answers no
getQuickMessages in a proxy session (error 59414), so each refresh of a proxy folder reads the state of
all its items.

### Proxy access to the user's mailbox

The tab *Proxy Access* of *GroupWise Settings…* is the proxy access list of the GroupWise client
(getProxyAccessList, createProxyAccess, modifyProxyAccess, removeProxyAccess): who may log in as proxy,
with read and write rights for mail, appointments, reminder notes and tasks and the other rights (alarms,
notifications, options/rules/folders, private items, security options — `setupSecurity`, not in the
SDK). The entry `<All User Access>` stands for every user. A new user is resolved first (resolveRequest):
the POA grants only with the UUID and answers success without it.

### Labels and categories

GroupWise categories are Evolution's labels (mail) and categories (calendar, tasks, notes). The built-in
categories are Evolution's built-in labels: Urgent = Important, Personal = Personal, Follow-up = To Do,
Low priority = Later; every other category is the label of the same name, added to Evolution's label list
with its GroupWise color. Setting a label or category in Evolution sets the category in GroupWise; one
GroupWise does not know yet is created there (as GroupWise Web does). In the event editor the
categories field is shown with *View → Categories*.

### Trash

The GroupWise Trash holds what was deleted from any folder. Emptying it purges what is in no other folder;
messages still in another folder only leave the Trash. **Restore** in the Trash (at the top of the context
menu of the message list, or *Edit → Restore*) puts messages back
into the folders they were deleted from, as "Undelete" in the GroupWise clients; messages whose folder is
gone go into the Mailbox. The action needs Evolution 3.56 or later.

### Follow-up flag and Tasklist

Evolution's follow-up flag of a mail (`follow-up`, `due-by`, `completed-on` user tags) is the item being
on the GroupWise Tasklist, both ways. Found on the POA: the Tasklist is a view — an item is on it while
it has a checklist entry (`modifyItem <update><checklist><sequence>`; `moveItem` into the Tasklist would
take it out of its folder), with `checklist/dueDate`; `complete`/`uncomplete` set `checklist/completed`;
`<delete><checklist/></delete>` takes it off (a sequence of 0 does not), `<delete><checklist><dueDate/>`
only the due date. After taking it off a checklist with sequence 0 remains, so only the listing of the
Tasklist folder tells what is on it; the folders of an account share it for 60 s per refresh. The server
state is kept per message (like the categories): a change of the user's own is written back, the
server's is taken over. Flags set before the Tasklist was known go onto it once.

### Delivery status and retract

In the Sent Items of a GroupWise account, the message context menu and the *Message* menu get *Delivery
Status…* (a window with each recipient and the events of its `<recipientStatus>` with their times:
delivered, opened, deleted, replied, forwarded, retracted, accepted/declined with comment …) and
*Retract…* (after a question: `retract` with `retractType` recipientMailboxes, as the GroupWise client
does; the sent item stays, its recipients show "retracted") and *Resend…* (the message in a composer as
with *Edit as New Message*; the composer's `presend` asks whether the original is retracted, which then
happens when it is sent). Found on the POA: a mail is retracted only
by its ID *with the container* — without it the POA answers success and does nothing. Both use a
connection of their own (a proxy login for a proxy account, without the right to change settings).

### Preparation and travel time

GroupWise can block time before and after an appointment (`travelTimeBefore`/`travelTimeAfter` in seconds);
the POA keeps an appointment "Before: …" and "After: …" for it (source personal, linked by
`travelAppointmentBefore/After` and `travelAppointmentBacklink`). Found out on the POA: it creates them on
sendItem, moves, renames and re-anchors them when the main appointment is modified, removes them with it and
links them along into a subcalendar — but it takes changes to them as they come. A travel time cannot be
set back (neither 0 nor `<delete>`); the GroupWise client takes it away by deleting the link
(`<delete><travelAppointmentAfter>…`) and purging the appointment, the number stays without effect. Setting
a value again makes a new appointment. The travel time is the user's own: it is not sent to attendees, and
for a meeting it lives on the organizer's own copy (received items can have one, too).

For a new meeting the backend keeps the travel time until the next listing (scheduled right after
sending) finds the own copy, and sets it there; a cursor with an iCalId filter right after sendItem had
made the SOAP process of the POA crash. A move into another calendar refreshes the calendar it came from,
so the extra appointments leave it at once.

The backend shows the times as `X-GW-TRAVEL-BEFORE/AFTER` while the link exists and marks the extra
appointments with `X-GW-TRAVEL-OF`. Changed in Evolution, it sets them on the item (the own copy of a
meeting; for a new meeting after sending, found by its iCalId) or takes them away as the GroupWise client
does. Moving an extra appointment moves the main appointment by as much, resizing it changes the travel
time, deleting it is refused. The appointment editor gets a *Travel Time* tab with the red car of the
GroupWise client (hours and minutes before and after, minutes in steps of 5, "Travel times before/after the
appointment are the same"), for GroupWise calendars only and not for the extra appointments.

### Rules

The *Rules* tab of *GroupWise Settings…* lists, edits, copies, removes, orders and runs the rules of the
mailbox (getRuleList, createItem Rule, modifyItem, removeItem, executeRule) with the scope of the
GroupWise client: event (new item with sources, filed item, open/close folder, completed item, startup,
exit, user activated), item types, appointment conflicts, conditions joined by and/or (one level) on the
fields of the client (found out with rules made there: dates and counter comparisons are `field*` ops with
the other operand — `Today` or a counter such as `totalUsers` — in `<date>` and an offset in `<value>`;
fields without a schema name by number, e.g. 1436 due date, 119 assigned date, 922 completed date, 120 task
category as character code, 122 task priority, 532 third-party archive status; enumerations as `bitCare`,
some of them written twice with an empty `<mask/>`), actions in their order. Found out on the POA: a new filter clears types and source (they are sent along); `<update>`
appends actions (they are replaced by delete and add); the sequence is not renumbered. `Reply` answers
each sender once a day and avoids loops, `ReplyWithText` (its predecessor) answers everything — offered
last as "Reply unconditionally (dangerous)". SimpleForward forwards inline, Forward as attachment.
Rules the editor cannot show fully (nested groups, other fields or actions) are only shown. The POA runs
new and filed item rules itself; the client's events run only on executeRule: Evolution does that for
startup/exit and folder open/close when the account options (`run-startup-rules`, `run-folder-rules`)
say so.

### Signatures

The signatures of the mailbox (getSignatures; a MIME document each, as the GroupWise client writes it) are
signatures of Evolution, children of the collection: the registry module brings them at each login of the
collection (content as UTF-8 HTML with the pictures as data: URIs, the GroupWise ID and a checksum of the
synchronized content in the `GroupWise Folder` extension) and removes those gone from the server. The
GroupWise default becomes the identity's signature whenever it changed in GroupWise, also with "prompt
before sending". Evolution sends edits back (a file monitor compares the content with the checksum; a
rename; a new signature below the collection, made in the *Signatures* tab; a removal, unless the whole
collection goes). The *Signatures* tab of *GroupWise Settings…* adds, edits (Evolution's signature
editor), removes and sets the default.

### System folder names

The system folders come first, in the order of the GroupWise client (Mailbox, Sent Items, Tasklist, Work
In Progress, Junk Mail, Trash, Cabinet), through the `compare-folders` signal of the folder tree model; an
order the user sets in Evolution wins. Shared folders whose parent is gone are left out, as in the
GroupWise client.

The system folders show under the names of the GroupWise client in the user's language (German:
Aktenschrank, Ausgangsnachrichten, In Arbeit, Jobliste, Junkmail, Papierkorb); their full names stay those of the server.

The HTML body of a sent message goes with `<charset>UTF-8</charset>` and a `<meta charset>` in its head:
without, the POA stores it as US-ASCII and the GroupWise client shows its umlauts as two characters.

### Search result folders

GroupWise search result folders (e.g. "All Mail") are views onto items of other folders. Evolution shows
them; deleting and moving act in the folder a message is really in, nothing can be moved into them. They
are checked at most every 15 minutes and not kept for offline use.

**Limit:** through SOAP the POA returns only part of the hits of a search result folder (a little over
4000 in tests, of 61552 shown by the GroupWise client), whatever view, count, filter or API version is
used. For complete searches use Evolution's own search folders (*Edit → Search Folders*).

### Not yet supported

Shared mail folders and address books, writing into other users' calendars, signed or encrypted messages (GroupWise builds the message for the recipients itself).

## Tools

`gw-cli` (installed to `lib/evolution-groupwise/`) talks to a POA directly, for trying things out and
for debugging:

```sh
export GW_HOST=poa.example.com GW_USER=user        # password: $GW_PASSWORD, or asked for
gw-cli --ssl login
gw-cli --ssl folders
gw-cli --ssl items FOLDER-ID 20
gw-cli --ssl mime ITEM-ID > message.eml
gw-cli --ssl raw getTimestamp '<noop>true</noop>'
```

The build tree also has `gw-camel-check`, `gw-eds-check` and `gw-cal-check`, which go through Camel
and a running evolution-data-server. Log output: `G_MESSAGES_DEBUG=evolution-groupwise`.

## Translations

The messages are in English; `po/` holds the translations (German so far). `make update-po` in the build
directory refreshes the template `po/evolution-groupwise.pot` and merges it into the translations; a
new language is added as `po/<lang>.po` and listed in `po/LINGUAS`.

## License

GNU Lesser General Public License, version 2.1 or later (see `COPYING`), like evolution-data-server.

Copyright © 2026 bond Software Entwicklung GmbH.
