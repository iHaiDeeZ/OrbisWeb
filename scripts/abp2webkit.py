#!/usr/bin/env python3
"""Wisp: Adblock Plus filter lists (EasyList, EasyPrivacy, ...) to WebKit content-blocker rules.

WebKit compiles a JSON list of {trigger, action} rules to bytecode and applies it inside the engine
(WebKitUserContentFilterStore): blocking happens before a request leaves, element hiding is a style
sheet, nothing runs in the page. Its url-filter is a small regular-expression language: no
alternation (|), no backreferences, no lookarounds; a rule with several domains or types is several
rules or a list.

Converted:
  ||host^path, |http://..., plain substrings, * wildcards, ^ separators   -> url-filter
  $third-party / ~third-party                                              -> load-type
  $script $image $stylesheet $font $media $xmlhttprequest $subdocument
  $object $websocket $ping $other $document $popup                         -> resource-type
  $domain=a.com|~b.com                                                     -> if-domain / unless-domain
  $match-case                                                              -> url-filter-is-case-sensitive
  @@exceptions                                                             -> ignore-previous-rules (after the blocks)
  ##selector, domain.com##selector, ~domain.com##selector                  -> css-display-none
  domain.com#@#selector                                                    -> dropped (WebKit cannot un-hide)
Skipped (counted): $redirect/$rewrite/$csp/$removeparam/$header/$replace and other modifiers WebKit
cannot express, procedural/scriptlet cosmetics (#?#, #$#, ##+js, :has-text...), regex filters
(/.../), and rules past --max-rules (WebKit's limit is 150000 per list).

    python3 abp2webkit.py out.json list1.txt [list2.txt ...] [--max-rules N]
"""
import json
import re
import sys

RESOURCE_TYPES = {
    "script": ["script"], "image": ["image"], "stylesheet": ["style-sheet"], "font": ["font"],
    "media": ["media"], "xmlhttprequest": ["fetch"], "subdocument": ["document"], "document": ["document"],
    "object": ["other"], "object-subrequest": ["other"], "websocket": ["websocket"], "ping": ["ping"],
    "other": ["other"], "popup": ["popup"], "xhr": ["fetch"], "frame": ["document"], "css": ["style-sheet"],
}
ALL_TYPES = ["document", "image", "style-sheet", "script", "font", "raw", "svg-document", "media", "popup",
             "ping", "fetch", "websocket", "other"]
UNSUPPORTED = {"redirect", "redirect-rule", "rewrite", "csp", "removeparam", "header", "replace", "permissions",
               "urltransform", "uritransform", "jsonprune", "cookie", "method", "to", "denyallow", "1p", "3p",
               "strict1p", "strict3p", "badfilter", "important", "generichide", "elemhide", "specifichide",
               "genericblock", "inline-script", "inline-font", "empty", "mp4", "all", "first-party",
               "match-case-not", "sitekey", "webrtc", "rewrite"}
SEPARATOR = "[^a-zA-Z0-9_.%-]"

stats = {"network": 0, "exceptions": 0, "cosmetic": 0, "skipped": 0}


def pattern_to_regex(pattern):
    """An ABP address pattern to WebKit's url-filter regex, or None."""
    if pattern.startswith("/") and pattern.endswith("/") and len(pattern) > 2:
        return None  # a regex filter: often uses what WebKit lacks; skipped
    regex = ""
    start_anchor = end_anchor = False
    host_anchor = False
    if pattern.startswith("||"):
        host_anchor = True
        pattern = pattern[2:]
    elif pattern.startswith("|"):
        start_anchor = True
        pattern = pattern[1:]
    if pattern.endswith("|"):
        end_anchor = True
        pattern = pattern[:-1]
    for i, ch in enumerate(pattern):
        if ch == "*":
            regex += ".*"
        elif ch == "^":
            regex += SEPARATOR if i < len(pattern) - 1 else "(" + SEPARATOR + ".*)?$"
        elif ch in ".+?$(){}[]\\|/":
            regex += "\\" + ch if ch != "/" else "/"
        else:
            regex += ch
    if host_anchor:
        regex = "^[a-z][a-z0-9.+-]*://([^/:]*\\.)?" + regex
    elif start_anchor:
        regex = "^" + regex
    if end_anchor and not regex.endswith("$"):
        regex += "$"
    if regex in ("", ".*"):
        return None
    if not any(c.isalnum() for c in regex):
        return None
    return regex


def domain_list(value):
    yes, no = [], []
    for d in value.split("|"):
        d = d.strip().lower()
        if not d:
            continue
        (no if d.startswith("~") else yes).append("*" + d.lstrip("~"))
    return yes, no


