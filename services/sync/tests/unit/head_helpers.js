/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

/* import-globals-from head_appinfo.js */
/* import-globals-from ../../../common/tests/unit/head_helpers.js */
/* import-globals-from head_errorhandler_common.js */
/* import-globals-from head_http_server.js */

// This file expects Service to be defined in the global scope when EHTestsCommon
// is used (from service.js).
/* global Service */

var { AddonTestUtils, MockAsyncShutdown } = ChromeUtils.import(
  "resource://testing-common/AddonTestUtils.jsm"
);
var { Async } = ChromeUtils.import("resource://services-common/async.js");
var { CommonUtils } = ChromeUtils.import("resource://services-common/utils.js");
var { PlacesTestUtils } = ChromeUtils.import(
  "resource://testing-common/PlacesTestUtils.jsm"
);
var { sinon } = ChromeUtils.import("resource://testing-common/Sinon.jsm");
var { SerializableSet, Svc, Utils, getChromeWindow } = ChromeUtils.import(
  "resource://services-sync/util.js"
);
var { XPCOMUtils } = ChromeUtils.import(
  "resource://gre/modules/XPCOMUtils.jsm"
);
var { PlacesUtils } = ChromeUtils.import(
  "resource://gre/modules/PlacesUtils.jsm"
);
var { PlacesSyncUtils } = ChromeUtils.import(
  "resource://gre/modules/PlacesSyncUtils.jsm"
);
var { ObjectUtils } = ChromeUtils.import(
  "resource://gre/modules/ObjectUtils.jsm"
);
var {
  AccountState,
  MockFxaStorageManager,
  SyncTestingInfrastructure,
  configureFxAccountIdentity,
  configureIdentity,
  encryptPayload,
  makeFxAccountsInternalMock,
  makeIdentityConfig,
  promiseNamedTimer,
  promiseZeroTimer,
  syncTestLogging,
  waitForZeroTimer,
} = ChromeUtils.import("resource://testing-common/services/sync/utils.js");
ChromeUtils.defineModuleGetter(
  this,
  "AddonManager",
  "resource://gre/modules/AddonManager.jsm"
);

add_task(async function head_setup() {
  // Initialize logging. This will sometimes be reset by a pref reset,
  // so it's also called as part of SyncTestingInfrastructure().
  syncTestLogging();
  // If a test imports Service, make sure it is initialized first.
  if (typeof Service !== "undefined") {
    await Service.promiseInitialized;
  }
});

// This is needed for loadAddonTestFunctions().
var gGlobalScope = this;

function ExtensionsTestPath(path) {
  if (path[0] != "/") {
    throw Error("Path must begin with '/': " + path);
  }

  return "../../../../toolkit/mozapps/extensions/test/xpcshell" + path;
}

function webExtensionsTestPath(path) {
  if (path[0] != "/") {
    throw Error("Path must begin with '/': " + path);
  }

  return "../../../../toolkit/components/extensions/test/xpcshell" + path;
}

/**
 * Loads the WebExtension test functions by importing its test file.
 */
function loadWebExtensionTestFunctions() {
  /* import-globals-from ../../../../toolkit/components/extensions/test/xpcshell/head_sync.js */
  const path = webExtensionsTestPath("/head_sync.js");
  let file = do_get_file(path);
  let uri = Services.io.newFileURI(file);
  Services.scriptloader.loadSubScript(uri.spec, gGlobalScope);
}

/**
 * Installs an add-on from an addonInstall
 *
 * @param  install addonInstall instance to install
 */
async function installAddonFromInstall(install) {
  await install.install();

  Assert.notEqual(null, install.addon);
  Assert.notEqual(null, install.addon.syncGUID);

  return install.addon;
}

/**
 * Convenience function to install an add-on from the extensions unit tests.
 *
 * @param  file
 *         Add-on file to install.
 * @param  reconciler
 *         addons reconciler, if passed we will wait on the events to be
 *         processed before resolving
 * @return addon object that was installed
 */
async function installAddon(file, reconciler = null) {
  let install = await AddonManager.getInstallForFile(file);
  Assert.notEqual(null, install);
  const addon = await installAddonFromInstall(install);
  if (reconciler) {
    await reconciler.queueCaller.promiseCallsComplete();
  }
  return addon;
}

/**
 * Convenience function to uninstall an add-on.
 *
 * @param addon
 *        Addon instance to uninstall
 * @param reconciler
 *        addons reconciler, if passed we will wait on the events to be
 *        processed before resolving
 */
