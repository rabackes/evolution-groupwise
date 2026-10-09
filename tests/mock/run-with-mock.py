#!/usr/bin/env python3
"""Runs a test program against the mock POA of mock_poa.py.

Usage: run-with-mock.py PROGRAM [ARGS...]

Starts two servers on free ports and passes them in the environment:
  GW_MOCK_PORT           the mock POA (user u1, password secret)
  GW_MOCK_REDIRECT_PORT  a POA that redirects every login to the first one
  GW_MOCK_TLS_PORT       the same mock POA behind TLS, its certificate signed by the CA in
                         GW_MOCK_CA (like a GroupWise system CA); plain
                         http gets a TLS alert followed by a 301 to https, like a real POA

Extensions over mock_poa.py for the libegroupwise tests:
  GET /attachment?session=..&id=..[&mime=1]  the streaming interface of the POA
  GET /_test/expire   the next SOAP call fails as if the POA had dropped the session (59910,
                      without description; with ?text=... that description instead)
  GET /_test/delay?action=A&seconds=S  delay the next A request (to cancel it in flight)
  GET /_test/logins   number of successful logins so far
  GET /_test/read?id=ID            "1" if the mail is marked read on the server
  GET /_test/set-read?id=ID&value=0|1
  GET /_test/hide?id=ID            the item disappears from all listings (deleted elsewhere)
  GET /_test/set-categories?id=ID&value=C1,C2   the item's categories set in GroupWise
  GET /_test/categories?id=ID      its categories (sorted, comma separated)
  GET /_test/category-names        the names of all categories (sorted)
  GET /_test/rules                 the rules ("sequence:name:enabled:execution:types:source:n actions;..")
  GET /_test/executed-rules        the rules executeRule ran
  GET /_test/rule-xml?id=ID        filter and actions of a rule as stored
  GET /_test/signatures            the signatures ("name:default:body of a text/plain MIME;..")
  GET /_test/access                the proxy access list ("email:group/right,..;..", by ID)
  GET /_test/junk-entries          the junk, block and trust lists ("list:match", sorted)
  GET /_test/junk-settings         the junk mail settings ("field=value", sorted)
  GET /_test/vacation              the item XML of the last out of office rule written
  GET /_test/set-subject?id=ID&value=TEXT   the user changed the subject in GroupWise
  GET /_test/new-draft             a draft appears in the mailbox
  GET /_test/new-invitation        an unanswered appointment INV1 appears in the mailbox
                                   (answered, the POA lists it no more: /_test/hide?id=...)
  getItems/cursors filter on @type (FilterEntry or FilterGroup "or")
  a search result folder "Alle Mails" (QALL@14) lists the Mailbox items under their Mailbox IDs;
  like a GroupWise 26.2 POA it refuses cursors (59916), getItems answers
  GET /_test/break-mime822         the Mime.822 attachment of MAIL2 cannot be streamed
  GET /_test/empty?n=N             the next N SOAP responses are empty documents
  GET /_test/calls?action=A        how many A requests came in
  GET /_test/reset                 the mailbox as at the start (counters and clock keep going)
  createCursor/readCursor/destroyCursor over getItems, with "modified gte" filters
  getQuickMessages (list Modified) with a server clock that advances with every change
  createItem (folders), modifyItem (folder name, parent), removeItem (folders): the folder
  list follows
  sendItem: a draft lands in the Mailbox (like a GroupWise 26.2 POA), a sent mail in Sent Items
  GET /_test/last-send             the item XML of the last sendItem
  GET /_test/last-send-session     the session it came in (PROXY9: a proxy login)
  moveItems, removeItems, purge: the listings follow (moved, copied, deleted items; Trash
  items name their containers, those they left with a "deleted" date; linking one back
  clears the date, moving it out of the Trash takes it out of every folder)
"""
import os
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import re
import urllib.parse
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape as xml_escape, unescape as xml_unescape
from http.server import BaseHTTPRequestHandler, HTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mock_poa  # noqa: E402

state = mock_poa.state
state["logins"] = 0
state["expire"] = False
state["hidden"] = set()
state["break822"] = False
state["expire_text"] = ""
state["delay"] = {}
state["modified"] = {}  # item ID -> modification time of changes made through the mock
state["clock"] = 0
state["counts"] = {}
state["out"], state["in"], state["purged"] = set(), {}, set()
state["new_folders"], state["folder_mods"], state["removed_folders"] = [], {}, set()
state["subjects"] = {}  # item ID -> subject changed in GroupWise
state["item_categories"] = {}  # base item ID -> category IDs as items name them


def default_categories():
    """The category list: the built-in ones (their IDs @61 in the list, @12 on items), own ones."""
    return [("1.domain1.po1.100.0.1.0.1@61", "Personal", "Personal", 16711680, False),
            ("2.domain1.po1.100.0.1.0.1@61", "Follow-up", "FollowUp", 33023, False),
            ("3.domain1.po1.100.0.1.0.1@61", "Urgent", "Urgent", 255, False),
            ("4.domain1.po1.100.0.1.0.1@61", "Low priority", "LowPriority", 12632256, False),
            ("K1.domain1.po1.100.0.1.0.1@12", "Kurs", "Normal", 10944422, False),
            ("H1.domain1.po1.100.0.1.0.1@12", "Versteckt", "Normal", None, True)]


state["categories"] = default_categories()


def default_junk():
    """The junk, block and trust lists: (id, match, matchType, listType)."""
    return [("JE1@66", "bob@phantom.com", "email", "trust"), ("JE2@66", "spam.example", "domain", "junk")]


