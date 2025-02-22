// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * @fileoverview
 * 'settings-reset-page' is the settings page containing reset
 * settings.
 */
Polymer({
  is: 'settings-reset-page',

  behaviors: [settings.RouteObserverBehavior],

  properties: {
    /** Preferences state. */
    prefs: Object,

    // <if expr="_google_chrome and is_win">
    /** @private */
    showIncompatibleApplications_: {
      type: Boolean,
      value: function() {
        return loadTimeData.getBoolean('showIncompatibleApplications');
      },
    },
    // </if>
  },

  /**
   * settings.RouteObserverBehavior
   * @param {!settings.Route} route
   * @protected
   */
  currentRouteChanged: function(route) {
    const lazyRender =
        /** @type {!CrLazyRenderElement} */ (this.$.resetProfileDialog);

    if (route == settings.routes.TRIGGERED_RESET_DIALOG ||
        route == settings.routes.RESET_DIALOG) {
      /** @type {!SettingsResetProfileDialogElement} */ (lazyRender.get())
          .show();
    } else {
      const dialog = /** @type {?SettingsResetProfileDialogElement} */ (
          lazyRender.getIfExists());
      if (dialog) {
        dialog.cancel();
      }
    }
  },

  /** @private */
  onShowResetProfileDialog_: function() {
    settings.navigateTo(
        settings.routes.RESET_DIALOG, new URLSearchParams('origin=userclick'));
  },

  /** @private */
  onResetProfileDialogClose_: function() {
    settings.navigateToPreviousRoute();
    cr.ui.focusWithoutInk(assert(this.$.resetProfile));
  },

  // <if expr="_google_chrome and is_win">
  /** @private */
  onChromeCleanupTap_: function() {
    settings.navigateTo(settings.routes.CHROME_CLEANUP);
  },

  /** @private */
  onIncompatibleApplicationsTap_: function() {
    settings.navigateTo(settings.routes.INCOMPATIBLE_APPLICATIONS);
  },
  // </if>
});
