// Wisp: the WebExtensions API (chrome.* and browser.*) for an extension's content scripts and pages.
// The browser injects this before the extension's own code, with the __WISP_*__ values filled in:
// in content scripts it runs in the extension's isolated world, in the extension's pages (background,
// popup, options) in the page. Calls go to the browser (ui/ext/extensions.cpp) through the "wispExt"
// script message handler, whose reply is the call's result; events come back through
// globalThis.__wispExt.dispatch(). Callback style and promise style both work, as in Chrome MV3.
(function () {
    "use strict";
    if (globalThis.__wispExt)
        return;
    const EXT_ID = "__WISP_EXT_ID__";
    const CONTEXT = "__WISP_CONTEXT__";          // "content" or "extension"
    const TAB_ID = __WISP_TAB_ID__;               // the tab a content script runs in (-1 elsewhere)
    const MANIFEST = __WISP_MANIFEST__;
    const MESSAGES = __WISP_MESSAGES__;
    const handler = globalThis.webkit && webkit.messageHandlers && webkit.messageHandlers.wispExt;
    const frameUrl = () => { try { return String(location.href); } catch (e) { return ""; } };
    const isTop = (() => { try { return window.top === window; } catch (e) { return false; } })();

    function clone(value) {
        return value === undefined ? null : JSON.parse(JSON.stringify(value));
    }
    function call(api, method, args) {
        if (!handler)
            return Promise.reject(new Error("OrbisWeb: no extension bridge here"));
        const message = { ext: EXT_ID, ctx: CONTEXT, tab: TAB_ID, url: frameUrl(), top: isTop, api, method,
            args: clone(args || []) };
        return handler.postMessage(message).then(result => {
            if (result && typeof result === "object" && "__wispError" in result)
                throw new Error(result.__wispError);
            return result;
        });
    }
    // chrome-style callbacks and lastError, or a promise when no callback is given
    const runtimeState = { lastError: undefined };
    function settle(promise, callback) {
        if (typeof callback !== "function")
            return promise;
        promise.then(result => {
            try { callback(result); } catch (e) { console.error(e); }
        }, error => {
            runtimeState.lastError = { message: String(error && error.message || error) };
            try { callback(); } catch (e) { console.error(e); }
            runtimeState.lastError = undefined;
        });
        return undefined;
    }
    function lastArgCallback(args) {
        return args.length && typeof args[args.length - 1] === "function" ? args.pop() : undefined;
    }
    function api(apiName, method, minArgs) {
        return function (...args) {
            const callback = lastArgCallback(args);
            return settle(call(apiName, method, args), callback);
        };
    }

    // --- events -----------------------------------------------------------------------------------
    const events = new Map();
    class WispEvent {
        constructor(name) { this.name = name; this.listeners = []; }
        addListener(listener, filter) {
            if (typeof listener !== "function" || this.listeners.includes(listener))
                return;
            this.listeners.push(listener);
            if (this.listeners.length === 1)
                call("events", "subscribe", [this.name, filter || null]).catch(() => {});
        }
        removeListener(listener) {
            this.listeners = this.listeners.filter(l => l !== listener);
            if (!this.listeners.length)
                call("events", "unsubscribe", [this.name]).catch(() => {});
        }
        hasListener(listener) { return this.listeners.includes(listener); }
        hasListeners() { return this.listeners.length > 0; }
        emit(args) {
            const results = [];
            for (const listener of this.listeners.slice()) {
                try { results.push(listener(...args)); } catch (e) { console.error(e); }
            }
            return results;
        }
    }
    function event(name) {
        if (!events.has(name))
            events.set(name, new WispEvent(name));
        return events.get(name);
    }

    // runtime.onMessage: a listener may answer with sendResponse (now, or later after returning
    // true) or by returning a promise
    function deliverMessage(eventName, message, sender, replyId) {
        const ev = events.get(eventName);
        if (!ev || !ev.listeners.length) {
            if (replyId)
                call("runtime", "respond", [replyId, null, false]).catch(() => {});
            return;
        }
        let answered = false, keep = false;
        const sendResponse = response => {
            if (answered || !replyId)
                return;
            answered = true;
            call("runtime", "respond", [replyId, response === undefined ? null : response, true]).catch(() => {});
        };
        for (const listener of ev.listeners.slice()) {
            let result;
            try { result = listener(message, sender, sendResponse); } catch (e) { console.error(e); continue; }
            if (result === true)
                keep = true;
            else if (result && typeof result.then === "function") {
                keep = true;
                result.then(sendResponse, e => sendResponse(undefined));
            }
        }
        if (!keep && !answered && replyId)
            call("runtime", "respond", [replyId, null, false]).catch(() => {});
    }

    // ports (runtime.connect / tabs.connect / onConnect)
    const ports = new Map();
    function makePort(portId, name, sender) {
        const onMessage = new WispEvent("port.onMessage"), onDisconnect = new WispEvent("port.onDisconnect");
        const port = {
            name: name || "", sender,
            onMessage: { addListener: l => onMessage.listeners.push(l), removeListener: l => { onMessage.listeners = onMessage.listeners.filter(x => x !== l); }, hasListener: l => onMessage.listeners.includes(l) },
            onDisconnect: { addListener: l => onDisconnect.listeners.push(l), removeListener: l => { onDisconnect.listeners = onDisconnect.listeners.filter(x => x !== l); }, hasListener: l => onDisconnect.listeners.includes(l) },
            postMessage(message) { call("runtime", "portPost", [portId, clone(message)]).catch(() => {}); },
            disconnect() { ports.delete(portId); call("runtime", "portClose", [portId]).catch(() => {}); },
            _message: onMessage, _disconnect: onDisconnect,
        };
        ports.set(portId, port);
        return port;
    }

    globalThis.__wispExt = {
        dispatch(name, args, extra) {
            if (name === "runtime.onMessage" || name === "runtime.onMessageExternal")
                return deliverMessage(name, args[0], args[1], extra);
            if (name === "runtime.onConnect") {
                const port = makePort(args[0], args[1], args[2]);
                event("runtime.onConnect").emit([port]);
                return;
            }
            if (name === "port.message") {
                const port = ports.get(args[0]);
                if (port)
                    port._message.emit([args[1], port]);
                return;
            }
            if (name === "port.disconnect") {
                const port = ports.get(args[0]);
                if (port) {
                    ports.delete(args[0]);
                    port._disconnect.emit([port]);
                }
                return;
            }
            const ev = events.get(name);
            if (ev)
                ev.emit(args || []);
        },
    };

    // --- i18n -------------------------------------------------------------------------------------
    function getMessage(name, substitutions) {
        const entry = MESSAGES[String(name).toLowerCase()];
        if (!entry)
            return name.startsWith("@@") ? (name === "@@extension_id" ? EXT_ID : name === "@@ui_locale" ? "en" : name === "@@bidi_dir" ? "ltr" : "") : "";
        let text = entry.message || "";
        const subs = substitutions === undefined ? [] : Array.isArray(substitutions) ? substitutions.map(String) : [String(substitutions)];
        if (entry.placeholders) {
            for (const [key, value] of Object.entries(entry.placeholders)) {
                const content = String(value.content || "").replace(/\$(\d)/g, (m, d) => subs[Number(d) - 1] ?? "");
                text = text.replace(new RegExp("\\$" + key + "\\$", "gi"), content);
            }
        }
        return text.replace(/\$(\d)/g, (m, d) => subs[Number(d) - 1] ?? "").replace(/\$\$/g, "$");
    }

    // --- the namespaces -----------------------------------------------------------------------------
    const getURL = path => `chrome-extension://${EXT_ID}/${String(path || "").replace(/^\/+/, "")}`;
    const runtime = {
        id: EXT_ID,
        getURL,
        getManifest: () => clone(MANIFEST),
        get lastError() { return runtimeState.lastError; },
        sendMessage(...args) {
            const callback = lastArgCallback(args);
            let extensionId = null, message, options;
            if (args.length >= 2 && (typeof args[0] === "string" || args[0] === null) && args.length !== 1)
                [extensionId, message, options] = args;
            else
                [message, options] = args;
            return settle(call("runtime", "sendMessage", [extensionId, clone(message)]), callback);
        },
        connect(...args) {
            const info = args.find(a => a && typeof a === "object") || {};
            const portId = "p" + Math.random().toString(36).slice(2) + Date.now().toString(36);
            const port = makePort(portId, info.name, null);
            call("runtime", "connect", [portId, info.name || ""]).catch(() => port.disconnect());
            return port;
        },
        getPlatformInfo: (...args) => settle(Promise.resolve({ os: "linux", arch: "x86-64", nacl_arch: "x86-64" }), lastArgCallback(args)),
        getBrowserInfo: () => Promise.resolve({ name: "OrbisWeb", vendor: "OrbisWeb", version: "1.0", buildID: "ps4" }),
        openOptionsPage: api("runtime", "openOptionsPage"),
        setUninstallURL: (...args) => settle(Promise.resolve(), lastArgCallback(args)),
        reload: () => call("runtime", "reload", []),
        requestUpdateCheck: (...args) => settle(Promise.resolve({ status: "no_update" }), lastArgCallback(args)),
        getContexts: api("runtime", "getContexts"),
        onMessage: event("runtime.onMessage"),
        onMessageExternal: event("runtime.onMessageExternal"),
        onConnect: event("runtime.onConnect"),
        onConnectExternal: event("runtime.onConnectExternal"),
        onInstalled: event("runtime.onInstalled"),
        onStartup: event("runtime.onStartup"),
        onSuspend: event("runtime.onSuspend"),
        onUpdateAvailable: event("runtime.onUpdateAvailable"),
    };

    function storageArea(area) {
        return {
            get: api("storage", "get." + area),
            set: api("storage", "set." + area),
            remove: api("storage", "remove." + area),
            clear: api("storage", "clear." + area),
            getBytesInUse: api("storage", "getBytesInUse." + area),
            setAccessLevel: (...args) => settle(Promise.resolve(), lastArgCallback(args)),
            onChanged: event("storage." + area + ".onChanged"),
            QUOTA_BYTES: area === "sync" ? 102400 : 10485760,
        };
    }
    const storage = {
        local: storageArea("local"), sync: storageArea("sync"), session: storageArea("session"),
        managed: storageArea("managed"), onChanged: event("storage.onChanged"),
    };

    const tabs = {
        query: api("tabs", "query"), get: api("tabs", "get"), create: api("tabs", "create"),
        update: api("tabs", "update"), remove: api("tabs", "remove"), reload: api("tabs", "reload"),
        duplicate: api("tabs", "duplicate"), goBack: api("tabs", "goBack"), goForward: api("tabs", "goForward"),
        getCurrent: (...args) => settle(TAB_ID >= 0 ? call("tabs", "get", [TAB_ID]) : Promise.resolve(undefined), lastArgCallback(args)),
        sendMessage(tabId, message, ...rest) {
            const callback = lastArgCallback(rest);
            const options = rest[0] || {};
            return settle(call("tabs", "sendMessage", [tabId, clone(message), options.frameId ?? null]), callback);
        },
        connect(tabId, info) {
            const portId = "p" + Math.random().toString(36).slice(2) + Date.now().toString(36);
            const port = makePort(portId, info && info.name, null);
            call("tabs", "connect", [tabId, portId, (info && info.name) || ""]).catch(() => port.disconnect());
            return port;
        },
        executeScript: api("tabs", "executeScript"), insertCSS: api("tabs", "insertCSS"), removeCSS: api("tabs", "removeCSS"),
        captureVisibleTab: (...args) => settle(Promise.reject(new Error("not supported")), lastArgCallback(args)),
        detectLanguage: (...args) => settle(Promise.resolve("und"), lastArgCallback(args)),
        setZoom: api("tabs", "setZoom"), getZoom: api("tabs", "getZoom"),
        onCreated: event("tabs.onCreated"), onUpdated: event("tabs.onUpdated"), onActivated: event("tabs.onActivated"),
        onRemoved: event("tabs.onRemoved"), onReplaced: event("tabs.onReplaced"), onHighlighted: event("tabs.onHighlighted"),
        onMoved: event("tabs.onMoved"), onAttached: event("tabs.onAttached"), onDetached: event("tabs.onDetached"),
        onZoomChange: event("tabs.onZoomChange"),
        TAB_ID_NONE: -1,
    };

    const windows = {
        WINDOW_ID_CURRENT: -2, WINDOW_ID_NONE: -1,
        get: api("windows", "get"), getCurrent: api("windows", "getCurrent"), getLastFocused: api("windows", "getLastFocused"),
        getAll: api("windows", "getAll"), create: api("windows", "create"), update: api("windows", "update"),
        remove: api("windows", "remove"),
        onCreated: event("windows.onCreated"), onRemoved: event("windows.onRemoved"), onFocusChanged: event("windows.onFocusChanged"),
    };

    const action = {
        setBadgeText: api("action", "setBadgeText"), getBadgeText: api("action", "getBadgeText"),
        setBadgeBackgroundColor: api("action", "setBadgeBackgroundColor"), getBadgeBackgroundColor: api("action", "getBadgeBackgroundColor"),
        setBadgeTextColor: api("action", "setBadgeTextColor"),
        setTitle: api("action", "setTitle"), getTitle: api("action", "getTitle"),
        setIcon: api("action", "setIcon"), setPopup: api("action", "setPopup"), getPopup: api("action", "getPopup"),
        enable: api("action", "enable"), disable: api("action", "disable"), isEnabledForTab: api("action", "isEnabled"),
        openPopup: api("action", "openPopup"), getUserSettings: () => Promise.resolve({ isOnToolbar: true }),
        onClicked: event("action.onClicked"),
    };

    // functions do not cross the bridge as JSON: their source does
    const withFunctionSource = method => (...args) => {
        const callback = lastArgCallback(args);
        const details = Object.assign({}, args[0] || {});
        for (const key of ["func", "function"]) {
            if (typeof details[key] === "function") {
                details.funcSource = String(details[key]);
                delete details[key];
            }
        }
        return settle(call("scripting", method, [details]), callback);
    };
    const scripting = {
        executeScript: withFunctionSource("executeScript"), insertCSS: api("scripting", "insertCSS"),
        removeCSS: api("scripting", "removeCSS"), registerContentScripts: api("scripting", "registerContentScripts"),
        unregisterContentScripts: api("scripting", "unregisterContentScripts"),
        getRegisteredContentScripts: api("scripting", "getRegisteredContentScripts"),
        updateContentScripts: api("scripting", "updateContentScripts"),
    };

    const declarativeNetRequest = {
        updateDynamicRules: api("dnr", "updateDynamicRules"), getDynamicRules: api("dnr", "getDynamicRules"),
        updateSessionRules: api("dnr", "updateSessionRules"), getSessionRules: api("dnr", "getSessionRules"),
        updateEnabledRulesets: api("dnr", "updateEnabledRulesets"), getEnabledRulesets: api("dnr", "getEnabledRulesets"),
        updateStaticRules: api("dnr", "updateStaticRules"), getDisabledRuleIds: api("dnr", "getDisabledRuleIds"),
        getAvailableStaticRuleCount: (...args) => settle(Promise.resolve(150000), lastArgCallback(args)),
        isRegexSupported: (...args) => settle(Promise.resolve({ isSupported: true }), lastArgCallback(args)),
        setExtensionActionOptions: (...args) => settle(Promise.resolve(), lastArgCallback(args)),
        getMatchedRules: (...args) => settle(Promise.resolve({ rulesMatchedInfo: [] }), lastArgCallback(args)),
        testMatchOutcome: (...args) => settle(Promise.resolve({ matchedRules: [] }), lastArgCallback(args)),
        onRuleMatchedDebug: event("dnr.onRuleMatchedDebug"),
        MAX_NUMBER_OF_DYNAMIC_RULES: 30000, MAX_NUMBER_OF_SESSION_RULES: 5000,
        MAX_NUMBER_OF_REGEX_RULES: 1000, MAX_NUMBER_OF_STATIC_RULESETS: 100, MAX_NUMBER_OF_ENABLED_STATIC_RULESETS: 50,
        GUARANTEED_MINIMUM_STATIC_RULES: 30000, DYNAMIC_RULESET_ID: "_dynamic", SESSION_RULESET_ID: "_session",
    };

    const alarms = {
        create: (...args) => { const cb = lastArgCallback(args); const name = typeof args[0] === "string" ? args.shift() : ""; return settle(call("alarms", "create", [name, args[0] || {}]), cb); },
        get: api("alarms", "get"), getAll: api("alarms", "getAll"), clear: api("alarms", "clear"),
        clearAll: api("alarms", "clearAll"), onAlarm: event("alarms.onAlarm"),
    };

    const menus = {
        create(properties, callback) { call("menus", "create", [properties]).then(() => callback && callback()).catch(() => {}); return properties && properties.id; },
        update: api("menus", "update"), remove: api("menus", "remove"), removeAll: api("menus", "removeAll"),
        onClicked: event("menus.onClicked"), ContextType: {}, ItemType: {},
    };

    const permissions = {
        contains: (...args) => settle(Promise.resolve(true), lastArgCallback(args)),
        getAll: (...args) => settle(Promise.resolve({ permissions: MANIFEST.permissions || [], origins: MANIFEST.host_permissions || [] }), lastArgCallback(args)),
        request: (...args) => settle(Promise.resolve(true), lastArgCallback(args)),
        remove: (...args) => settle(Promise.resolve(true), lastArgCallback(args)),
        onAdded: event("permissions.onAdded"), onRemoved: event("permissions.onRemoved"),
    };

    const chromeNamespace = {
        runtime, storage, tabs, windows, action, scripting, declarativeNetRequest, alarms, permissions,
        browserAction: action, pageAction: action, contextMenus: menus, menus,
        i18n: {
            getMessage, getUILanguage: () => "en-US",
            getAcceptLanguages: (...args) => settle(Promise.resolve(["en-US", "en"]), lastArgCallback(args)),
            detectLanguage: (...args) => settle(Promise.resolve({ isReliable: false, languages: [] }), lastArgCallback(args)),
        },
        extension: {
            getURL, inIncognitoContext: false, getBackgroundPage: () => null, getViews: () => [],
            isAllowedIncognitoAccess: (...args) => settle(Promise.resolve(false), lastArgCallback(args)),
            isAllowedFileSchemeAccess: (...args) => settle(Promise.resolve(false), lastArgCallback(args)),
            onRequest: event("extension.onRequest"), sendRequest: runtime.sendMessage,
        },
        notifications: {
            create: (...args) => { const cb = lastArgCallback(args); const id = typeof args[0] === "string" ? args.shift() : ""; return settle(call("notifications", "create", [id, args[0] || {}]), cb); },
            clear: (...args) => settle(Promise.resolve(true), lastArgCallback(args)),
            getAll: (...args) => settle(Promise.resolve({}), lastArgCallback(args)),
            onClicked: event("notifications.onClicked"), onClosed: event("notifications.onClosed"),
            onButtonClicked: event("notifications.onButtonClicked"),
        },
        webNavigation: {
            onBeforeNavigate: event("webNavigation.onBeforeNavigate"), onCommitted: event("webNavigation.onCommitted"),
            onDOMContentLoaded: event("webNavigation.onDOMContentLoaded"), onCompleted: event("webNavigation.onCompleted"),
            onErrorOccurred: event("webNavigation.onErrorOccurred"), onHistoryStateUpdated: event("webNavigation.onHistoryStateUpdated"),
            onReferenceFragmentUpdated: event("webNavigation.onReferenceFragmentUpdated"),
            onCreatedNavigationTarget: event("webNavigation.onCreatedNavigationTarget"),
            getFrame: api("webNavigation", "getFrame"), getAllFrames: api("webNavigation", "getAllFrames"),
        },
        webRequest: {
            onBeforeRequest: event("webRequest.onBeforeRequest"), onBeforeSendHeaders: event("webRequest.onBeforeSendHeaders"),
            onSendHeaders: event("webRequest.onSendHeaders"), onHeadersReceived: event("webRequest.onHeadersReceived"),
            onResponseStarted: event("webRequest.onResponseStarted"), onCompleted: event("webRequest.onCompleted"),
            onErrorOccurred: event("webRequest.onErrorOccurred"), onBeforeRedirect: event("webRequest.onBeforeRedirect"),
            onAuthRequired: event("webRequest.onAuthRequired"), handlerBehaviorChanged: (...args) => settle(Promise.resolve(), lastArgCallback(args)),
        },
        commands: { getAll: (...args) => settle(Promise.resolve([]), lastArgCallback(args)), onCommand: event("commands.onCommand") },
        management: {
            getSelf: (...args) => settle(Promise.resolve({ id: EXT_ID, name: MANIFEST.name, version: MANIFEST.version, enabled: true, installType: "normal" }), lastArgCallback(args)),
            getAll: (...args) => settle(Promise.resolve([]), lastArgCallback(args)),
        },
        cookies: { get: api("cookies", "get"), getAll: api("cookies", "getAll"), set: api("cookies", "set"), remove: api("cookies", "remove"), onChanged: event("cookies.onChanged") },
        privacy: { network: {}, services: {}, websites: {} },
        offscreen: { createDocument: api("offscreen", "createDocument"), closeDocument: api("offscreen", "closeDocument"), hasDocument: api("offscreen", "hasDocument") },
        sidePanel: { setOptions: (...a) => Promise.resolve(), setPanelBehavior: (...a) => Promise.resolve(), open: (...a) => Promise.resolve() },
        dom: { openOrClosedShadowRoot: element => element.openOrClosedShadowRoot ? element.openOrClosedShadowRoot() : element.shadowRoot },
    };

    // A content script sees the extension namespaces it may use there; extension pages all of them
    const visible = CONTEXT === "content"
        ? { runtime: { id: runtime.id, getURL, getManifest: runtime.getManifest, sendMessage: runtime.sendMessage, connect: runtime.connect,
                       onMessage: runtime.onMessage, onConnect: runtime.onConnect, get lastError() { return runtimeState.lastError; } },
            storage, i18n: chromeNamespace.i18n, extension: { getURL, inIncognitoContext: false }, dom: chromeNamespace.dom }
        : chromeNamespace;
    globalThis.chrome = Object.assign(globalThis.chrome || {}, visible);
    globalThis.browser = globalThis.chrome;
})();
