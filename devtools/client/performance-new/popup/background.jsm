/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
"use strict";

/**
 * This file contains all of the background logic for controlling the state and
 * configuration of the profiler. It is in a JSM so that the logic can be shared
 * with both the popup client, and the keyboard shortcuts. The shortcuts don't need
 * access to any UI, and need to be loaded independent of the popup.
 */

// The following are not lazily loaded as they are needed during initialization.f
const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
const { AppConstants } = ChromeUtils.import(
  "resource://gre/modules/AppConstants.jsm"
);
const { loader } = ChromeUtils.import("resource://devtools/shared/Loader.jsm");

const ENTRIES_PREF = "devtools.performance.recording.entries";
const INTERVAL_PREF = "devtools.performance.recording.interval";
const FEATURES_PREF = "devtools.performance.recording.features";
const THREADS_PREF = "devtools.performance.recording.threads";
const OBJDIRS_PREF = "devtools.performance.recording.objdirs";
const DURATION_PREF = "devtools.performance.recording.duration";

// The following utilities are lazily loaded as they are not needed when controlling the
// global state of the profiler, and only are used during specific funcationality like
// symbolication or capturing a profile.
ChromeUtils.defineModuleGetter(this, "OS", "resource://gre/modules/osfile.jsm");
ChromeUtils.defineModuleGetter(
  this,
  "ProfilerGetSymbols",
  "resource://gre/modules/ProfilerGetSymbols.jsm"
);
loader.lazyRequireGetter(
  this,
  "receiveProfile",
  "devtools/client/performance-new/browser",
  true
);

const lazyPreferenceManagement = requireLazy(() => {
  const { require } = ChromeUtils.import(
    "resource://devtools/shared/Loader.jsm"
  );

  const preferenceManagementModule = require("devtools/client/performance-new/preference-management");
  return preferenceManagementModule;
});

const symbolCache = new Map();
async function getSymbolsFromThisBrowser(debugName, breakpadId) {
  if (symbolCache.size === 0) {
    // Prime the symbols cache.
    for (const lib of Services.profiler.sharedLibraries) {
      symbolCache.set(`${lib.debugName}/${lib.breakpadId}`, {
        path: lib.path,
        debugPath: lib.debugPath,
      });
    }
  }

  const cachedLibInfo = symbolCache.get(`${debugName}/${breakpadId}`);
  if (!cachedLibInfo) {
    throw new Error(
      `The library ${debugName} ${breakpadId} is not in the ` +
        "Services.profiler.sharedLibraries list, so the local path for it is not known " +
        "and symbols for it can not be obtained. This usually happens if a content " +
        "process uses a library that's not used in the parent process - " +
        "Services.profiler.sharedLibraries only knows about libraries in the " +
        "parent process."
    );
  }

  const { path, debugPath } = cachedLibInfo;
  if (!OS.Path.split(path).absolute) {
    throw new Error(
      "Services.profiler.sharedLibraries did not contain an absolute path for " +
        `the library ${debugName} ${breakpadId}, so symbols for this library can not ` +
        "be obtained."
    );
  }

  return ProfilerGetSymbols.getSymbolTable(path, debugPath, breakpadId);
}

async function captureProfile() {
  if (!Services.profiler.IsActive()) {
    // The profiler is not active, ignore this shortcut.
    return;
  }
  if (Services.profiler.IsPaused()) {
    return;
  }

  // Pause profiler before we collect the profile, so that we don't capture
  // more samples while the parent process waits for subprocess profiles.
  Services.profiler.Pause();

  const profile = await Services.profiler
    .getProfileDataAsGzippedArrayBuffer()
    .catch(e => {
      console.error(e);
      return {};
    });

  receiveProfile(profile, getSymbolsFromThisBrowser);

  Services.profiler.StopProfiler();
}

function startProfiler() {
  const { translatePreferencesToState } = lazyPreferenceManagement();
  const {
    entries,
    interval,
    features,
    threads,
    duration,
  } = translatePreferencesToState(getRecordingPreferencesFromBrowser());

  Services.profiler.StartProfiler(
    entries,
    interval,
    features,
    threads,
    duration
  );
}

