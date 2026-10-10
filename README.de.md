# evolution-groupwise

*[English version](README.md)*

GroupWise-Postfächer in [Evolution](https://gitlab.gnome.org/GNOME/evolution): Mail, Adressbücher,
Kalender, Aufgaben und Notizen, über die SOAP-Schnittstelle des GroupWise Post Office Agent (POA).
Geschrieben für Evolution / evolution-data-server 3.52, getestet mit GroupWise 26.2.

Ein Konto wird im Konto-Assistenten von Evolution eingerichtet (Servertyp „GroupWise“) und bietet mit
einer Anmeldung:

- **Mail**: alle Ordner, lesen und schreiben, verschieben/kopieren/löschen, Entwürfe, Senden über
  GroupWise (die Empfänger erhalten eine native GroupWise-Nachricht). Der Inhalt einer Nachricht wird
  erst beim Öffnen geladen.
- **Adressbücher**: GroupWise-Adressbuch (nur lesen), Häufige Kontakte und persönliche Adressbücher.
- **Kalender, Aufgaben, Notizen**: Termine (ganztägig, Erinnerungen, Belegt-Status), Besprechungen mit
  Einladungen, Antworten, Änderungen und Zurückziehen durch GroupWise selbst, Frei/Belegt-Suche; eigene
  Unterkalender als eigene Kalender, Proxy-Kalender und freigegebene Kalender (nur lesen).
- **Proxy-Konten**: die Postfächer von Benutzern, die einem Proxy-Rechte gegeben haben, als eigene
  Konten neben dem eigenen (in grauer Schrift), zum Lesen und Senden im Namen des anderen Benutzers.

## Bauen

Voraussetzungen: CMake, ein C-Compiler, gettext sowie die Entwicklungsdateien von
evolution-data-server (camel, libedataserver, libebackend, libebook, libedata-book, libecal,
libedata-cal), Evolution (evolution-mail, evolution-shell; nur für das Modul des Konto-Assistenten),
libsoup 3, libxml2 und libical-glib.

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

`-DWITH_EVOLUTION_UI=OFF` baut ohne das Modul des Konto-Assistenten (ohne Evolution-Entwicklungsdateien).
Die Tests starten einen Mock-POA auf localhost (Python 3).

### Pakete

- **Debian / Ubuntu**: `dpkg-buildpackage -us -uc -b` (siehe `debian/`).
- **openSUSE / Fedora**: `packaging/rpm/evolution-groupwise.spec`, siehe `packaging/rpm/README.md`.

### Entwicklungsinstallation ohne root

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=$HOME/.local/opt/evolution-groupwise
cmake --build build && cmake --install build
systemctl --user set-environment EDS_EXTRA_PREFIXES=$HOME/.local/opt/evolution-groupwise
systemctl --user restart evolution-source-registry
EDS_EXTRA_PREFIXES=$HOME/.local/opt/evolution-groupwise evolution
```

evolution-data-server und Evolution laden Module auch aus den Präfixen in `EDS_EXTRA_PREFIXES`. Vor der
Installation eines Pakets muss eine solche Installation weg, sonst wird jedes Modul doppelt geladen.

## Konto einrichten

In Evolution: *Datei → Neu → E-Mail-Konto*, E-Mail-Adresse eingeben, als Servertyp **GroupWise**
wählen. Server, Port (7191) und Benutzer sind aus der Adresse vorbelegt (benutzer@firma.de → Benutzer
„benutzer“, Server „mail.firma.de“); abweichende GroupWise-Kennungen hier korrigieren.

### Zertifikate

POA-Zertifikate, die von der GroupWise-System-CA signiert sind, gelten ohne Nachfrage als
vertrauenswürdig, sobald die CA bekannt ist:

- systemweit: `sudo cp GW-CA.crt /usr/local/share/ca-certificates/ && sudo update-ca-certificates`
  (Debian/Ubuntu) bzw. `sudo cp GW-CA.crt /etc/pki/trust/anchors/ && sudo update-ca-certificates` (openSUSE);
- nur für einen Benutzer: die CA (PEM) nach `~/.config/evolution-groupwise/ca/` legen
  (`$GROUPWISE_CA_DIR` ersetzt das Verzeichnis).

Die CA steht in der Zertifikatskette des GroupWise-Admin-Dienstes:
`openssl s_client -connect ADMINHOST:9710 -showcerts`. Ohne sie fragt Evolution einmal nach dem
Zertifikat, für die Mail und für Adressbücher und Kalender des Kontos.

## Wie GroupWise in Evolution abgebildet wird

### Kalender

- Termine, Aufgaben und Notizen liegen in GroupWise zusammen im Kalenderordner; Evolution zeigt sie in
  drei Quellen. Was der Benutzer an andere geschickt hat, liegt in den Ausgangsnachrichten (Sent Items) und wird
  von dort gelesen.
- Besprechungen verschickt und beantwortet GroupWise selbst (sendItem, accept, decline, retract);
  Evolution schickt für diese Kalender keine iTIP-Mails. Eine Antwort in Evolution (in der Nachricht
  oder im Kontextmenü des Kalenders mit *UAwg senden*) wird zu accept/decline. Eine geänderte
  Besprechung schickt GroupWise den Teilnehmern neu. Beim Löschen einer eigenen Besprechung fragt Evolution
  (Fähigkeit `retract-supported`) nach einem Grund und ob die Teilnehmer benachrichtigt werden: mit
  Benachrichtigung kommt die Besprechung als CANCEL bei `send_objects` an und wird mit dem Grund als
  Kommentar aus allen Postfächern zurückgezogen; ohne verschwindet sie nur beim Benutzer.
- Hat sich der Organisator selbst eingeladen (der GroupWise-Client tut das), zeigt Evolution die
  gesendete Besprechung mit den Antworten der Teilnehmer und der Erinnerung der eigenen Kopie. Evolution
  macht es jetzt genauso: Eine neue Besprechung in einem GroupWise-Kalender bekommt den Inhaber des
  Kalenders als Teilnehmer (angenommen), und der Benutzer unter den Teilnehmern wird mitgeschickt, damit
  der POA die eigene Kopie führt (angenommen) – den Ort für Erinnerung, Kategorien und Reisezeit des
  Organisators. Wie im GroupWise-Client erscheint eine gesendete Besprechung nur mit ihrer eigenen Kopie
  im Kalender (ohne sie steht sie nur in den Ausgangsnachrichten); die eigene Kopie hat eine eigene ID.
- Eine Besprechung, an der der Benutzer teilnimmt, erscheint einmal: Kalender von Proxy-Sitzungen
  (Proxy-Konten, Proxy-Kalender) lassen aus, was ein eigener Kalender derselben Anmeldung hat (eine Liste
  je Konto in der Calendar-Factory; eine Änderung dort gleicht die Proxy-Kalender neu ab).
- Die Kurzinfo (Tooltip) eines Termins baut Evolution fest selbst (ohne Einhakpunkt für Module); das Backend
  füllt, was sie zeigt: Ein Termin einer Proxy-Sitzung ohne Organisator bekommt den Eigentümer als ORGANIZER
  (markiert mit `X-GW-OWNER`, der Termin-Editor nimmt ihn wieder heraus, damit der Termin keine Besprechung
  wird), und die Teilnehmer einer Besprechung bekommen ihre Antwort als `X-RESPONSE-COMMENT` („Kommentare“
  der Kurzinfo), bis acht alle, sonst nur die Benutzer, für die das Konto als Proxy arbeitet (aus den
  eingerichteten Proxy-Konten und Proxy-Kalendern). Nur zur Anzeige; das Backend schickt beides nicht an
  GroupWise.
- Ressourcen (CUTYPE RESOURCE/ROOM, die Evolutions „Ressourcen“ in die Rolle NON-PARTICIPANT setzt) gehen als
  TO, wie der GroupWise-Client sie einlädt. Eine Besprechung ohne Ort, die eine Ressource vom Typ Ort
  einlädt, bekommt wie im GroupWise-Client deren Namen als Ort: `resolve` sagt, welche Teilnehmer Ressourcen
  sind, der Eintrag der Ressource im Systemadressbuch (`<uuid>@55`) hat `<name>` und `<flags><place>`.
  Ressourcen gehen mit ihrem Namen als displayName: ohne ihn macht der POA „Beamer Beamer“ (Vor- und
  Nachname), was das Backend als „Beamer“ zurückliest. Der Termin-Editor setzt den Ort schon, wenn sich die
  Teilnehmer ändern; das Adressbuch-Backend kennzeichnet Orte in ihrem Kontakt (`X-GROUPWISE-PLACE:1`). Eine
  gekürzte Teilnehmerliste in der Kurzinfo endet mit „… (+N)“.
- Neu oder mit neuer Zeit gespeichert, wird ein Termin eines GroupWise-Kalenders gegen die belegte Zeit der
  eigenen Kalender des Kontos geprüft (samt Reisezeiten anderer und seiner eigenen); bei einer
  Überschneidung fragt der Editor nach, wie der GroupWise-Client warnt. Freie, abgelehnte, abgesagte und
  ganztägige Einträge zählen nicht.
- Unbeantwortete Einladungen erscheinen im Kalender fett, vorläufig angenommene kursiv, abgelehnte
  durchgestrichen. Eine beantwortete Einladung verschwindet wie im GroupWise-Client aus der Mailbox.
- Ganztägige Termine reichen von Mitternacht des ersten bis Mitternacht nach dem letzten Tag (wie
  DTEND); `<alarm>` zählt Sekunden.
- Serien: GroupWise speichert keine Wiederholungsregel, sondern jede Instanz als eigenes Element;
  Evolution zeigt sie einzeln. Eine neue Serie (täglich, wöchentlich, monatlich, jährlich) wird aus
  ihrer Regel angelegt.
- Eigene Unterkalender sind eigene Kalender, in den Farben aus GroupWise (in Evolution änderbar). Der
  Hauptkalender zeigt nur Termine, die in keinem Unterkalender liegen; ein Termin aus „Privat“ steht nur
  in „Privat“ (in GroupWise ist er zusätzlich mit dem Hauptkalender verknüpft). Ein neuer Termin in einem
  Unterkalender wird dort angelegt. Einen Termin in einen anderen Kalender des Kontos verschieben (auf
  einen Kalender ziehen, *In Kalender verschieben …* oder die Kalenderauswahl im Editor) verschiebt das
  GroupWise-Element: Es bleibt derselbe Termin, eine Besprechung wird nicht erneut versendet.
  Abonnierte Internet-Kalender werden ausgelassen.
- Proxy-Kalender (Kalender anderer Benutzer, für die man Proxy-Rechte hat) und für den Benutzer
  freigegebene Kalender sind ebenfalls eigene Kalender, vorerst nur lesbar. Ein Proxy-Kalender wird über
  eine Proxy-Anmeldung am anderen Postfach gelesen, mit dem eigenen Passwort.
- Frei/Belegt kommt aus der Belegt-Suche des POA (interne Benutzer).

### Änderungen im Postfach sofort (Events)

Kontooption *Den Server alle … Sekunden nach Änderungen im Postfach fragen* (Vorgabe aus, 60 s, mindestens
15): GroupWise Web Services Events. Der Store hält eine Event-Konfiguration unter einem Schlüssel der
Installation (`Evolution_<Hash der Maschinen-ID>_<Anmeldung>`; bei einem Proxy-Konto im Postfach des anderen
Benutzers) für FolderItemAdd, FolderItemMove, ItemDelete, ItemUndelete, ItemPurge, ItemMarkRead,
ItemMarkUnread und ItemModify, liest die Datensätze mit `getEvents` und `remove` und gleicht nur die Ordner
ab, die sie nennen (`container`/`from`, sonst der geöffnete Ordner mit dem Objekt; Ausgangsnachrichten und
Papierkorb als Ansichten). Die Datensätze bleiben einen Tag; Ausschalten entfernt Konfiguration und
Datensätze. Am POA ermittelt: Die Ereignistypen gehen als `<events><event>…</event></events>` (eine einfache
Liste wird angenommen und zeichnet nichts auf); ein Datensatz nennt das Objekt ohne Typ und Container;
gelesen/ungelesen und getilgt nennen keinen Ordner; es funktioniert in Proxy-Sitzungen, die kein
getQuickMessages haben. Andere Anwendungen halten dort ebenfalls Konfigurationen (GroupWise Mobility):
angefasst wird nur der eigene Schlüssel. Der POA kann sich für jeden ersten neuen Datensatz auch beim Client
melden (noch nicht genutzt).

### Junk-Mail

Der Ordner „Junkmail“ (Junk Mail) von GroupWise ist Evolutions Junk-Ordner. Eine als unerwünscht markierte Nachricht kommt
dorthin, eine als erwünscht markierte zurück in die Mailbox. Das Markieren der Nachricht eines
Internet-Absenders lehrt auch GroupWise: Junk setzt die Adresse auf die Junk-Liste von GroupWise (und
von der Vertrauensliste), „Als erwünscht markieren“ auf die Vertrauensliste (und von Junk- und Blockliste). Evolutions
eigene Junk-Prüfung verschiebt nur. Das Kontextmenü einer Nachricht hat *GroupWise-Junkmail* mit
*Sender als verbürgt einstufen / als Junk einstufen / blockieren …*, für die Adresse oder die ganze
Domain, und *Junkmail-Einstellungen …*; dazu *Absender zum Adressbuch hinzufügen*. Die
Junk-Mail-Einstellungen und die drei Listen des Servers stehen im Fenster *GroupWise-Einstellungen …*
(Kontextmenü des Kontos in der Ordnerliste). Dort verschiebt das Kontextmenü jeder Liste Einträge auf eine der anderen
Listen (dort entfernt, hier hinzugefügt bei *OK*).

### GroupWise-Einstellungen, Abwesenheit

Das Kontextmenü des Kontos in der Ordnerliste öffnet *GroupWise-Einstellungen …*: die Einstellungen, die
der Server für das Postfach hält, für alle Programme. Die Registerkarte *Abwesenheit* ist die
Abwesenheitsregel des GroupWise-Clients (ein/aus, Zeitraum in ganzen Tagen oder mit Uhrzeit, Betreff
und Text der Antwort, eigene Antwort an externe Sender – allen oder nur den Kontakten); GroupWise
trägt für den Zeitraum einen Abwesenheitstermin in den Kalender ein. Die Registerkarte *Junkmail* hat
die Junk-Mail-Einstellungen und -Listen. Die Registerkarte *Proxy-Konten* listet die Benutzer, die einem
Proxy-Rechte gegeben haben (getProxyList), und richtet Proxy-Konten für sie ein oder entfernt sie; die
Registerkarte *Proxy-Zugriff* verwaltet, wer das eigene Postfach öffnen darf.

### Proxy-Konten

Wo der GroupWise-Client sein ganzes Fenster auf das andere Postfach umschaltet, zeigt Evolution es als
eigenes Konto: eine Sammlung mit der Anmeldung des eigenen Kontos und `Proxy=<Adresse>` im Abschnitt
`[Groupwise Backend]`, darunter ein Mailkonto, eine Identität mit Namen und Adresse des anderen Benutzers
und ein Transport. Das Registry-Modul füllt sie wie das eigene Konto, in einer Proxy-Sitzung: Kalender und
eigene Unterkalender des anderen Benutzers mit Aufgaben- und Notizlisten (anfangs nicht angekreuzt, ohne
Erinnerungen) und die persönlichen Adressbücher (ohne Autovervollständigung; das Systemadressbuch hat der
Benutzer ohnehin). Die eigene Sammlung lässt die Proxy-Kalender von Benutzern weg, für die es ein
Proxy-Konto gibt. Das Passwort des Benutzers wird beim Einrichten auch für die Proxy-Sammlung gespeichert.
Eine eigene Sammlung, weil Evolution jedes Kind einer Sammlung für die Sammlung selbst hält (Umbenennen,
Aktivieren und Entfernen wirken auf sie). Proxy-Konten aus 0.3 und 0.4 (Mailquellen ohne Sammlung) richtet
die Registerkarte *Proxy-Konten* neu ein.

Die Proxy-Anmeldung hält sich an die dort gewährten Rechte: ohne Schreibrecht für Mail ist das Mailkonto
schreibgeschützt, ohne Leserecht verbindet es sich nicht; Kalender, Aufgaben- und Notizlisten sind mit dem
Schreibrecht für Termine, Aufgaben und Notizen beschreibbar. Der Transport sendet über die Proxy-Sitzung;
die Nachricht geht im Namen des anderen Benutzers hinaus und liegt danach in dessen
Ausgangsnachrichten. Konto und Ordner sind in der Ordnerliste grau. *GroupWise-Einstellungen …* und das
Junk-Mail-Untermenü wirken auf das andere Postfach, mit dem Proxy-Recht, Einstellungen, Regeln und Ordner
zu ändern (`<misc><setup>`). In einer Proxy-Sitzung beantwortet der POA kein getQuickMessages (Fehler
59414); jeder Abgleich eines Proxy-Ordners liest deshalb den Status aller Einträge.

### Proxy-Zugriff auf das eigene Postfach

Die Registerkarte *Proxy-Zugriff* der *GroupWise-Einstellungen …* ist die Proxy-Zugriffsliste des
GroupWise-Clients (getProxyAccessList, createProxyAccess, modifyProxyAccess, removeProxyAccess): wer sich
als Proxy anmelden darf, mit Lese- und Schreibrechten für Mail, Termine, Erinnerungsnotizen und Aufgaben und
den übrigen Rechten (Alarme, Benachrichtigungen, Optionen/Regeln/Ordner, private Objekte,
Sicherheitsoptionen – `setupSecurity`, nicht im SDK). Der Eintrag `<All User Access>` steht für alle
Benutzer. Ein neuer Benutzer wird zuerst aufgelöst (resolveRequest): der POA vergibt Rechte nur mit der
UUID und meldet ohne sie trotzdem Erfolg.

### Beschriftungen und Kategorien

GroupWise-Kategorien sind in Evolution Beschriftungen (Mail) und Kategorien (Kalender, Aufgaben,
Notizen). Die vordefinierten Kategorien sind Evolutions Standard-Beschriftungen: Urgent = Wichtig,
Personal = Persönlich, Follow-up = Zu erledigen, Low priority = Später; jede andere Kategorie ist die
Beschriftung gleichen Namens und wird mit ihrer GroupWise-Farbe in Evolutions Beschriftungsliste
aufgenommen. Eine Beschriftung oder Kategorie in Evolution setzt die Kategorie in GroupWise; eine, die
GroupWise noch nicht kennt, wird dort angelegt (wie in GroupWise Web). Im Termin-Editor zeigt
*Ansicht → Kategorien* das Kategorienfeld.

### Papierkorb

Der GroupWise-Papierkorb enthält, was aus irgendeinem Ordner gelöscht wurde. Beim Leeren wird endgültig
gelöscht, was in keinem anderen Ordner mehr liegt; Nachrichten, die noch in einem anderen Ordner liegen,
verlassen nur den Papierkorb. **Wiederherstellen** im Papierkorb (ganz oben im Kontextmenü der
Nachrichtenliste oder *Bearbeiten → Wiederherstellen*) legt Nachrichten in die Ordner
zurück, aus denen sie gelöscht wurden, wie „Wiederherstellen“ in den GroupWise-Clients; Nachrichten,
deren Ordner nicht mehr existiert, kommen in die Mailbox. Die Aktion braucht Evolution 3.56 oder neuer.

### Folgenachricht und Jobliste

Evolutions Markierung zur Nachverfolgung einer Mail (Tags `follow-up`, `due-by`, `completed-on`) ist das
Objekt auf der Jobliste von GroupWise, in beide Richtungen. Am POA ermittelt: Die Jobliste ist eine Ansicht –
ein Objekt steht darauf, solange es einen Jobliste-Eintrag hat (`modifyItem <update><checklist><sequence>`;
`moveItem` in die Jobliste würde es aus seinem Ordner nehmen), mit `checklist/dueDate`; `complete`/`uncomplete`
setzen `checklist/completed`; `<delete><checklist/></delete>` nimmt es herunter (eine Sequenz 0 nicht),
`<delete><checklist><dueDate/>` nur die Fälligkeit. Nach dem Herunternehmen bleibt ein Eintrag mit Sequenz 0,
daher sagt nur die Auflistung des Ordners Jobliste, was darauf steht; die Ordner eines Kontos teilen sie
je Abgleich 60 s. Der Serverstand liegt je Nachricht (wie bei den Kategorien): eine eigene Änderung geht
zum Server, eine des Servers wird übernommen. Markierungen von vorher kommen einmalig auf die Jobliste.

### Zustellstatus und Zurückziehen

In den Ausgangsnachrichten eines GroupWise-Kontos bekommen das Kontextmenü einer Nachricht und das Menü
*Nachricht* die Einträge *Zustellstatus …* (ein Fenster mit jedem Empfänger und den Ereignissen seines
`<recipientStatus>` mit Zeitpunkt: zugestellt, geöffnet, gelöscht, beantwortet, weitergeleitet,
zurückgezogen, angenommen/abgelehnt mit Kommentar …) und *Zurückziehen …* (nach einer Rückfrage: `retract`
mit `retractType` recipientMailboxes, wie der GroupWise-Client; das gesendete Objekt bleibt, seine
Empfänger erscheinen als „zurückgezogen“) und *Neu senden …* (die Nachricht in einem Nachrichtenfenster wie bei
*Als neue Nachricht bearbeiten*; das Signal `presend` des Fensters fragt, ob das Original zurückgezogen wird,
was dann beim Senden geschieht). Am POA ermittelt: Eine Mail wird nur über ihre ID *mit Container*
zurückgezogen – ohne ihn meldet der POA Erfolg und tut nichts. Beides nutzt eine eigene Verbindung (bei
einem Proxy-Konto eine Proxy-Anmeldung, ohne das Recht, Einstellungen zu ändern).

### Vorbereitungs- und Reisezeit

GroupWise kann Zeit vor und nach einem Termin belegen (`travelTimeBefore`/`travelTimeAfter` in Sekunden); der
POA hält dafür einen Termin „Before: …“ und „After: …“ (Quelle persönlich, verknüpft über
`travelAppointmentBefore/After` und `travelAppointmentBacklink`). Am POA ermittelt: er legt sie bei sendItem
an, verschiebt, benennt und richtet sie neu aus, wenn der Haupttermin geändert wird, entfernt sie mit ihm und
verknüpft sie in einen Unterkalender mit – Änderungen an ihnen selbst nimmt er aber ungeprüft hin. Eine
Reisezeit lässt sich nicht zurücksetzen (weder 0 noch `<delete>`); der GroupWise-Client entfernt sie, indem er
die Verknüpfung löscht (`<delete><travelAppointmentAfter>…`) und den Termin tilgt, die Zahl bleibt wirkungslos
stehen. Ein erneut gesetzter Wert legt einen neuen Termin an. Die Reisezeit gehört dem Benutzer allein: sie
geht nicht an Teilnehmer, und bei einer Besprechung steht sie an der eigenen Kopie des Organisators (auch
erhaltene Termine können eine haben).

Bei einer neuen Besprechung merkt sich das Backend die Reisezeit, bis der nächste Abgleich (gleich nach
dem Versand angestoßen) die eigene Kopie findet, und setzt sie dort; ein Cursor mit iCalId-Filter direkt nach
sendItem hatte den SOAP-Prozess des POA abstürzen lassen. Ein Verschieben in einen anderen Kalender gleicht
den bisherigen neu ab, damit die Zusatztermine dort sofort verschwinden.

Das Backend zeigt die Zeiten als `X-GW-TRAVEL-BEFORE/AFTER`, solange die Verknüpfung besteht, und kennzeichnet
die Zusatztermine mit `X-GW-TRAVEL-OF`. In Evolution geändert, setzt es sie am Termin (bei einer Besprechung an
der eigenen Kopie; bei einer neuen nach dem Versand, gefunden über die iCalId) oder entfernt sie wie der
GroupWise-Client. Verschieben eines Zusatztermins verschiebt den Haupttermin um dieselbe Zeit,
Größenänderung ändert die Reisezeit, Löschen wird abgelehnt. Der Termin-Editor bekommt eine Registerkarte
*Reisezeit* mit dem roten Auto des GroupWise-Clients (Stunden und Minuten davor und danach, Minuten in
Schritten von 5, „Reisezeiten vor/nach Termin sind gleich“), nur bei GroupWise-Kalendern und nicht bei den
Zusatzterminen.

### Regeln

Die Registerkarte *Regeln* der *GroupWise-Einstellungen …* listet, bearbeitet, kopiert, entfernt, ordnet und
führt die Regeln des Postfachs aus (getRuleList, createItem Rule, modifyItem, removeItem, executeRule), im
Umfang des GroupWise-Clients: Ereignis (neues Objekt mit Quellen, abgelegtes Objekt, Ordner öffnen/schließen,
erledigtes Objekt, Starten, Beenden, benutzeraktiviert), Objekttypen, Terminkonflikte, Bedingungen mit
und/oder (eine Ebene) auf den Feldern des Clients (ermittelt mit dort angelegten Regeln: Datums- und
Zählervergleiche sind `field*`-Operatoren mit dem zweiten Operanden – `Today` oder ein Zähler wie
`totalUsers` – in `<date>` und einem Zuschlag in `<value>`; Felder ohne Schemanamen als Nummer, z. B. 1436
Erledigen bis, 119 In Auftrag gegeben am, 922 Abschlussdatum, 120 Jobkategorie als Zeichencode, 122
Jobpriorität, 532 Archivstatus von Drittanbietern; Aufzählungen als `bitCare`, manche doppelt mit leerer
`<mask/>`), Aktionen in ihrer Reihenfolge. Am POA ermittelt: ein neuer Filter löscht types und source (sie
gehen mit); `<update>` hängt Aktionen an (sie werden per delete und add ersetzt); die Reihenfolgenummern
zählt er nicht neu. `Reply` antwortet jedem Absender einmal am Tag und vermeidet Schleifen, `ReplyWithText`
(der Vorläufer) antwortet auf alles – als letzte Aktion „Unbedingt antworten (gefährlich)“ angeboten.
SimpleForward leitet im Text weiter, Forward als Anlage. Regeln, die der Editor nicht vollständig darstellen
kann (verschachtelte Gruppen, andere Felder oder Aktionen), zeigt er nur an. Regeln für neue und abgelegte
Objekte führt der POA selbst aus; die Client-Ereignisse nur auf executeRule: Evolution übernimmt Starten/Beenden
und Ordner öffnen/schließen, wenn die Kontooptionen (`run-startup-rules`, `run-folder-rules`) es wollen.

### Signaturen

Die Signaturen des Postfachs (getSignatures; je ein MIME-Dokument, wie der GroupWise-Client es schreibt)
sind Signaturen von Evolution, Kinder der Sammlung: das Registry-Modul holt sie bei jeder Anmeldung der
Sammlung (Inhalt als UTF-8-HTML mit den Bildern als data:-URIs, GroupWise-ID und Prüfsumme des
abgeglichenen Inhalts in der Erweiterung `GroupWise Folder`) und entfernt, was auf dem Server fehlt. Die
Standardsignatur von GroupWise wird zur Signatur der Identität, sobald sie sich in GroupWise geändert hat,
auch bei „vor dem Senden fragen“. Evolution schickt Änderungen zurück (ein Dateimonitor vergleicht den
Inhalt mit der Prüfsumme; Umbenennen; eine neue Signatur unter der Sammlung, angelegt in der Registerkarte
*Signaturen*; Entfernen, außer wenn die ganze Sammlung entfernt wird). Die Registerkarte *Signaturen* der
*GroupWise-Einstellungen …* legt an, bearbeitet (Evolutions Signatur-Editor), entfernt und setzt den
Standard.

### Namen der Systemordner

Die Systemordner stehen vorn, in der Reihenfolge des GroupWise-Clients (Mailbox, Ausgangsnachrichten,
Jobliste, In Arbeit, Junkmail, Papierkorb, Aktenschrank), über das Signal `compare-folders` des
Ordnerbaum-Modells; eine in Evolution eingestellte Reihenfolge hat Vorrang. Freigegebene Ordner, deren
übergeordneter Ordner fehlt, bleiben wie im GroupWise-Client ausgeblendet.

Die Systemordner tragen die Namen des GroupWise-Clients in der Sprache des Benutzers (deutsch:
Aktenschrank, Ausgangsnachrichten, In Arbeit, Jobliste, Junkmail, Papierkorb); ihre vollen Namen bleiben die des Servers.

Der HTML-Teil einer gesendeten Nachricht geht mit `<charset>UTF-8</charset>` und einem `<meta charset>`
im Kopf hinaus: ohne legt der POA ihn als US-ASCII ab, und der GroupWise-Client zeigt Umlaute als zwei
Zeichen.

### Suchergebnisordner

GroupWise-Suchergebnisordner (z. B. „Alle Mails“) sind Sichten auf Elemente anderer Ordner. Evolution
zeigt sie; Löschen und Verschieben wirken im Ordner, in dem eine Nachricht tatsächlich liegt, hinein
kann nichts verschoben werden. Abgeglichen wird höchstens alle 15 Minuten, offline werden sie nicht
vorgehalten.

**Grenze:** Über SOAP liefert der POA nur einen Teil der Treffer eines Suchergebnisordners (im Test
etwas über 4000 von 61552, die der GroupWise-Client zeigt), unabhängig von View, `count`, Filter oder
Protokollversion. Vollständige Suchen bieten Evolutions eigene Suchordner (*Bearbeiten → Suchordner*).

### Noch nicht unterstützt

Freigegebene Mailordner und Adressbücher, Schreiben in Kalender anderer Benutzer, signierte oder verschlüsselte Nachrichten (GroupWise baut die Nachricht für die Empfänger
selbst zusammen).

## Werkzeuge

`gw-cli` (installiert nach `lib/evolution-groupwise/`) spricht direkt mit einem POA, zum Ausprobieren
und zur Fehlersuche:

```sh
export GW_HOST=poa.firma.de GW_USER=benutzer      # Passwort: $GW_PASSWORD oder Abfrage
gw-cli --ssl login
gw-cli --ssl folders
gw-cli --ssl items ORDNER-ID 20
gw-cli --ssl mime ELEMENT-ID > nachricht.eml
gw-cli --ssl raw getTimestamp '<noop>true</noop>'
```

Im Build-Verzeichnis gibt es außerdem `gw-camel-check`, `gw-eds-check` und `gw-cal-check`, die über
Camel bzw. einen laufenden evolution-data-server gehen. Protokoll: `G_MESSAGES_DEBUG=evolution-groupwise`.

## Übersetzungen

Die Meldungen sind englisch; `po/` enthält die Übersetzungen (bisher Deutsch). `make update-po` im
Build-Verzeichnis aktualisiert die Vorlage `po/evolution-groupwise.pot` und führt sie in die
Übersetzungen zusammen; eine neue Sprache kommt als `po/<sprache>.po` dazu und wird in `po/LINGUAS`
eingetragen.

## Lizenz

GNU Lesser General Public License, Version 2.1 oder später (siehe `COPYING`), wie evolution-data-server.

Copyright © 2026 bond Software Entwicklung GmbH.