async function uninstallAddon(addon, reconciler = null) {
  const uninstallPromise = new Promise(res => {
    let listener = {
      onUninstalled(uninstalled) {
        if (uninstalled.id == addon.id) {
          AddonManager.removeAddonListener(listener);
          res(uninstalled);
        }
      },
    };
    AddonManager.addAddonListener(listener);
  });
  addon.uninstall();
  await uninstallPromise;
  if (reconciler) {
    await reconciler.queueCaller.promiseCallsComplete();
  }
}

async function generateNewKeys(collectionKeys, collections = null) {
  let wbo = await collectionKeys.generateNewKeysWBO(collections);
  let modified = new_timestamp();
  collectionKeys.setContents(wbo.cleartext, modified);
}

// Helpers for testing open tabs.
// These reflect part of the internal structure of TabEngine,
// and stub part of Service.wm.

function mockShouldSkipWindow(win) {
  return win.closed || win.mockIsPrivate;
}

function mockGetTabState(tab) {
  return tab;
}

function mockGetWindowEnumerator(url, numWindows, numTabs, indexes, moreURLs) {
  let elements = [];

  function url2entry(urlToConvert) {
    return {
      url: typeof urlToConvert == "function" ? urlToConvert() : urlToConvert,
      title: "title",
    };
  }

  for (let w = 0; w < numWindows; ++w) {
    let tabs = [];
    let win = {
      closed: false,
      mockIsPrivate: false,
      gBrowser: {
        tabs,
      },
    };
    elements.push(win);

    for (let t = 0; t < numTabs; ++t) {
      tabs.push(
        Cu.cloneInto(
          {
            index: indexes ? indexes() : 1,
            entries: (moreURLs ? [url].concat(moreURLs()) : [url]).map(
              url2entry
            ),
            attributes: {
              image: "image",
            },
            lastAccessed: 1499,
          },
          {}
        )
      );
    }
  }

  // Always include a closed window and a private window.
  elements.push({
    closed: true,
    mockIsPrivate: false,
    gBrowser: {
      tabs: [],
    },
  });

  elements.push({
    closed: false,
    mockIsPrivate: true,
    gBrowser: {
      tabs: [],
    },
  });

  return elements.values();
}

// Used for the (many) cases where we do a 'partial' sync, where only a single
// engine is actually synced, but we still want to ensure we're generating a
// valid ping. Returns a promise that resolves to the ping, or rejects with the
// thrown error after calling an optional callback.
async function sync_engine(
  engine,
  onError
) {
  let caughtError = null;

  // neuter the scheduler as it interacts badly with some of the tests - the
  // engine being synced usually isn't the registered engine, so we see
  // scored incremented and not removed, which schedules unexpected syncs.
  let oldObserve = Service.scheduler.observe;
  Service.scheduler.observe = () => {};
  try {
    Svc.Obs.notify("weave:service:sync:start");
    try {
      await engine.sync();
    } catch (e) {
      caughtError = e;
    }
    if (caughtError) {
      Svc.Obs.notify("weave:service:sync:error", caughtError);
    } else {
      Svc.Obs.notify("weave:service:sync:finish");
    }
  } finally {
    Service.scheduler.observe = oldObserve;
  }
  return submitPromise;
}

// Returns a promise that resolves once the specified observer notification
// has fired.
function promiseOneObserver(topic, callback) {
  return new Promise((resolve, reject) => {
    let observer = function(subject, data) {
      Svc.Obs.remove(topic, observer);
      resolve({ subject, data });
    };
    Svc.Obs.add(topic, observer);
  });
}

// Avoid an issue where `client.name2` containing unicode characters causes
// a number of tests to fail, due to them assuming that we do not need to utf-8
// encode or decode data sent through the mocked server (see bug 1268912).
// We stash away the original implementation so test_utils_misc.js can test it.
Utils._orig_getDefaultDeviceName = Utils.getDefaultDeviceName;
Utils.getDefaultDeviceName = function() {
  return "Test device name";
};

async function registerRotaryEngine() {
  let { RotaryEngine } = ChromeUtils.import(
    "resource://testing-common/services/sync/rotaryengine.js"
  );
  await Service.engineManager.clear();

  await Service.engineManager.register(RotaryEngine);
  let engine = Service.engineManager.get("rotary");
  let syncID = await engine.resetLocalSyncID();
  engine.enabled = true;

  return { engine, syncID, tracker: engine._tracker };
}

