/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
"use strict";

function translatePreferencesToState(preferences) {
  return {
    ...preferences,
    interval: preferences.interval / 1000, // converts from µs to ms
  };
}

function translatePreferencesFromState(state) {
  return {
    ...state,
    interval: state.interval * 1000, // converts from ms to µs
  };
}

module.exports = {
  translatePreferencesToState,
  translatePreferencesFromState,
};
