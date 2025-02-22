// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.tab;

import android.annotation.SuppressLint;
import android.os.Handler;
import android.os.Message;

import org.chromium.chrome.browser.ChromeFeatureList;
import org.chromium.chrome.browser.device.DeviceClassManager;
import org.chromium.chrome.browser.util.AccessibilityUtil;
import org.chromium.chrome.browser.util.UrlConstants;
import org.chromium.components.dom_distiller.core.DomDistillerUrlUtils;
import org.chromium.components.security_state.ConnectionSecurityLevel;
import org.chromium.content_public.browser.ImeAdapter;
import org.chromium.content_public.browser.ImeEventObserver;
import org.chromium.content_public.browser.NavigationHandle;
import org.chromium.content_public.browser.SelectionPopupController;
import org.chromium.content_public.browser.WebContents;
import org.chromium.content_public.common.BrowserControlsState;

/**
 * Determines the desired visibility of the browser controls based on the current state of a given
 * tab.
 */
public class TabStateBrowserControlsVisibilityDelegate
        extends BrowserControlsVisibilityDelegate implements ImeEventObserver {
    protected static final int MSG_ID_ENABLE_FULLSCREEN_AFTER_LOAD = 1;
    /** The maximum amount of time to wait for a page to load before entering fullscreen. */
    private static final long MAX_FULLSCREEN_LOAD_DELAY_MS = 3000;

    private static boolean sDisableLoadingCheck;

    protected final TabImpl mTab;
    private WebContents mWebContents;

    private boolean mIsFullscreenWaitingForLoad;

    /**
     * Basic constructor.
     * @param tab The associated {@link Tab}.
     */
    public TabStateBrowserControlsVisibilityDelegate(Tab tab) {
        super(BrowserControlsState.BOTH);

        mTab = (TabImpl) tab;

        mTab.addObserver(new EmptyTabObserver() {
            @SuppressLint("HandlerLeak")
            private Handler mHandler = new Handler() {
                @Override
                public void handleMessage(Message msg) {
                    if (msg == null) return;
                    if (msg.what == MSG_ID_ENABLE_FULLSCREEN_AFTER_LOAD) {
                        if (!mIsFullscreenWaitingForLoad) return;
                        updateWaitingForLoad(false);
                    }
                }
            };

            private long getLoadDelayMs() {
                return sDisableLoadingCheck ? 0 : MAX_FULLSCREEN_LOAD_DELAY_MS;
            }

            private void cancelEnableFullscreenLoadDelay() {
                mHandler.removeMessages(MSG_ID_ENABLE_FULLSCREEN_AFTER_LOAD);
                updateWaitingForLoad(false);
            }

            private void scheduleEnableFullscreenLoadDelayIfNecessary() {
                if (mIsFullscreenWaitingForLoad
                        && !mHandler.hasMessages(MSG_ID_ENABLE_FULLSCREEN_AFTER_LOAD)) {
                    mHandler.sendEmptyMessageDelayed(
                            MSG_ID_ENABLE_FULLSCREEN_AFTER_LOAD, getLoadDelayMs());
                }
            }

            @Override
            public void onContentChanged(Tab tab) {
                onWebContentsUpdated(tab.getWebContents());
            }

            @Override
            public void onWebContentsSwapped(Tab tab, boolean didStartLoad, boolean didFinishLoad) {
                if (!didStartLoad) return;

                // As we may have missed the main frame commit notification for the
                // swapped web contents, schedule the enabling of fullscreen now.
                scheduleEnableFullscreenLoadDelayIfNecessary();
            }

            @Override
            public void onDidFinishNavigation(Tab tab, NavigationHandle navigation) {
                if (!navigation.hasCommitted() || !navigation.isInMainFrame()) return;
                mHandler.removeMessages(MSG_ID_ENABLE_FULLSCREEN_AFTER_LOAD);
                mHandler.sendEmptyMessageDelayed(
                        MSG_ID_ENABLE_FULLSCREEN_AFTER_LOAD, getLoadDelayMs());
            }

            @Override
            public void onPageLoadStarted(Tab tab, String url) {
                mHandler.removeMessages(MSG_ID_ENABLE_FULLSCREEN_AFTER_LOAD);
                updateWaitingForLoad(!DomDistillerUrlUtils.isDistilledPage(url));
            }

            @Override
            public void onPageLoadFinished(Tab tab, String url) {
                // Handle the case where a commit or prerender swap notification failed to arrive
                // and the enable fullscreen message was never enqueued.
                scheduleEnableFullscreenLoadDelayIfNecessary();
            }

            @Override
            public void onPageLoadFailed(Tab tab, int errorCode) {
                cancelEnableFullscreenLoadDelay();
            }

            @Override
            public void onHidden(Tab tab, @TabHidingType int type) {
                cancelEnableFullscreenLoadDelay();
            }

            @Override
            public void onSSLStateUpdated(Tab tab) {
                updateVisibilityConstraints();
            }

            @Override
            public void onRendererResponsiveStateChanged(Tab tab, boolean isResponsive) {
                updateVisibilityConstraints();
            }

            @Override
            public void onDidAttachInterstitialPage(Tab tab) {
                updateVisibilityConstraints();
            }

            @Override
            public void onDidDetachInterstitialPage(Tab tab) {
                updateVisibilityConstraints();
            }

            @Override
            public void onShown(Tab tab, int type) {
                updateVisibilityConstraints();
            }

            @Override
            public void onActivityAttachmentChanged(Tab tab, boolean isAttached) {
                if (isAttached) updateVisibilityConstraints();
            }

            @Override
            public void onDestroyed(Tab tab) {
                super.onDestroyed(tab);

                // Remove pending handler actions to prevent memory leaks.
                mHandler.removeCallbacksAndMessages(null);
            }
        });
        onWebContentsUpdated(mTab.getWebContents());
    }

    private void onWebContentsUpdated(WebContents contents) {
        if (mWebContents == contents) return;
        mWebContents = contents;
        if (mWebContents == null) return;
        ImeAdapter.fromWebContents(mWebContents).addEventObserver(this);
    }

    private void updateWaitingForLoad(boolean waiting) {
        if (mIsFullscreenWaitingForLoad == waiting) return;
        mIsFullscreenWaitingForLoad = waiting;
        updateVisibilityConstraints();
    }

    private boolean enableHidingBrowserControls() {
        if (ChromeFeatureList.isEnabled(ChromeFeatureList.DONT_AUTO_HIDE_BROWSER_CONTROLS)
                && mTab.getActivity() != null && mTab.getActivity().getToolbarManager() != null
                && mTab.getActivity().getToolbarManager().getBottomToolbarCoordinator() != null) {
            return false;
        }

        WebContents webContents = mTab.getWebContents();
        if (webContents == null || webContents.isDestroyed()) return false;

        String url = mTab.getUrl();
        boolean enableHidingBrowserControls = url != null;
        enableHidingBrowserControls &= !url.startsWith(UrlConstants.CHROME_URL_PREFIX);
        enableHidingBrowserControls &= !url.startsWith(UrlConstants.CHROME_NATIVE_URL_PREFIX);

        int securityState = mTab.getSecurityLevel();
        enableHidingBrowserControls &= (securityState != ConnectionSecurityLevel.DANGEROUS);

        enableHidingBrowserControls &=
                !SelectionPopupController.fromWebContents(webContents).isFocusedNodeEditable();
        enableHidingBrowserControls &= !mTab.isShowingErrorPage();
        enableHidingBrowserControls &= !webContents.isShowingInterstitialPage();
        enableHidingBrowserControls &= !mTab.isRendererUnresponsive();
        enableHidingBrowserControls &= !mTab.isHidden();
        enableHidingBrowserControls &= !mIsFullscreenWaitingForLoad;

        // TODO(tedchoc): AccessibilityUtil and DeviceClassManager checks do not belong in Tab
        //                logic.  They should be moved to application level checks.
        enableHidingBrowserControls &= !AccessibilityUtil.isAccessibilityEnabled();
        enableHidingBrowserControls &= DeviceClassManager.enableFullscreen();

        return enableHidingBrowserControls;
    }

    /**
     * @return The constraints that determine the visibility of the browser controls.
     */
    protected @BrowserControlsState int calculateVisibilityConstraints() {
        return enableHidingBrowserControls() ? BrowserControlsState.BOTH
                                             : BrowserControlsState.SHOWN;
    }

    /**
     * Updates the browser controls visibility constraints based on the current configuration.
     */
    protected void updateVisibilityConstraints() {
        set(calculateVisibilityConstraints());
    }

    /**
     * Disables the logic that prevents hiding the top controls during page load for testing.
     */
    public static void disablePageLoadDelayForTests() {
        sDisableLoadingCheck = true;
    }

    // ImeEventObserver

    @Override
    public void onNodeAttributeUpdated(boolean editable, boolean password) {
        updateVisibilityConstraints();
    }
}
