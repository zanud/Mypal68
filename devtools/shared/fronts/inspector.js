/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const Services = require("Services");
const {
  FrontClassWithSpec,
  registerFront,
} = require("devtools/shared/protocol.js");
const { inspectorSpec } = require("devtools/shared/specs/inspector");
loader.lazyRequireGetter(this, "flags", "devtools/shared/flags");

const SHOW_ALL_ANONYMOUS_CONTENT_PREF =
  "devtools.inspector.showAllAnonymousContent";
const SHOW_UA_SHADOW_ROOTS_PREF = "devtools.inspector.showUserAgentShadowRoots";

/**
 * Client side of the inspector actor, which is used to create
 * inspector-related actors, including the walker.
 */
class InspectorFront extends FrontClassWithSpec(inspectorSpec) {
  constructor(client, targetFront, parentFront) {
    super(client, targetFront, parentFront);

    this._client = client;
    this._highlighters = new Map();

    // Attribute name from which to retrieve the actorID out of the target actor's form
    this.formAttributeName = "inspectorActor";
  }

  // async initialization
  async initialize() {
    await Promise.all([this._getWalker(), this._getHighlighter()]);
  }

  async _getWalker() {
    const showAllAnonymousContent = Services.prefs.getBoolPref(
      SHOW_ALL_ANONYMOUS_CONTENT_PREF
    );
    const showUserAgentShadowRoots = Services.prefs.getBoolPref(
      SHOW_UA_SHADOW_ROOTS_PREF
    );
    this.walker = await this.getWalker({
      showAllAnonymousContent,
      showUserAgentShadowRoots,
    });
  }

  async _getHighlighter() {
    const autohide = !flags.testing;
    this.highlighter = await this.getHighlighter(autohide);
  }

  hasHighlighter(type) {
    return this._highlighters.has(type);
  }

  destroy() {
    // Highlighter fronts are managed by InspectorFront and so will be
    // automatically destroyed. But we have to clear the `_highlighters`
    // Map as well as explicitly call `finalize` request on all of them.
    this.destroyHighlighters();
    super.destroy();
  }

  destroyHighlighters() {
    for (const type of this._highlighters.keys()) {
      if (this._highlighters.has(type)) {
        this._highlighters.get(type).finalize();
        this._highlighters.delete(type);
      }
    }
  }

  async getHighlighterByType(typeName) {
    let highlighter = null;
    try {
      highlighter = await super.getHighlighterByType(typeName);
    } catch (_) {
      throw new Error(
        "The target doesn't support " +
          `creating highlighters by types or ${typeName} is unknown`
      );
    }
    return highlighter;
  }

  getKnownHighlighter(type) {
    return this._highlighters.get(type);
  }

  async getOrCreateHighlighterByType(type) {
    let front = this._highlighters.get(type);
    if (!front) {
      front = await this.getHighlighterByType(type);
      this._highlighters.set(type, front);
    }
    return front;
  }

  async pickColorFromPage(options) {
    await super.pickColorFromPage(options);
  }

  /**
   * Given a node grip, return a NodeFront on the right context.
   *
   * @param {Object} grip: The node grip.
   * @returns {Promise<NodeFront|null>} A promise that resolves with  a NodeFront or null
   *                                    if the NodeFront couldn't be created/retrieved.
   */
  async getNodeFrontFromNodeGrip(grip) {
    const gripHasContentDomReference = "contentDomReference" in grip;

    if (!gripHasContentDomReference) {
      // Backward compatibility ( < Firefox 71):
      // If the grip does not have a contentDomReference, we can't know in which browsing
      // context id the node lives. We fall back on gripToNodeFront that might retrieve
      // the expected nodeFront.
      return this.walker.gripToNodeFront(grip);
    }

    const { contentDomReference } = grip;
    const { browsingContextId } = contentDomReference;

    // If the grip lives in the same browsing context id than the current one, we can
    // directly use the current walker.
    // TODO: When Bug 1578745 lands, we might want to force using `this.walker` as well
    // when the new pref is set to false.
    if (this.targetFront.browsingContextID === browsingContextId) {
      return this.walker.getNodeActorFromContentDomReference(
        contentDomReference
      );
    }

    // If the contentDomReference has a different browsing context than the current one,
    // we are either in Fission or in the Omniscient Browser Toolbox, so we need to
    // retrieve the walker of the BrowsingContextTarget.
    const descriptor = await this.targetFront.client.mainRoot.getBrowsingContextDescriptor(
      browsingContextId
    );
    const target = await descriptor.getTarget();
    const { walker } = await target.getFront("inspector");
    return walker.getNodeActorFromContentDomReference(contentDomReference);
  }
}

exports.InspectorFront = InspectorFront;
registerFront(InspectorFront);
