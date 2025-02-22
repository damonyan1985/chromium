// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.customtabs;

import androidx.browser.customtabs.CustomTabsCallback;
import androidx.browser.customtabs.CustomTabsSessionToken;

import org.chromium.chrome.browser.browserservices.BrowserServicesIntentDataProvider;
import org.chromium.chrome.browser.dependency_injection.ActivityScope;
import org.chromium.chrome.browser.tab.EmptyTabObserver;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.TabHidingType;
import org.chromium.chrome.browser.tab.TabImpl;
import org.chromium.chrome.browser.tab.TabSelectionType;
import org.chromium.components.security_state.ConnectionSecurityLevel;

import javax.inject.Inject;

/**
 * An observer for firing navigation events on {@link CustomTabsCallback}.
 */
@ActivityScope
public class CustomTabNavigationEventObserver extends EmptyTabObserver {
    // An operation was aborted (due to user action). Should match the value in net_error_list.h.
    private static final int NET_ERROR_ABORTED = -3;

    private final CustomTabsSessionToken mSessionToken;
    private final CustomTabsConnection mConnection;

    @Inject
    public CustomTabNavigationEventObserver(
            BrowserServicesIntentDataProvider intentDataProvider, CustomTabsConnection connection) {
        mSessionToken = intentDataProvider.getSession();
        mConnection = connection;
    }

    @Override
    public void onPageLoadStarted(Tab tab, String url) {
        mConnection.notifyNavigationEvent(mSessionToken, CustomTabsCallback.NAVIGATION_STARTED);
    }

    @Override
    public void onPageLoadFinished(Tab tab, String url) {
        mConnection.notifyNavigationEvent(mSessionToken, CustomTabsCallback.NAVIGATION_FINISHED);
    }

    @Override
    public void onPageLoadFailed(Tab tab, int errorCode) {
        int navigationEvent = errorCode == NET_ERROR_ABORTED ? CustomTabsCallback.NAVIGATION_ABORTED
                                                             : CustomTabsCallback.NAVIGATION_FAILED;
        mConnection.notifyNavigationEvent(mSessionToken, navigationEvent);
    }

    @Override
    public void onShown(Tab tab, @TabSelectionType int type) {
        mConnection.notifyNavigationEvent(mSessionToken, CustomTabsCallback.TAB_SHOWN);
    }

    @Override
    public void onHidden(Tab tab, @TabHidingType int type) {
        mConnection.notifyNavigationEvent(mSessionToken, CustomTabsCallback.TAB_HIDDEN);
    }

    @Override
    public void onDidAttachInterstitialPage(Tab tab) {
        if (((TabImpl) tab).getSecurityLevel() != ConnectionSecurityLevel.DANGEROUS) return;
        mConnection.notifyNavigationEvent(mSessionToken, CustomTabsCallback.NAVIGATION_FAILED);
    }
}