def default_access():
    """Who may log in as proxy: [id, uuid, email, name, rights ("group/name")]."""
    return [["<All User Access>@60", "<All User Access>", "", "<All User Access>", {"appointment/read"}],
            ["U1@60", "U1", "anna.mueller@example.com", "M\u00fcller, Anna",
             {"mail/read", "mail/write", "appointment/read", "misc/setup"}]]


# The users resolveRequest knows: email -> (uuid, display name)
USERS = {"anna.mueller@example.com": ("U1", "M\u00fcller, Anna"), "max.weber@example.com": ("U2", "Weber, Max"),
         "bob@phantom.com": ("UB", "Bob")}

def default_signatures():
    """[id, name, default, base64 MIME]"""
    import base64
    mime = base64.b64encode(b"MIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nGr\xc3\xbc\xc3\x9fe\r\n").decode()
    return [["SIG1@63", "Kurz", True, mime]]


state["signatures"] = default_signatures()
state["signature_add"] = "Prompt"
def default_rules():
    """The rules as the POA keeps them: ID -> {field: XML text}, actions a list of <action> XML."""
    return {"R1@10": {"name": "Heise", "enabled": "1", "execution": "New", "sequence": "0", "types": "Mail",
                      "source": "received",
                      "filter": "<filter><element xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' xsi:type='FilterEntry'>"
                                "<op>contains</op><field>from</field><value>heise.de</value></element></filter>",
                      "actions": ["<action><type>Move</type><container>F1.domain1.po1.100.0.1.0.1@14</container></action>"]}}


RULE_FIELDS = ("name", "enabled", "execution", "sequence", "types", "source", "container", "conflict")

state["rules"] = default_rules()
state["executed_rules"] = []
state["access"] = default_access()
state["junk"] = default_junk()
state["vacation"] = None  # the body of the last createItem VacationRule
state["junk_settings"] = {"useJunkList": "1", "useBlockList": "1", "usePAB": "0", "persistence": "14", "autoDelete": "0"}


def reset():
    state["read"].update({mock_poa.MAIL1: True, mock_poa.MAIL2: False})
    state["out"] = set()        # (container, base ID): no longer in that container
    state["in"] = {}            # container -> {base ID: item element} added by moves, copies, removes
    state["purged"] = set()
    state.pop("last_send", None)
    state.pop("last_send_session", None)
    state["send_count"] = 0
    state["new_folders"] = []   # (id, name, parent)
    state["folder_mods"] = {}   # id -> {"name": .., "parent": ..}
    state["removed_folders"] = set()
    state["subjects"] = {}
    state["item_categories"] = {}
    state["categories"] = default_categories()
    state["junk"] = default_junk()
    state["access"] = default_access()
    state["rules"] = default_rules()
    state["executed_rules"] = []
    state["signatures"] = default_signatures()
    state["signature_add"] = "Prompt"
    state["vacation"] = None
    state["junk_settings"] = {"useJunkList": "1", "useBlockList": "1", "usePAB": "0", "persistence": "14", "autoDelete": "0"}
    state.pop("sent", None)
    for key, value in (("hidden", set()), ("modified", {}), ("delay", {}), ("break822", False),
                       ("expire", False), ("expire_text", ""), ("empty", 0)):
        state[key] = value


def server_now():
    return "2026-09-27T12:%02d:%02dZ" % divmod(state["clock"], 60)


def touch(item_id):
    """A change on the server: the item gets the current time, the clock moves on."""
    state["clock"] += 1
    state["modified"][item_id] = server_now()
    state["clock"] += 1
state["empty"] = 0
cursors = {}


def clean(text):
    return "".join((text or "").split())


def base_id(item_id):
    """The part of an item ID that stays the same in every container."""
    return clean(item_id).split(":", 1)[0]


def container_of(item_id):
    return clean(item_id).split(":", 1)[1]


def find_item(handler, container, base):
    # the mock POA wants a session in every request
    for xml in list_items(handler, f"<session>SESSION123</session><container>{container}</container>"):
        item = ET.fromstring(xml)
        if base_id(item.findtext("id")) == base:
            return item
    return None


def place(item, container, containers=None):
    """A copy of item as it appears in container."""
    import copy
    new = copy.deepcopy(item)
    new.find("id").text = base_id(item.findtext("id")) + ":" + container
    for c in new.findall("container"):
        new.remove(c)
    for name, deleted in (containers or [(container, None)]):
        c = ET.SubElement(new, "container")
        c.text = name
        if deleted:
            c.set("deleted", deleted)
    return new


def containers_holding(handler, base):
    return [c for c in [mock_poa.FOLDERS["inbox"], "F1.domain1.po1.100.0.1.0.1@14", "F2.domain1.po1.100.0.1.0.1@14",
                        "F3.domain1.po1.100.0.1.0.1@14"]
            if find_item(handler, c, base) is not None]


QUERY_FOLDER = "QALL@14"