def network_rule(line):
    exception = line.startswith("@@")
    if exception:
        line = line[2:]
    pattern, options = line, ""
    if "$" in line and not (line.startswith("/") and line.endswith("/")):
        pattern, options = line.rsplit("$", 1)
    trigger, types, not_types = {}, [], []
    for opt in [o.strip() for o in options.split(",") if o.strip()]:
        name, _, value = opt.partition("=")
        negated = name.startswith("~")
        name = name.lstrip("~").lower()
        if name in ("third-party", "3p"):
            trigger["load-type"] = ["first-party" if negated else "third-party"]
        elif name in ("first-party", "1p"):
            trigger["load-type"] = ["third-party" if negated else "first-party"]
        elif name == "domain":
            yes, no = domain_list(value)
            if yes:
                trigger["if-domain"] = yes
            elif no:
                trigger["unless-domain"] = no
        elif name == "match-case":
            trigger["url-filter-is-case-sensitive"] = True
        elif name in RESOURCE_TYPES:
            (not_types if negated else types).extend(RESOURCE_TYPES[name])
        elif name in UNSUPPORTED or name:
            return None
    regex = pattern_to_regex(pattern)
    if not regex:
        return None
    trigger["url-filter"] = regex
    if types:
        trigger["resource-type"] = sorted(set(types))
    elif not_types:
        trigger["resource-type"] = [t for t in ALL_TYPES if t not in set(not_types)]
    if "if-domain" in trigger and "unless-domain" in trigger:
        del trigger["unless-domain"]
    action = {"type": "ignore-previous-rules" if exception else "block"}
    return exception, {"trigger": trigger, "action": action}


def cosmetic_rule(line):
    if "#@#" in line or "#?#" in line or "#$#" in line or "#%#" in line or "##+js" in line or "##^" in line:
        return None
    domains, _, selector = line.partition("##")
    selector = selector.strip()
    if not selector or any(p in selector for p in (":has-text", ":-abp-", ":xpath", ":upward", ":remove",
                                                     ":style(", ":matches-css", ":min-text-length", ":watch-attr",
                                                     ":others", ":if(", ":nth-ancestor", ":matches-path")):
        return None
    trigger = {"url-filter": ".*"}
    if domains:
        yes, no = domain_list(domains.replace(",", "|"))
        if yes:
            trigger["if-domain"] = yes
        elif no:
            trigger["unless-domain"] = no
    return {"trigger": trigger, "action": {"type": "css-display-none", "selector": selector}}


def convert(paths, max_rules):
    blocks, exceptions, generic_hide, specific_hide = [], [], [], []
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            for raw in f:
                line = raw.strip()
                if not line or line.startswith("!") or line.startswith("["):
                    continue
                if "##" in line or "#@#" in line or "#?#" in line or "#$#" in line:
                    rule = cosmetic_rule(line)
                    if rule is None:
                        stats["skipped"] += 1
                        continue
                    stats["cosmetic"] += 1
                    (generic_hide if "if-domain" not in rule["trigger"] and "unless-domain" not in rule["trigger"]
                     else specific_hide).append(rule)
                    continue
                converted = network_rule(line)
                if converted is None:
                    stats["skipped"] += 1
                    continue
                exception, rule = converted
                if exception:
                    stats["exceptions"] += 1
                    exceptions.append(rule)
                else:
                    stats["network"] += 1
                    blocks.append(rule)
    # Generic element hiding as few rules: WebKit joins a rule's selector list into one style sheet;
    # chunks keep each selector list a reasonable size.
    hide = []
    selectors = [r["action"]["selector"] for r in generic_hide]
    for i in range(0, len(selectors), 500):
        hide.append({"trigger": {"url-filter": ".*"},
                     "action": {"type": "css-display-none", "selector": ", ".join(selectors[i:i + 500])}})
    rules = blocks + hide + specific_hide
    # exceptions last: ignore-previous-rules undoes what came before it
    room = max_rules - len(exceptions)
    rules = rules[:room] + exceptions
    return rules


def main():
    args = sys.argv[1:]
    max_rules = 140000
    if "--max-rules" in args:
        i = args.index("--max-rules")
        max_rules = int(args[i + 1])
        del args[i:i + 2]
    out, inputs = args[0], args[1:]
    rules = convert(inputs, max_rules)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(rules, f, separators=(",", ":"))
    print(f"abp2webkit: {len(rules)} rules ({stats['network']} blocks, {stats['exceptions']} exceptions, "
          f"{stats['cosmetic']} element-hiding; {stats['skipped']} skipped) -> {out}")


main()
