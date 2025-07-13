/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
"use strict";

const { Arg, RetVal, generateActorSpec } = require("devtools/shared/protocol");

const contentViewerSpec = generateActorSpec({
  typeName: "contentViewer",

  methods: {
    getIsPrintSimulationEnabled: {
      request: {},
      response: {
        enabled: RetVal("boolean"),
      },
    },

    startPrintMediaSimulation: {
      request: {},
      response: {},
    },

    stopPrintMediaSimulation: {
      request: {
        state: Arg(0, "boolean"),
      },
      response: {},
    },
  },
});

exports.contentViewerSpec = contentViewerSpec;