function stopProfiler() {
  Services.profiler.StopProfiler();
}

function toggleProfiler() {
  if (Services.profiler.IsPaused()) {
    return;
  }
  if (Services.profiler.IsActive()) {
    stopProfiler();
  } else {
    startProfiler();
  }
}

function restartProfiler() {
  stopProfiler();
  startProfiler();
}


function _getArrayOfStringsPref(prefName, defaultValue) {
  let array;
  try {
    const text = Services.prefs.getCharPref(prefName);
    array = JSON.parse(text);
  } catch (error) {
    return defaultValue;
  }

  if (
    Array.isArray(array) &&
    array.every(feature => typeof feature === "string")
  ) {
    return array;
  }

  return defaultValue;
}

function _getArrayOfStringsHostPref(prefName, defaultValue) {
  let array;
  try {
    const text = Services.prefs.getStringPref(
      prefName,
      JSON.stringify(defaultValue)
    );
    array = JSON.parse(text);
  } catch (error) {
    return defaultValue;
  }

  if (
    Array.isArray(array) &&
    array.every(feature => typeof feature === "string")
  ) {
    return array;
  }

  return defaultValue;
}

let _defaultPrefs;

function getDefaultRecordingPreferences() {
  if (!_defaultPrefs) {
    _defaultPrefs = {
      entries: 10000000, // ~80mb,
      // Do not expire markers, let them roll off naturally from the circular buffer.
      duration: 0,
      interval: 1000, // 1000µs = 1ms
      features: ["js", "leaf", "responsiveness", "stackwalk"],
      threads: ["GeckoMain", "Compositor"],
      objdirs: [],
    };

    if (AppConstants.platform === "android") {
      // Java profiling is only meaningful on android.
      _defaultPrefs.features.push("java");
    }
  }

  return _defaultPrefs;
}

function getRecordingPreferencesFromBrowser() {
  const defaultPrefs = getDefaultRecordingPreferences();

  const entries = Services.prefs.getIntPref(ENTRIES_PREF, defaultPrefs.entries);
  const interval = Services.prefs.getIntPref(
    INTERVAL_PREF,
    defaultPrefs.interval
  );
  const features = _getArrayOfStringsPref(FEATURES_PREF, defaultPrefs.features);
  const threads = _getArrayOfStringsPref(THREADS_PREF, defaultPrefs.threads);
  const objdirs = _getArrayOfStringsHostPref(
    OBJDIRS_PREF,
    defaultPrefs.objdirs
  );
  const duration = Services.prefs.getIntPref(
    DURATION_PREF,
    defaultPrefs.duration
  );

  const supportedFeatures = new Set(Services.profiler.GetFeatures());

  return {
    entries,
    interval,
    // Validate the features before passing them to the profiler.
    features: features.filter(feature => supportedFeatures.has(feature)),
    threads,
    objdirs,
    duration,
  };
}

function setRecordingPreferencesOnBrowser(prefs) {
  Services.prefs.setIntPref(ENTRIES_PREF, prefs.entries);
  // The interval pref stores the value in microseconds for extra precision.
  Services.prefs.setIntPref(INTERVAL_PREF, prefs.interval);
  Services.prefs.setCharPref(FEATURES_PREF, JSON.stringify(prefs.features));
  Services.prefs.setCharPref(THREADS_PREF, JSON.stringify(prefs.threads));
  Services.prefs.setCharPref(OBJDIRS_PREF, JSON.stringify(prefs.objdirs));
}

const platform = AppConstants.platform;

function revertRecordingPreferences() {
  setRecordingPreferencesOnBrowser(getDefaultRecordingPreferences());
}

var EXPORTED_SYMBOLS = [
  "captureProfile",
  "startProfiler",
  "stopProfiler",
  "restartProfiler",
  "toggleProfiler",
  "platform",
  "getSymbolsFromThisBrowser",
  "getDefaultRecordingPreferences",
  "getRecordingPreferencesFromBrowser",
  "setRecordingPreferencesOnBrowser",
  "revertRecordingPreferences",
];
