/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
"use strict";

/* exported gInit, gDestroy, loader */

const { BrowserLoader } = ChromeUtils.import(
  "resource://devtools/client/shared/browser-loader.js"
);
const { require, loader } = BrowserLoader({
  baseURI: "resource://devtools/client/performance-new/",
  window,
});
const Perf = require("devtools/client/performance-new/components/Perf");
const ReactDOM = require("devtools/client/shared/vendor/react-dom");
const React = require("devtools/client/shared/vendor/react");
const createStore = require("devtools/client/shared/redux/create-store");
const selectors = require("devtools/client/performance-new/store/selectors");
const reducers = require("devtools/client/performance-new/store/reducers");
const actions = require("devtools/client/performance-new/store/actions");
const { Provider } = require("devtools/client/shared/vendor/react-redux");
const {
  receiveProfile,
  getRecordingPreferencesFromDebuggee,
  setRecordingPreferencesOnDebuggee,
  createMultiModalGetSymbolTableFn,
} = require("devtools/client/performance-new/browser");

const { getDefaultRecordingPreferences } = ChromeUtils.import(
  "resource://devtools/client/performance-new/popup/background.jsm.js"
);

async function gInit(perfFront, preferenceFront) {
  const store = createStore(reducers);

  // Send the initial requests in parallel.
  const [recordingPreferences, supportedFeatures] = await Promise.all([
    // Pull the default recording settings from the background.jsm module. Update them
    // according to what's in the target's preferences. This way the preferences are
    // stored on the target. This could be useful for something like Android where you
    // might want to tweak the settings.
    getRecordingPreferencesFromDebuggee(
      preferenceFront,
      getDefaultRecordingPreferences()
    ),
    // Get the supported features from the debuggee. If the debuggee is before
    // Firefox 72, then return null, as the actor does not support that feature.
    // We can't use `target.actorHasMethod`, because the target is not provided
    // when remote debugging. Unfortunately, this approach means that if this
    // function throws a real error, it will get swallowed here.
    Promise.resolve(perfFront.getSupportedFeatures()).catch(() => null),
  ]);

  // Do some initialization, especially with privileged things that are part of the
  // the browser.
  store.dispatch(
    actions.initializeStore({
      perfFront,
      receiveProfile,
      recordingPreferences,
      supportedFeatures,
      isPopup: false,

      // Go ahead and hide the implementation details for the component on how the
      // preference information is stored
      setRecordingPreferences: newRecordingPreferences =>
        setRecordingPreferencesOnDebuggee(
          preferenceFront,
          newRecordingPreferences
        ),

      // Configure the getSymbolTable function for the DevTools workflow.
      // See createMultiModalGetSymbolTableFn for more information.
      getSymbolTableGetter: profile =>
        createMultiModalGetSymbolTableFn(
          profile,
          selectors.getPerfFront(store.getState()),
          selectors.getObjdirs(store.getState())
        ),
    })
  );

  ReactDOM.render(
    React.createElement(Provider, { store }, React.createElement(Perf)),
    document.querySelector("#root")
  );
}

function gDestroy() {
  ReactDOM.unmountComponentAtNode(document.querySelector("#root"));
}
