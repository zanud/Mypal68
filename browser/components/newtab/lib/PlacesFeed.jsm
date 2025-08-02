/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
"use strict";

const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");

const { actionCreators: ac, actionTypes: at } = ChromeUtils.import(
  "resource://activity-stream/common/Actions.jsm"
);

ChromeUtils.defineModuleGetter(
  this,
  "NewTabUtils",
  "resource://gre/modules/NewTabUtils.jsm"
);
ChromeUtils.defineModuleGetter(
  this,
  "PlacesUtils",
  "resource://gre/modules/PlacesUtils.jsm"
);

const LINK_BLOCKED_EVENT = "newtab-linkBlocked";
const PLACES_LINKS_CHANGED_DELAY_TIME = 1000; // time in ms to delay timer for places links changed events

/**
 * Observer - a wrapper around history/bookmark observers to add the QueryInterface.
 */
class Observer {
  constructor(dispatch, observerInterface) {
    this.dispatch = dispatch;
    this.QueryInterface = ChromeUtils.generateQI([
      observerInterface,
      Ci.nsISupportsWeakReference,
    ]);
  }
}

/**
 * BookmarksObserver - observes events from PlacesUtils.bookmarks
 */
class BookmarksObserver extends Observer {
  constructor(dispatch) {
    super(dispatch, Ci.nsINavBookmarkObserver);
    this.skipTags = true;
  }

  // Empty functions to make xpconnect happy
  onItemMoved() {}

  // Disabled due to performance cost, see Issue 3203 /
  // https://bugzilla.mozilla.org/show_bug.cgi?id=1392267.
  onItemChanged() {}
}

/**
 * PlacesObserver - observes events from PlacesUtils.observers
 */
class PlacesObserver extends Observer {
  constructor(dispatch) {
    super(dispatch, Ci.nsINavBookmarkObserver);
    this.handlePlacesEvent = this.handlePlacesEvent.bind(this);
  }

  handlePlacesEvent(events) {
    const removedURLs = [];

    for (const {
      itemType,
      source,
      dateAdded,
      guid,
      title,
      url,
      isRemovedFromStore,
      isTagging,
      type,
    } of events) {
      switch (type) {
        case "history-cleared":
          this.dispatch({ type: at.PLACES_HISTORY_CLEARED });
          break;
        case "page-removed":
          if (isRemovedFromStore) {
            this.dispatch({ type: at.PLACES_LINKS_CHANGED });
            this.dispatch({
              type: at.PLACES_LINK_DELETED,
              data: { url },
            });
          }
          break;
        case "bookmark-added":
          // Skips items that are not bookmarks (like folders), about:* pages or
          // default bookmarks, added when the profile is created.
          if (
            isTagging ||
            itemType !== PlacesUtils.bookmarks.TYPE_BOOKMARK ||
            source === PlacesUtils.bookmarks.SOURCES.IMPORT ||
            source === PlacesUtils.bookmarks.SOURCES.RESTORE ||
            source === PlacesUtils.bookmarks.SOURCES.RESTORE_ON_STARTUP ||
            source === PlacesUtils.bookmarks.SOURCES.SYNC ||
            (!url.startsWith("http://") && !url.startsWith("https://"))
          ) {
            return;
          }

          this.dispatch({ type: at.PLACES_LINKS_CHANGED });
          this.dispatch({
            type: at.PLACES_BOOKMARK_ADDED,
            data: {
              bookmarkGuid: guid,
              bookmarkTitle: title,
              dateAdded: dateAdded * 1000,
              url,
            },
          });
          break;
        case "bookmark-removed":
          if (
            isTagging ||
            (itemType === PlacesUtils.bookmarks.TYPE_BOOKMARK &&
              source !== PlacesUtils.bookmarks.SOURCES.IMPORT &&
              source !== PlacesUtils.bookmarks.SOURCES.RESTORE &&
              source !== PlacesUtils.bookmarks.SOURCES.RESTORE_ON_STARTUP &&
              source !== PlacesUtils.bookmarks.SOURCES.SYNC)
          ) {
            removedURLs.push(url);
          }
          break;
      }
    }

    if (removedURLs.length) {
      this.dispatch({ type: at.PLACES_LINKS_CHANGED });
      this.dispatch({
        type: at.PLACES_BOOKMARKS_REMOVED,
        data: { urls: removedURLs },
      });
    }
  }
}

