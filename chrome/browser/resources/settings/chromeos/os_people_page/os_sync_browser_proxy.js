// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * @fileoverview A helper object to get the status of the sync backend and user
 * preferences on what data to sync.
 */
cr.exportPath('settings');

/**
 * User preferences for OS sync. 'Registered' means the user has the option to
 * select a type. For example, a type might not be registered due to a feature
 * flag being disabled.
 * @see components/sync/driver/sync_service.h
 *
 * TODO(jamescook): Encryption options.
 *
 * @typedef {{
 *   osAppsRegistered: boolean,
 *   osAppsSynced: boolean,
 *   osPreferencesRegistered: boolean,
 *   osPreferencesSynced: boolean,
 *   syncAllOsDataTypes: boolean,
 *   wifiConfigurationsRegistered: boolean,
 *   wifiConfigurationsSynced: boolean,
 * }}
 */
settings.OsSyncPrefs;

cr.define('settings', function() {
  /** @interface */
  class OsSyncBrowserProxy {
    /**
     * Function to invoke when the sync page has been navigated to. This
     * registers the UI as the "active" sync UI.
     */
    didNavigateToOsSyncPage() {}

    /**
     * Function to invoke when leaving the sync page so that the C++ layer can
     * be notified that the sync UI is no longer open.
     */
    didNavigateAwayFromOsSyncPage() {}

    /**
     * Sets whether the OS sync feature should be enabled. Sync will not start
     * until the user either navigates away from the page or closes settings.
     * @param {boolean} enabled
     */
    setOsSyncFeatureEnabled(enabled) {}

    /**
     * Sets which types of data to sync.
     * @param {!settings.OsSyncPrefs} osSyncPrefs
     */
    setOsSyncDatatypes(osSyncPrefs) {}
  }

  /**
   * @implements {settings.OsSyncBrowserProxy}
   */
  class OsSyncBrowserProxyImpl {
    /** @override */
    didNavigateToOsSyncPage() {
      chrome.send('DidNavigateToOsSyncPage');
    }

    /** @override */
    didNavigateAwayFromOsSyncPage() {
      chrome.send('DidNavigateAwayFromOsSyncPage');
    }

    /** @override */
    setOsSyncFeatureEnabled(enabled) {
      return chrome.send('SetOsSyncFeatureEnabled', [enabled]);
    }

    /** @override */
    setOsSyncDatatypes(osSyncPrefs) {
      return chrome.send('SetOsSyncDatatypes', [osSyncPrefs]);
    }
  }

  cr.addSingletonGetter(OsSyncBrowserProxyImpl);

  return {
    OsSyncBrowserProxy: OsSyncBrowserProxy,
    OsSyncBrowserProxyImpl: OsSyncBrowserProxyImpl,
  };
});
