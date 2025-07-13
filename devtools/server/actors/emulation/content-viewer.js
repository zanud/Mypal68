/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { Ci } = require("chrome");
const protocol = require("devtools/shared/protocol");
const { contentViewerSpec } = require("devtools/shared/specs/content-viewer");

/**
 * This actor emulates various browser content environments by using methods available
 * on the ContentViewer exposed by the platform.
 */
const ContentViewerActor = protocol.ActorClassWithSpec(contentViewerSpec, {
  initialize(conn, targetActor) {
    protocol.Actor.prototype.initialize.call(this, conn);
    this.targetActor = targetActor;
    this.docShell = targetActor.docShell;

    this.onWillNavigate = this.onWillNavigate.bind(this);
    this.onWindowReady = this.onWindowReady.bind(this);

    this.targetActor.on("will-navigate", this.onWillNavigate);
    this.targetActor.on("window-ready", this.onWindowReady);
  },

  destroy() {
    this.stopPrintMediaSimulation();

    this.targetActor.off("will-navigate", this.onWillNavigate);
    this.targetActor.off("window-ready", this.onWindowReady);

    this.targetActor = null;
    this.docShell = null;

    protocol.Actor.prototype.destroy.call(this);
  },

  onWillNavigate({ isTopLevel }) {
    // Make sure that print simulation is stopped before navigating to another page. We
    // need to do this since the browser will cache the last state of the page in its
    // session history.
    if (this._printSimulationEnabled && isTopLevel) {
      this.stopPrintMediaSimulation(true);
    }
  },

  onWindowReady({ isTopLevel }) {
    // Since `emulateMedium` only works for the current page, we need to ensure persistent
    // print simulation for when the user navigates to a new page while its enabled.
    // To do this, we need to tell the page to begin print simulation before the DOM
    // content is available to the user:
    if (this._printSimulationEnabled && isTopLevel) {
      this.startPrintMediaSimulation();
    }
  },

  /* Simulating print media for the page */

  _printSimulationEnabled: false,

  getIsPrintSimulationEnabled() {
    return this._printSimulationEnabled;
  },

  async startPrintMediaSimulation() {
    this._printSimulationEnabled = true;
    this.targetActor.docShell.contentViewer.emulateMedium("print");
  },

  /**
   * Stop simulating print media for the current page.
   *
   * @param {Boolean} state
   *        Whether or not to set _printSimulationEnabled to false. If true, we want to
   *        stop simulation print media for the current page but NOT set
   *        _printSimulationEnabled to false. We do this specifically for the
   *        "will-navigate" event where we still want to continue simulating print when
   *        navigating to the next page. Defaults to false, meaning we want to completely
   *        stop print simulation.
   */
  async stopPrintMediaSimulation(state = false) {
    this._printSimulationEnabled = state;
    this.targetActor.docShell.contentViewer.stopEmulatingMedium();
  },
});

exports.ContentViewerActor = ContentViewerActor;