class PlacesFeed {
  constructor() {
    this.placesChangedTimer = null;
    this.customDispatch = this.customDispatch.bind(this);
    this.bookmarksObserver = new BookmarksObserver(this.customDispatch);
    this.placesObserver = new PlacesObserver(this.customDispatch);
  }

  addObservers() {
    // NB: Directly get services without importing the *BIG* PlacesUtils module
    Cc["@mozilla.org/browser/nav-bookmarks-service;1"]
      .getService(Ci.nsINavBookmarksService)
      .addObserver(this.bookmarksObserver, true);
    PlacesUtils.observers.addListener(
      ["bookmark-added", "bookmark-removed", "history-cleared", "page-removed"],
      this.placesObserver.handlePlacesEvent
    );

    Services.obs.addObserver(this, LINK_BLOCKED_EVENT);
  }

  /**
   * setTimeout - A custom function that creates an nsITimer that can be cancelled
   *
   * @param {func} callback       A function to be executed after the timer expires
   * @param {int}  delay          The time (in ms) the timer should wait before the function is executed
   */
  setTimeout(callback, delay) {
    let timer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
    timer.initWithCallback(callback, delay, Ci.nsITimer.TYPE_ONE_SHOT);
    return timer;
  }

  customDispatch(action) {
    // If we are changing many links at once, delay this action and only dispatch
    // one action at the end
    if (action.type === at.PLACES_LINKS_CHANGED) {
      if (this.placesChangedTimer) {
        this.placesChangedTimer.delay = PLACES_LINKS_CHANGED_DELAY_TIME;
      } else {
        this.placesChangedTimer = this.setTimeout(() => {
          this.placesChangedTimer = null;
          this.store.dispatch(ac.OnlyToMain(action));
        }, PLACES_LINKS_CHANGED_DELAY_TIME);
      }
    } else {
      this.store.dispatch(ac.BroadcastToContent(action));
    }
  }

  removeObservers() {
    if (this.placesChangedTimer) {
      this.placesChangedTimer.cancel();
      this.placesChangedTimer = null;
    }
    PlacesUtils.bookmarks.removeObserver(this.bookmarksObserver);
    PlacesUtils.observers.removeListener(
      ["bookmark-added", "bookmark-removed", "history-cleared", "page-removed"],
      this.placesObserver.handlePlacesEvent
    );
    Services.obs.removeObserver(this, LINK_BLOCKED_EVENT);
  }

  /**
   * observe - An observer for the LINK_BLOCKED_EVENT.
   *           Called when a link is blocked.
   *
   * @param  {null} subject
   * @param  {str} topic   The name of the event
   * @param  {str} value   The data associated with the event
   */
  observe(subject, topic, value) {
    if (topic === LINK_BLOCKED_EVENT) {
      this.store.dispatch(
        ac.BroadcastToContent({
          type: at.PLACES_LINK_BLOCKED,
          data: { url: value },
        })
      );
    }
  }

  /**
   * Open a link in a desired destination defaulting to action's event.
   */
  openLink(action, where = "", isPrivate = false) {
    const params = {
      private: isPrivate,
      triggeringPrincipal: Services.scriptSecurityManager.createNullPrincipal(
        {}
      ),
    };

    // Always include the referrer (even for http links) if we have one
    const { event, referrer, typedBonus } = action.data;
    if (referrer) {
      const ReferrerInfo = Components.Constructor(
        "@mozilla.org/referrer-info;1",
        "nsIReferrerInfo",
        "init"
      );
      params.referrerInfo = new ReferrerInfo(
        Ci.nsIReferrerInfo.UNSAFE_URL,
        true,
        Services.io.newURI(referrer)
      );
    }

    const urlToOpen = action.data.url;

    // Mark the page as typed for frecency bonus before opening the link
    if (typedBonus) {
      PlacesUtils.history.markPageAsTyped(Services.io.newURI(urlToOpen));
    }

    const win = action._target.browser.ownerGlobal;
    win.openLinkIn(urlToOpen, where || win.whereToOpenLink(event), params);
  }

  fillSearchTopSiteTerm({ _target, data }) {
    _target.browser.ownerGlobal.gURLBar.search(`${data.label} `);
  }

  _getSearchPrefix() {
    const searchAliases =
      Services.search.defaultEngine.wrappedJSObject.__internalAliases;
    if (searchAliases && searchAliases.length > 0) {
      return `${searchAliases[0]} `;
    }
    return "";
  }