def list_items(handler, body):
    """The items getItems would return, as XML strings, filtered like the POA does."""
    if f"<container>{QUERY_FOLDER}</container>" in body:  # a view onto the Mailbox
        return list_items(handler, body.replace(QUERY_FOLDER, mock_poa.FOLDERS["inbox"]))
    gte = re.search(r"<op>gte</op><field>modified</field><value>(.*?)</value>", body)
    types = re.findall(r"<field>@type</field><value>(.*?)</value>", body)
    out = mock_poa.Handler.dispatch(handler, "getItems", re.sub(r"<filter>.*</filter>", "", body, flags=re.S))
    root = ET.fromstring(out)
    container = re.search(r"<container>(.*?)</container>", body).group(1)
    listed = [i for i in root.iter("item")
              if (container, base_id(i.findtext("id"))) not in state["out"] and base_id(i.findtext("id")) not in state["purged"]]
    listed += [i for b, i in state["in"].get(container, {}).items()
               if (container, b) not in state["out"] and b not in state["purged"]]
    items = []
    for item in listed:
        if clean(item.findtext("id")) in state["hidden"]:
            continue
        if item.find("modified") is None:  # mock items have no modification time
            ET.SubElement(item, "modified").text = item.findtext("delivered") or item.findtext("created") or ""
        if clean(item.findtext("id")) in state["subjects"] and item.find("subject") is not None:
            # Like a GroupWise 26.2 POA: a view naming its fields gets the changed
            # subject only when it asks for originalSubject too, else the original
            view = (re.search(r"<view>(.*?)</view>", body) or [None, "default"])[1].split()
            if "default" in view or "originalSubject" in view:
                ET.SubElement(item, "originalSubject").text = item.findtext("subject")
                item.find("subject").text = state["subjects"][clean(item.findtext("id"))]
        cats = state["item_categories"].get(base_id(item.findtext("id")))
        if cats and item.find("categories") is None:
            c = ET.SubElement(item, "categories")
            for cat in cats:
                ET.SubElement(c, "category").text = cat
        if clean(item.findtext("id")) in state["modified"]:
            item.find("modified").text = state["modified"][clean(item.findtext("id"))]
        if gte and item.findtext("modified") < gte.group(1):
            continue
        if types and not any(item.get("type", "").endswith(t) for t in types):
            continue
        items.append(ET.tostring(item, encoding="unicode"))
    return items


# The original of MAIL2 as received from the Internet (hidden attachment SYS2, "Mime.822")
ORIGINAL = ("Received: from mx.phantom.com\r\nX-Original-MIME: yes\r\nMessage-ID: <orig-1@phantom.com>\r\nIn-Reply-To: <parent-1@phantom.com>\r\n"
            "From: Bob <bob@phantom.com>\r\nSubject: Test message\r\nContent-Type: text/plain\r\n\r\n"
            + mock_poa.BODY.replace("\n", "\r\n")).encode()

MIME = ("From: Bob <bob@phantom.com>\r\nTo: u1 <u1@phantom.com>\r\nSubject: Test message\r\n"
        "Date: Thu, 24 Sep 2026 08:30:00 +0000\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\n"
        "\r\n" + mock_poa.BODY.replace("\n", "\r\n")).encode()