// Set the validation prefs to attempt validation every time to avoid non-determinism.
function enableValidationPrefs(engines = ["bookmarks"]) {
  for (let engine of engines) {
    Svc.Prefs.set(`engine.${engine}.validation.interval`, 0);
    Svc.Prefs.set(`engine.${engine}.validation.percentageChance`, 100);
    Svc.Prefs.set(`engine.${engine}.validation.maxRecords`, -1);
    Svc.Prefs.set(`engine.${engine}.validation.enabled`, true);
  }
}

async function serverForEnginesWithKeys(users, engines, callback) {
  // Generate and store a fake default key bundle to avoid resetting the client
  // before the first sync.
  let wbo = await Service.collectionKeys.generateNewKeysWBO();
  let modified = new_timestamp();
  Service.collectionKeys.setContents(wbo.cleartext, modified);

  let allEngines = [Service.clientsEngine].concat(engines);

  let globalEngines = {};
  for (let engine of allEngines) {
    let syncID = await engine.resetLocalSyncID();
    globalEngines[engine.name] = { version: engine.version, syncID };
  }

  let contents = {
    meta: {
      global: {
        syncID: Service.syncID,
        storageVersion: STORAGE_VERSION,
        engines: globalEngines,
      },
    },
    crypto: {
      keys: encryptPayload(wbo.cleartext),
    },
  };
  for (let engine of allEngines) {
    contents[engine.name] = {};
  }

  return serverForUsers(users, contents, callback);
}

async function serverForFoo(engine, callback) {
  // The bookmarks engine *always* tracks changes, meaning we might try
  // and sync due to the bookmarks we ourselves create! Worse, because we
  // do an engine sync only, there's no locking - so we end up with multiple
  // syncs running. Neuter that by making the threshold very large.
  Service.scheduler.syncThreshold = 10000000;
  return serverForEnginesWithKeys({ foo: "password" }, engine, callback);
}

// Places notifies history observers asynchronously, so `addVisits` might return
// before the tracker receives the notification. This helper registers an
// observer that resolves once the expected notification fires.
async function promiseVisit(expectedType, expectedURI) {
  return new Promise(resolve => {
    function done(type, uri) {
      if (uri == expectedURI.spec && type == expectedType) {
        PlacesObservers.removeListener(
          ["page-visited", "page-removed"],
          observer.handlePlacesEvents
        );
        resolve();
      }
    }
    let observer = {
      handlePlacesEvents(events) {
        Assert.equal(events.length, 1);

        if (events[0].type === "page-visited") {
          done("added", events[0].url);
        } else if (events[0].type === "page-removed") {
          Assert.ok(events[0].isRemovedFromStore);
          done("removed", events[0].url);
        }
      },
    };
    PlacesObservers.addListener(
      ["page-visited", "page-removed"],
      observer.handlePlacesEvents
    );
  });
}

async function addVisit(
  suffix,
  referrer = null,
  transition = PlacesUtils.history.TRANSITION_LINK
) {
  let uriString = "http://getfirefox.com/" + suffix;
  let uri = CommonUtils.makeURI(uriString);
  _("Adding visit for URI " + uriString);

  let visitAddedPromise = promiseVisit("added", uri);
  await PlacesTestUtils.addVisits({
    uri,
    visitDate: Date.now() * 1000,
    transition,
    referrer,
  });
  await visitAddedPromise;

  return uri;
}

function bookmarkNodesToInfos(nodes) {
  return nodes.map(node => {
    let info = {
      guid: node.guid,
      index: node.index,
    };
    if (node.children) {
      info.children = bookmarkNodesToInfos(node.children);
    }
    return info;
  });
}

async function assertBookmarksTreeMatches(rootGuid, expected, message) {
  let root = await PlacesUtils.promiseBookmarksTree(rootGuid, {
    includeItemIds: true
  });
  let actual = bookmarkNodesToInfos(root.children);

  if (!ObjectUtils.deepEqual(actual, expected)) {
    _(`Expected structure for ${rootGuid}`, JSON.stringify(expected));
    _(`Actual structure for ${rootGuid}`, JSON.stringify(actual));
    throw new Assert.constructor.AssertionError({ actual, expected, message });
  }
}

function add_bookmark_test(task) {
  const { BookmarksEngine } = ChromeUtils.import(
    "resource://services-sync/engines/bookmarks.js"
  );

  add_task(async function() {
    _(`Running bookmarks test ${task.name}`);
    let engine = new BookmarksEngine(Service);
    await engine.initialize();
    await engine._resetClient();
    try {
      await task(engine);
    } finally {
      await engine.finalize();
    }
  });
}