  handoffSearchToAwesomebar({ _target, data, meta }) {
    const searchAlias = this._getSearchPrefix();
    const urlBar = _target.browser.ownerGlobal.gURLBar;
    let isFirstChange = true;

    if (!data || !data.text) {
      urlBar.setHiddenFocus();
    } else {
      // Pass the provided text to the awesomebar. Prepend the @engine shortcut.
      urlBar.search(`${searchAlias}${data.text}`);
      isFirstChange = false;
    }

    const checkFirstChange = () => {
      // Check if this is the first change since we hidden focused. If it is,
      // remove hidden focus styles, prepend the search alias and hide the
      // in-content search.
      if (isFirstChange) {
        isFirstChange = false;
        urlBar.removeHiddenFocus();
        urlBar.search(searchAlias);
        this.store.dispatch(
          ac.OnlyToOneContent({ type: at.HIDE_SEARCH }, meta.fromTarget)
        );
        urlBar.removeEventListener("compositionstart", checkFirstChange);
        urlBar.removeEventListener("paste", checkFirstChange);
      }
    };

    const onKeydown = ev => {
      // Check if the keydown will cause a value change.
      if (ev.key.length === 1 && !ev.altKey && !ev.ctrlKey && !ev.metaKey) {
        checkFirstChange();
      }
      // If the Esc button is pressed, we are done. Show in-content search and cleanup.
      if (ev.key === "Escape") {
        onDone(); // eslint-disable-line no-use-before-define
      }
    };

    const onDone = () => {
      // We are done. Show in-content search again and cleanup.
      this.store.dispatch(
        ac.OnlyToOneContent({ type: at.SHOW_SEARCH }, meta.fromTarget)
      );
      urlBar.removeHiddenFocus();

      urlBar.removeEventListener("keydown", onKeydown);
      urlBar.removeEventListener("mousedown", onDone);
      urlBar.removeEventListener("blur", onDone);
      urlBar.removeEventListener("compositionstart", checkFirstChange);
      urlBar.removeEventListener("paste", checkFirstChange);
    };

    urlBar.addEventListener("keydown", onKeydown);
    urlBar.addEventListener("mousedown", onDone);
    urlBar.addEventListener("blur", onDone);
    urlBar.addEventListener("compositionstart", checkFirstChange);
    urlBar.addEventListener("paste", checkFirstChange);
  }

  onAction(action) {
    switch (action.type) {
      case at.INIT:
        // Briefly avoid loading services for observing for better startup timing
        Services.tm.dispatchToMainThread(() => this.addObservers());
        break;
      case at.UNINIT:
        this.removeObservers();
        break;
      case at.BLOCK_URL: {
        const { url, pocket_id } = action.data;
        NewTabUtils.activityStreamLinks.blockURL({ url, pocket_id });
        break;
      }
      case at.BOOKMARK_URL:
        NewTabUtils.activityStreamLinks.addBookmark(
          action.data,
          action._target.browser.ownerGlobal
        );
        break;
      case at.DELETE_BOOKMARK_BY_ID:
        NewTabUtils.activityStreamLinks.deleteBookmark(action.data);
        break;
      case at.DELETE_HISTORY_URL: {
        const { url, forceBlock, pocket_id } = action.data;
        NewTabUtils.activityStreamLinks.deleteHistoryEntry(url);
        if (forceBlock) {
          NewTabUtils.activityStreamLinks.blockURL({ url, pocket_id });
        }
        break;
      }
      case at.OPEN_NEW_WINDOW:
        this.openLink(action, "window");
        break;
      case at.OPEN_PRIVATE_WINDOW:
        this.openLink(action, "window", true);
        break;
      case at.FILL_SEARCH_TERM:
        this.fillSearchTopSiteTerm(action);
        break;
      case at.HANDOFF_SEARCH_TO_AWESOMEBAR:
        this.handoffSearchToAwesomebar(action);
        break;
      case at.OPEN_LINK: {
        this.openLink(action);
        break;
      }
    }
  }
}

this.PlacesFeed = PlacesFeed;

// Exported for testing only
PlacesFeed.BookmarksObserver = BookmarksObserver;
PlacesFeed.PlacesObserver = PlacesObserver;

const EXPORTED_SYMBOLS = ["PlacesFeed"];