class Handler(mock_poa.Handler):
    def dispatch(self, action, body):
        state["counts"][action] = state["counts"].get(action, 0) + 1
        if action in state["delay"]:
            import time
            time.sleep(state["delay"].pop(action))
        if state["empty"] > 0:
            state["empty"] -= 1
            return ""
        if action == "login":
            out = super().dispatch(action, body)
            if "<code>0</code>" in out:
                state["logins"] += 1
            return out
        if state["expire"]:
            state["expire"] = False
            return mock_poa.err(action, 59910, state["expire_text"])  # a GroupWise 26.2 POA sends no text
        if action == "getQuickMessages":
            since = re.search(r"<startDate>(.*?)</startDate>", body).group(1)
            assert "<list>Modified</list>" in body, body
            now = server_now()
            state["clock"] += 1
            items = [i for i in list_items(self, re.sub(r"<startDate>.*?</startDate>", "", body))
                     if ET.fromstring(i).findtext("modified") >= since]
            return mock_poa.ok(action, f"<startDate>{now}</startDate><items>" + "".join(items) + "</items>")
        if action in ("markRead", "markUnRead"):
            for mid in re.findall(r"<item>(.*?)</item>", body):
                touch(clean(mid))
        if action == "sendItem":
            item_xml = re.search(r"<item[ >].*</item>", body, re.S).group(0)
            state["last_send"] = item_xml
            session = re.search(r"<session>(.*?)</session>", body)
            state["last_send_session"] = session.group(1) if session else ""
            draft = "<source>draft</source>" in item_xml
            state["send_count"] = state.get("send_count", 0) + 1
            base = "SENT%d" % state["send_count"]
            container = mock_poa.FOLDERS["inbox"] if draft else "S.domain1.po1.100.0.1.0.1@30"
            subject = re.search(r"<subject>(.*?)</subject>", item_xml, re.S)
            item = ET.fromstring(f"<item type='Mail'><id>{base}@1:{container}</id><container>{container}</container>"
                                 f"<status><read>1</read></status><source>{'draft' if draft else 'sent'}</source>"
                                 f"<created>{server_now()}</created><subject></subject><size>100</size></item>")
            item.find("subject").text = ET.fromstring(f"<s>{subject.group(1)}</s>").text if subject else ""
            state["in"].setdefault(container, {})[base_id(item.findtext("id"))] = item
            touch(f"{base}@1:{container}")
            return mock_poa.ok(action, f"<id>{base}@1:{container}</id>")
        if action == "getFolderList":
            root = ET.fromstring(super().dispatch(action, body))
            folders = next(root.iter("folders"))
            folders.append(ET.fromstring(f"<folder type='QueryFolder'><id>{QUERY_FOLDER}</id><name>Alle Mails</name>"
                                         f"<parent>{mock_poa.FOLDERS['root']}</parent><folderType>Query</folderType></folder>"))
            for fid, name, parent in state["new_folders"]:
                folders.append(ET.fromstring(f"<folder type='Folder'><id>{fid}</id><name>{name}</name>"
                                             f"<parent>{parent}</parent><count>0</count></folder>"))
            parents = {f.findtext("id"): f.findtext("parent") for f in folders.findall("folder")}

            def removed(fid):
                while fid:
                    if fid in state["removed_folders"]:
                        return True
                    fid = parents.get(fid)
                return False
            for folder in list(folders.findall("folder")):
                fid = folder.findtext("id")
                if removed(fid):
                    folders.remove(folder)
                    continue
                for key, value in state["folder_mods"].get(fid, {}).items():
                    folder.find(key).text = value
            return ET.tostring(root, encoding="unicode")
        if action == "getItem":
            # like the POA: an item by its ID in any folder it is in
            item_id = re.search(r"<id>(.*?)</id>", body)
            if item_id and base_id(item_id.group(1)) == base_id(mock_poa.MAIL2) and clean(item_id.group(1)) != mock_poa.MAIL2:
                body = body.replace(item_id.group(1), mock_poa.MAIL2)
        if action == "getRuleList":
            rules = ""
            for rid, r in state["rules"].items():
                rules += "<rule xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' xsi:type='Rule'>" + f"<id>{rid}</id>"
                for f in RULE_FIELDS:
                    # like the POA: a rule switched off has no <enabled>
                    if r.get(f) and not (f == "enabled" and r[f] in ("0", "false")):
                        rules += f"<{f}>{xml_escape(r[f])}</{f}>"
                rules += r.get("filter", "") + "<actions>" + "".join(r["actions"]) + "</actions></rule>"
            v = state["vacation"]
            if v:
                # like the POA: the dates of a range of days as times too
                fields = "".join(re.findall(r"<(?:enabled|subject|replyToExternalUsers|myContactsOnly|externalSubject|"
                                            r"includeSenderMessage|startDay|endDay|startDate|endDate)>[^<]*</[a-zA-Z]+>"
                                            r"|<(?:message|externalMessage)><part>[^<]*</part></(?:message|externalMessage)>", v))
                days = re.search(r"<startDay>(.*?)</startDay><endDay>(.*?)</endDay>", v)
                if days:
                    fields += f"<startDate>{days.group(1)}T22:00:00Z</startDate><endDate>{days.group(2)}T21:59:59Z</endDate>"
                rules += ("<rule xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' xsi:type='VacationRule'>"
                          "<id>V1@85</id><name>Out of Office Rule</name>" + fields + "</rule>")
            return mock_poa.ok(action, "<rules>" + rules + "</rules>")
        if action == "createItem" and 'xsi:type="Rule"' in body:
            rid = "R%d@10" % (len(state["rules"]) + 10)
            r = {f: xml_unescape(m.group(1)) for f in RULE_FIELDS
                 for m in [re.search(f"<{f}>(.*?)</{f}>", body.split("<filter>")[0].split("<actions>")[0])] if m}
            r.setdefault("sequence", str(len(state["rules"])))
            flt = re.search(r"<filter>.*</filter>", body, re.S)
            r["filter"] = flt.group(0) if flt else ""
            acts = re.search(r"<actions>(.*)</actions>", body, re.S)
            r["actions"] = re.findall(r"<action>.*?</action>", acts.group(1), re.S) if acts else []
            state["rules"][rid] = r
            return mock_poa.ok(action, f"<id>{rid}</id>")
        if action == "modifyItem" and re.search(r"<id>R\d+@10</id>", body):
            r = state["rules"].get(re.search(r"<id>(.*?)</id>", body).group(1))
            if r is None:
                return mock_poa.err(action, 59906, "")
            if re.search(r"<delete>\s*<actions\s*/>\s*</delete>", body):
                r["actions"] = []
            for part in ("add", "update"):
                block = re.search(f"<{part}>(.*?)</{part}>", body, re.S)
                if not block:
                    continue
                block = block.group(1)
                flt = re.search(r"<filter>.*</filter>|<filter/>", block, re.S)
                if flt:
                    # like the POA: a new filter clears types and source
                    r["filter"] = "" if flt.group(0) == "<filter/>" else flt.group(0)
                    r.pop("types", None)
                    r.pop("source", None)
                    block = block.replace(flt.group(0), "")
                acts = re.search(r"<actions>(.*)</actions>", block, re.S)
                if acts:
                    # like the POA: actions are appended
                    r["actions"] += re.findall(r"<action>.*?</action>", acts.group(1), re.S)
                    block = block.replace(acts.group(0), "")
                for f in RULE_FIELDS:
                    m = re.search(f"<{f}>(.*?)</{f}>", block)
                    if m:
                        r[f] = xml_unescape(m.group(1))
            return mock_poa.ok(action)
        if action == "removeItem" and re.search(r"<id>R\d+@10</id>", body):
            state["rules"].pop(re.search(r"<id>(.*?)</id>", body).group(1), None)
            return mock_poa.ok(action)
        if action == "executeRule":
            state["executed_rules"].append(re.search(r"<id>(.*?)</id>", body).group(1))
            return mock_poa.ok(action)
        if action == "createItem" and 'xsi:type="VacationRule"' in body:
            state["vacation"] = body
            return mock_poa.ok(action, "<id>V1@85</id>")
        if action == "getTimezoneList":
            def zone(zid, desc, std_month):
                return (f"<timezone><id>{zid}</id><description>{desc}</description>"
                        "<daylight><name>Daylight</name><month>3</month><dayOfWeek occurrence='Last'>Sunday</dayOfWeek>"
                        "<hour>2</hour><minute>0</minute><offset>7200</offset></daylight>"
                        f"<standard><name>Standard</name><month>{std_month}</month><dayOfWeek occurrence='Last'>Sunday</dayOfWeek>"
                        "<hour>3</hour><minute>0</minute><offset>3600</offset></standard></timezone>")
            return mock_poa.ok(action, "<timezones><timezone><id>UTC</id><description>UTC</description>"
                               "<standard><name>UTC</name><offset>0</offset></standard></timezone>"
                               + zone("CET", "Warsaw", 9) + zone("WET", "Berlin", 10) + "</timezones>")
        if action == "getSignatures":
            return mock_poa.ok(action, "<signatures>" + "".join(
                f"<signature><id>{i}</id><name>{xml_escape(n)}</name><default>{int(d)}</default>"
                f"<part><data>{m}</data><size>{len(m)}</size></part><global>0</global></signature>"
                for i, n, d, m in state["signatures"])
                + f"<setting><enabled>1</enabled><add>{state['signature_add']}</add></setting></signatures>")
        if action in ("createSignature", "modifySignatures"):
            for sig in re.findall(r"<signature>(.*?)</signature>", body, re.S):
                sid = re.search(r"<id>(.*?)</id>", sig)
                if sid:
                    entry = next((e for e in state["signatures"] if e[0] == sid.group(1)), None)
                    if not entry:
                        return mock_poa.err(action, 59906, "")
                else:
                    entry = ["SIG%d@63" % (len(state["signatures"]) + 10), "", False, ""]
                    state["signatures"].append(entry)
                name = re.search(r"<name>(.*?)</name>", sig)
                if name:
                    entry[1] = xml_unescape(name.group(1))
                data = re.search(r"<data>(.*?)</data>", sig, re.S)
                if data:
                    entry[3] = data.group(1)
                default = re.search(r"<default>(.*?)</default>", sig)
                if default and default.group(1) in ("1", "true"):
                    for e in state["signatures"]:
                        e[2] = e is entry
                elif default:
                    entry[2] = False
            if action == "createSignature":
                return mock_poa.ok(action, f"<id>{entry[0]}</id>")
            return mock_poa.ok(action)
        if action == "removeSignature":
            sid = re.search(r"<id>(.*?)</id>", body).group(1)
            state["signatures"] = [e for e in state["signatures"] if e[0] != sid]
            return mock_poa.ok(action)
        if action == "getProxyAccessList":
            def rights_xml(rights):
                out = ""
                for group in ("appointment", "mail", "misc", "note", "task"):
                    names = sorted(r.split("/")[1] for r in rights if r.startswith(group + "/"))
                    out += f"<{group}>" + "".join(f"<{n}>1</{n}>" for n in names) + f"</{group}>"
                return out
            return mock_poa.ok(action, "<accessRights>" + "".join(
                f"<entry><displayName>{xml_escape(n)}</displayName>" + (f"<email>{e}</email>" if e else "")
                + f"<uuid>{xml_escape(u)}</uuid><id>{xml_escape(i)}</id>{rights_xml(r)}</entry>"
                for i, u, e, n, r in state["access"]) + "</accessRights>")
        if action == "resolve":
            out = ""
            for rec in re.findall(r"<recipient>(.*?)</recipient>", body, re.S):
                email = re.search(r"<email>(.*?)</email>", rec)
                user = USERS.get(email.group(1).lower()) if email else None
                out += "<recipient>" + (f"<displayName>{user[1]}</displayName><email>{email.group(1)}</email>"
                                        f"<uuid>{user[0]}</uuid>" if user else rec) + "</recipient>"
            return mock_poa.ok(action, f"<recipients>{out}</recipients>")
        if action == "createProxyAccess":
            uuid = re.search(r"<uuid>(.*?)</uuid>", body)
            if not uuid:  # like the POA: success, but nothing granted
                return mock_poa.ok(action)
            entry = [uuid.group(1) + "@60", uuid.group(1), re.search(r"<email>(.*?)</email>", body).group(1),
                     re.search(r"<displayName>(.*?)</displayName>", body).group(1), set()]
            for group, inner in re.findall(r"<(appointment|mail|misc|note|task)>(.*?)</\1>", body):
                entry[4].update(f"{group}/{n}" for n in re.findall(r"<(\w+)>1</\1>", inner))
            state["access"].append(entry)
            return mock_poa.ok(action, f"<id>{entry[0]}</id>")
        if action == "modifyProxyAccess":
            aid = xml_unescape(re.search(r"<id>(.*?)</id>", body).group(1))
            entry = next((e for e in state["access"] if e[0] == aid), None)
            if not entry:
                return mock_poa.err(action, 59905, "")
            for part, update in (("add", entry[4].update), ("delete", entry[4].difference_update)):
                block = re.search(f"<{part}>(.*?)</{part}>", body, re.S)
                for group, inner in re.findall(r"<(appointment|mail|misc|note|task)>(.*?)</\1>", block.group(1) if block else ""):
                    update({f"{group}/{n}" for n in re.findall(r"<(\w+)>1</\1>", inner)})
            return mock_poa.ok(action)
        if action == "removeProxyAccess":
            aid = xml_unescape(re.search(r"<id>(.*?)</id>", body).group(1))
            state["access"] = [e for e in state["access"] if e[0] != aid]
            return mock_poa.ok(action)
        if action == "getJunkEntries":
            out = ""
            for kind in ("junk", "block", "trust"):
                out += f"<{kind}>" + "".join(
                    f"<entry><id>{i}</id><match>{m}</match><matchType>{t}</matchType><listType>{k}</listType></entry>"
                    for i, m, t, k in state["junk"] if k == kind) + f"</{kind}>"
            return mock_poa.ok(action, out)
        if action == "createJunkEntry":
            jid = "JE%d@66" % (len(state["junk"]) + 10)
            state["junk"].append((jid, re.search(r"<match>(.*?)</match>", body).group(1),
                                  re.search(r"<matchType>(.*?)</matchType>", body).group(1),
                                  re.search(r"<listType>(.*?)</listType>", body).group(1)))
            return mock_poa.ok(action, f"<id>{jid}</id>")
        if action == "removeJunkEntry":
            jid = re.search(r"<id>(.*?)</id>", body).group(1)
            state["junk"] = [e for e in state["junk"] if e[0] != jid]
            return mock_poa.ok(action)
        if action == "getJunkMailSettings":
            return mock_poa.ok(action, "<settings>" + "".join(
                f"<setting><field>{f}</field><value>{v}</value></setting>" for f, v in state["junk_settings"].items()) + "</settings>")
        if action == "modifyJunkMailSettings":
            for f, v in re.findall(r"<setting><field>(.*?)</field><value>(.*?)</value></setting>", body):
                state["junk_settings"][f] = v
            return mock_poa.ok(action)
        if action == "getCategoryList":
            out = ""
            for cid, name, kind, color, hidden in state["categories"]:
                out += (f"<category><id>{cid}</id><name>{name}</name><type>{kind}</type>"
                        + (f"<color>{color}</color>" if color is not None else "")
                        + ("<flags><notInMasterList>1</notInMasterList></flags>" if hidden else "") + "</category>")
            return mock_poa.ok(action, "<categories>" + out + "</categories>")
        if action == "createItem" and 'xsi:type="Category"' in body:
            cid = "NEWCAT%d.domain1.po1.100.0.1.0.1@12" % len(state["categories"])
            color = re.search(r"<color>(.*?)</color>", body)
            state["categories"].append((cid, re.search(r"<name>(.*?)</name>", body).group(1), "Normal",
                                        int(color.group(1)) if color else None, False))
            return mock_poa.ok(action, f"<id>{cid}</id>")
        if action == "modifyItem" and "<categories>" in body:
            item_id = re.search(r"<id>(.*?)</id>", body).group(1)
            cats = state["item_categories"].setdefault(base_id(item_id), [])
            for part, block in re.findall(r"<(add|delete)><categories>(.*?)</categories></\1>", body, re.S):
                for cat in re.findall(r"<category>(.*?)</category>", block):
                    if part == "add" and cat not in cats:
                        cats.append(cat)
                    elif part == "delete" and cat in cats:
                        cats.remove(cat)
            touch(item_id)
            return mock_poa.ok(action, "<modified>2026-09-27T12:00:00Z</modified>")
        if action == "createItem" and 'xsi:type="Folder"' in body:
            fid = "NEWF%d@14" % (len(state["new_folders"]) + 1)
            state["new_folders"].append((fid, re.search(r"<name>(.*?)</name>", body).group(1),
                                         re.search(r"<parent>(.*?)</parent>", body).group(1)))
            return mock_poa.ok(action, f"<id>{fid}</id>")
        if action == "modifyItem" and re.search(r"<id>[^<]*@1[34]</id>", body):
            fid = re.search(r"<id>(.*?)</id>", body).group(1)
            for key in ("name", "parent"):
                value = re.search(rf"<update>.*?<{key}>(.*?)</{key}>.*?</update>", body, re.S)
                if value:
                    state["folder_mods"].setdefault(fid, {})[key] = value.group(1)
            return mock_poa.ok(action, "<modified>2026-09-27T12:00:00Z</modified>")
        if action == "removeItem" and re.search(r"<id>[^<]*@1[34]</id>", body) and "<container>" not in body:
            fid = re.search(r"<id>(.*?)</id>", body).group(1)
            trash = mock_poa.FOLDERS["trash"]
            # Its items go to the Trash; like the POA, the folder that is gone shows
            # up there as the Trash itself
            for xml in list_items(self, f"<session>SESSION123</session><container>{fid}</container>"):
                item = ET.fromstring(xml)
                base = base_id(item.findtext("id"))
                holding = [(c, None) for c in containers_holding(self, base)] + [(trash, server_now())]
                state["in"].setdefault(trash, {})[base] = place(item, trash, holding)
                state["out"].discard((trash, base))
                state["out"].add((fid, base))
            state["removed_folders"].add(fid)
            return mock_poa.ok(action)
        if action == "moveItems":
            for block in re.findall(r"<item>(.*?)</item>", body, re.S):
                item_id = re.search(r"<id>(.*?)</id>", block).group(1)
                dest = re.search(r"<container>(.*?)</container>", block).group(1)
                source = re.search(r"<from>(.*?)</from>", block)
                base = base_id(item_id)
                item = find_item(self, container_of(item_id), base)
                if item is None:
                    return mock_poa.err(action, 59905, "")
                trash = mock_poa.FOLDERS["trash"]
                if container_of(item_id) == trash:
                    # Like the POA, the Trash is a view: out of it with a source, the item
                    # leaves every folder; linked into a container it was deleted from,
                    # that one is live again, and with none left deleted it leaves the Trash
                    if source:
                        for c in containers_holding(self, base):
                            state["out"].add((c, base))
                        state["out"].add((trash, base))
                    else:
                        entry = state["in"].get(trash, {}).get(base)
                        if entry is not None:
                            for c in entry.findall("container"):
                                if c.text == dest:
                                    c.attrib.pop("deleted", None)
                            if not any(c.get("deleted") for c in entry.findall("container")):
                                state["out"].add((trash, base))
                    source = None
                state["out"].discard((dest, base))
                state["in"].setdefault(dest, {})[base] = place(item, dest)
                if source:
                    state["out"].add((source.group(1), base))
                touch(base + ":" + dest)
            return mock_poa.ok(action)
        if action == "removeItems":
            source = re.search(r"<container>(.*?)</container>", body).group(1)
            trash = mock_poa.FOLDERS["trash"]
            for item_id in re.findall(r"<item>(.*?)</item>", body):
                base = base_id(item_id)
                item = find_item(self, source, base)
                if item is None:
                    return mock_poa.err(action, 59905, "")
                state["out"].add((source, base))
                if source == trash:
                    continue  # out of the Trash only, in its folders it stays
                # like the POA: the Trash entry lists every container, the left ones with a date
                holding = [(c, None) for c in containers_holding(self, base)] + [(source, server_now())]
                state["in"].setdefault(trash, {})[base] = place(item, trash, holding)
                state["out"].discard((trash, base))
            return mock_poa.ok(action)
        if action == "purge":
            for item_id in re.findall(r"<item>(.*?)</item>", body):
                state["purged"].add(base_id(item_id))  # gone from every folder
            return mock_poa.ok(action)
        if action == "createCursor" and f"<container>{QUERY_FOLDER}</container>" in body:
            return mock_poa.err(action, 59916, "")  # no cursors on search result folders
        if action == "getItems" and f"<container>{QUERY_FOLDER}</container>" in body:
            return mock_poa.ok(action, "<items>" + "".join(list_items(self, body)) + "</items>")
        if action == "createCursor" and "<container>TW@86</container>" in body:
            return mock_poa.ok(action)  # like the TeamWorks container: no cursor, no items
        if action == "createCursor":
            cid = str(len(cursors) + 1)
            cursors[cid] = list_items(self, body)
            return mock_poa.ok(action, f"<cursor>{cid}</cursor>")
        if action == "readCursor":
            cid = re.search(r"<cursor>(.*?)</cursor>", body).group(1)
            count = int(re.search(r"<count>(.*?)</count>", body).group(1))
            page, cursors[cid] = cursors[cid][:count], cursors[cid][count:]
            return mock_poa.ok(action, "<items>" + "".join(page) + "</items>")
        if action == "destroyCursor":
            cursors.pop(re.search(r"<cursor>(.*?)</cursor>", body).group(1), None)
            return mock_poa.ok(action)
        return super().dispatch(action, body)

    def reply(self, code, data, headers=()):
        self.send_response(code)
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        query = dict(urllib.parse.parse_qsl(url.query))
        if url.path == "/_test/delay":
            state["delay"][query["action"]] = float(query["seconds"])
            return self.reply(200, b"ok")
        if url.path == "/_test/expire":
            state["expire"] = True
            state["expire_text"] = query.get("text", "")
            return self.reply(200, b"ok")
        if url.path == "/_test/logins":
            return self.reply(200, str(state["logins"]).encode())
        if url.path == "/_test/read":
            return self.reply(200, b"1" if state["read"].get(query["id"]) else b"0")
        if url.path == "/_test/last-send-session":
            return self.reply(200, state.get("last_send_session", "").encode())
        if url.path == "/_test/last-send":
            return self.reply(200, state.get("last_send", "").encode())
        if url.path == "/_test/reset":
            reset()
            return self.reply(200, b"ok")
        if url.path == "/_test/calls":
            return self.reply(200, str(state["counts"].get(query["action"], 0)).encode())
        if url.path == "/_test/set-read":
            state["read"][query["id"]] = query["value"] == "1"
            touch(query["id"])
            return self.reply(200, b"ok")
        if url.path == "/_test/set-subject":
            state["subjects"][query["id"]] = query["value"]
            touch(query["id"])
            return self.reply(200, b"ok")
        if url.path == "/_test/set-categories":
            state["item_categories"][base_id(query["id"])] = [c for c in query.get("value", "").split(",") if c]
            touch(query["id"])
            return self.reply(200, b"ok")
        if url.path == "/_test/categories":
            return self.reply(200, ",".join(sorted(state["item_categories"].get(base_id(query["id"]), []))).encode())
        if url.path == "/_test/category-names":
            return self.reply(200, ",".join(sorted(n for _, n, _, _, _ in state["categories"])).encode())
        if url.path == "/_test/rules":
            return self.reply(200, ";".join(
                f"{r.get('sequence')}:{r.get('name')}:{int(r.get('enabled', '0') in ('1', 'true'))}:{r.get('execution')}:{r.get('types', '')}:"
                f"{r.get('source', '')}:{len(r['actions'])}" for _, r in sorted(state["rules"].items(),
                                                                                 key=lambda kv: int(kv[1].get('sequence', 0)))).encode())
        if url.path == "/_test/executed-rules":
            return self.reply(200, ",".join(state["executed_rules"]).encode())
        if url.path == "/_test/rule-xml":
            query_id = query.get("id", "")
            r = state["rules"].get(query_id, {})
            return self.reply(200, (r.get("filter", "") + "".join(r.get("actions", []))).encode())
        if url.path == "/_test/signatures":
            import base64
            return self.reply(200, ";".join(f"{n}:{int(d)}:" + base64.b64decode(m).decode("utf-8", "replace")
                                            .split("\r\n\r\n", 1)[-1].strip()
                                            for _, n, d, m in state["signatures"]).encode())
        if url.path == "/_test/access":
            return self.reply(200, ";".join(f"{e[2] or e[1]}:" + ",".join(sorted(e[4]))
                                            for e in sorted(state["access"], key=lambda e: e[0])).encode())
        if url.path == "/_test/junk-entries":
            return self.reply(200, ",".join(sorted(f"{k}:{m}" for _, m, _, k in state["junk"])).encode())
        if url.path == "/_test/vacation":
            return self.reply(200, (state["vacation"] or "").encode())
        if url.path == "/_test/junk-settings":
            return self.reply(200, ",".join(f"{f}={v}" for f, v in sorted(state["junk_settings"].items())).encode())
        if url.path == "/_test/hide":
            state["hidden"].add(query["id"])
            return self.reply(200, b"ok")
        if url.path == "/_test/new-draft":
            state["sent"] = "x"
            touch("DRAFT1")
            return self.reply(200, b"ok")
        if url.path == "/_test/new-invitation":
            inbox = mock_poa.FOLDERS["inbox"]
            item = ET.fromstring(f"<item type='Appointment'><id>INV1@4:{inbox}</id><container>{inbox}</container>"
                                 f"<source>received</source><created>{server_now()}</created>"
                                 f"<delivered>{server_now()}</delivered><subject>Besprechung</subject><size>100</size>"
                                 f"<startDate>2026-10-01T08:00:00Z</startDate><endDate>2026-10-01T09:00:00Z</endDate></item>")
            state["in"].setdefault(inbox, {})["INV1@4"] = item
            touch(f"INV1@4:{inbox}")
            return self.reply(200, b"ok")
        if url.path == "/_test/break-mime822":
            state["break822"] = True
            return self.reply(200, b"ok")
        if url.path == "/_test/empty":
            state["empty"] = int(query["n"])
            return self.reply(200, b"ok")
        if url.path != "/attachment":
            return self.reply(404, b"")
        if state["expire"] or query.get("session") != "SESSION123":
            state["expire"] = False
            return self.reply(400, b"", [("X-GWError-Code", "0xEA06")])  # invalid session
        if base_id(query.get("id", "")) == base_id(mock_poa.MAIL2) and query.get("mime") == "1":
            return self.reply(200, MIME, [("Content-Type", "message/rfc822")])
        if query.get("id") == "SYS2" and not state["break822"]:
            return self.reply(200, ORIGINAL, [("Content-Type", "text/plain")])
        if query.get("id") == mock_poa.ATT:
            return self.reply(200, b"attachment!", [("Content-Type", "text/plain")])
        # Real POAs send the code in hex: 0xEA02 = 59906, item not found
        return self.reply(400, b"", [("X-GWError-Code", "0xEA02")])


def redirect_handler(target_port):
    class Redirect(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_POST(self):
            self.rfile.read(int(self.headers.get("Content-Length", 0)))
            data = ("<?xml version='1.0'?><SOAP-ENV:Envelope xmlns:SOAP-ENV='http://schemas.xmlsoap.org/soap/envelope/'>"
                    "<SOAP-ENV:Body><loginResponse><redirectToHost><ipAddress>127.0.0.1</ipAddress>"
                    f"<port>{target_port}</port></redirectToHost><status><code>59923</code>"
                    "<description>Redirect user to different PO.</description></status></loginResponse>"
                    "</SOAP-ENV:Body></SOAP-ENV:Envelope>").encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/xml")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
    return Redirect


class TlsOnlyServer(HTTPServer):
    """Serves TLS; answers plain http the way a POA with SSL required does."""
    context: ssl.SSLContext

    def get_request(self):
        sock, addr = super().get_request()
        first = sock.recv(1, socket.MSG_PEEK)
        if first == b"\x16":  # TLS handshake record
            return self.context.wrap_socket(sock, server_side=True), addr
        sock.recv(65536)
        host = f"127.0.0.1:{self.server_address[1]}"
        sock.sendall(b"\x15\x03\x03\x00\x02\x02\x16HTTP/1.1 301 Moved Permanently\r\n"
                     + f"Location: https://{host}/soap\r\n\r\n<html>Moved</html>".encode())
        sock.close()
        raise OSError("plain http on the TLS port")


def tls_context(directory):
    """Like a GroupWise system: an own CA (ca.pem, for GW_MOCK_CA) signs the server certificate."""
    def run(*args):
        subprocess.run(args, check=True, capture_output=True, cwd=directory)
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/O=MOCKGW/CN=MOCKGW-CA",
        "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign",
        "-keyout", "ca.key", "-out", "ca.pem")
    run("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-subj", "/O=mockgw/OU=POA/CN=127.0.0.1",
        "-keyout", "key.pem", "-out", "server.csr")
    with open(os.path.join(directory, "ext.cnf"), "w") as ext:
        ext.write("subjectAltName=IP:127.0.0.1,DNS:localhost\n")
    run("openssl", "x509", "-req", "-in", "server.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial",
        "-days", "1", "-extfile", "ext.cnf", "-out", "cert.pem")
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(os.path.join(directory, "cert.pem"), os.path.join(directory, "key.pem"))
    return context


def serve(handler, server_class=HTTPServer, context=None):
    server = server_class(("127.0.0.1", 0), handler)
    if context:
        server.context = context
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server.server_address[1]


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    port = serve(Handler)
    with tempfile.TemporaryDirectory() as directory:
        tls_port = serve(Handler, TlsOnlyServer, tls_context(directory))
        env = dict(os.environ, GW_MOCK_PORT=str(port), GW_MOCK_REDIRECT_PORT=str(serve(redirect_handler(port))),
                   GW_MOCK_TLS_PORT=str(tls_port), GW_MOCK_CA=os.path.join(directory, "ca.pem"))
        return subprocess.call(sys.argv[1:], env=env)


if __name__ == "__main__":
    sys.exit(main())
