// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.toolbar.top;

import static android.support.test.espresso.Espresso.onView;
import static android.support.test.espresso.matcher.ViewMatchers.withId;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import static org.chromium.chrome.browser.tasks.tab_management.TabUiTestHelper.enterTabSwitcher;
import static org.chromium.chrome.browser.tasks.tab_management.TabUiTestHelper.rotateDeviceToOrientation;
import static org.chromium.chrome.browser.tasks.tab_management.TabUiTestHelper.verifyTabSwitcherCardCount;

import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.support.test.filters.MediumTest;
import android.view.View;

import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TestRule;
import org.junit.runner.RunWith;

import org.chromium.base.test.util.CommandLineFlags;
import org.chromium.base.test.util.Restriction;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.ChromeFeatureList;
import org.chromium.chrome.browser.ChromeSwitches;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.compositor.layouts.Layout;
import org.chromium.chrome.browser.flags.FeatureUtilities;
import org.chromium.chrome.features.start_surface.StartSurfaceLayout;
import org.chromium.chrome.test.ChromeJUnit4ClassRunner;
import org.chromium.chrome.test.ChromeTabbedActivityTestRule;
import org.chromium.chrome.test.util.browser.Features;
import org.chromium.content_public.browser.test.util.CriteriaHelper;
import org.chromium.ui.test.util.UiRestriction;

/** End-to-end tests for adaptive toolbar. */
@RunWith(ChromeJUnit4ClassRunner.class)
// clang-format off
@CommandLineFlags.Add({ChromeSwitches.DISABLE_FIRST_RUN_EXPERIENCE})
@Restriction(UiRestriction.RESTRICTION_TYPE_PHONE)
public class AdaptiveToolbarTest {
    // Params to turn off new tab variation in GTS.
    private static final String NO_NEW_TAB_VARIATION_PARAMS = "force-fieldtrial-params=" +
            "Study.Group:tab_grid_layout_android_new_tab/false";
    // clang-format on
    @Rule
    public ChromeTabbedActivityTestRule mActivityTestRule = new ChromeTabbedActivityTestRule();

    @Rule
    public TestRule mProcessor = new Features.InstrumentationProcessor();

    @Before
    public void setUp() {
        FeatureUtilities.setTabGroupsAndroidEnabledForTesting(false);
    }

    @After
    public void tearDown() {
        FeatureUtilities.setGridTabSwitcherEnabledForTesting(null);
        FeatureUtilities.setIsBottomToolbarEnabledForTesting(null);
        mActivityTestRule.getActivity().setRequestedOrientation(
                ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
    }

    private void setupFlagsAndLaunchActivity(
            boolean isBottomToolbarEnabled, boolean isGridTabSwitcherEnabled) {
        FeatureUtilities.setGridTabSwitcherEnabledForTesting(isGridTabSwitcherEnabled);
        FeatureUtilities.setIsBottomToolbarEnabledForTesting(isBottomToolbarEnabled);
        mActivityTestRule.startMainActivityFromLauncher();
        CriteriaHelper.pollUiThread(mActivityTestRule.getActivity()
                                            .getTabModelSelector()
                                            .getTabModelFilterProvider()
                                            .getCurrentTabModelFilter()::isTabModelRestored);
    }

    @Test
    @MediumTest
    // clang-format off
    @CommandLineFlags.Add({"enable-features=" + ChromeFeatureList.TAB_GRID_LAYOUT_ANDROID +
            "<Study", "force-fieldtrials=Study/Group", NO_NEW_TAB_VARIATION_PARAMS})
    public void testTopToolbar_WithGTS_WithBottomToolbar() throws InterruptedException {
        // clang-format on
        setupFlagsAndLaunchActivity(true, true);
        final ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        Layout layout = cta.getLayoutManager().getOverviewLayout();
        assertTrue(layout instanceof StartSurfaceLayout);
        enterTabSwitcher(cta);
        verifyTabSwitcherCardCount(cta, 1);

        checkTopToolbarButtonsExistence(true);
        checkTopToolbarButtonsVisibility(false);

        rotateDeviceToOrientation(cta, Configuration.ORIENTATION_LANDSCAPE);

        checkTopToolbarButtonsExistence(true);
        checkTopToolbarButtonsVisibility(true);
    }

    @Test
    @MediumTest
    // clang-format off
    @CommandLineFlags.Add({"enable-features=" + ChromeFeatureList.TAB_GRID_LAYOUT_ANDROID +
            "<Study", "force-fieldtrials=Study/Group", NO_NEW_TAB_VARIATION_PARAMS})
    public void testTopToolbar_WithGTS_WithoutBottomToolbar() throws InterruptedException {
        // clang-format on
        setupFlagsAndLaunchActivity(false, true);
        final ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        Layout layout = cta.getLayoutManager().getOverviewLayout();
        assertTrue(layout instanceof StartSurfaceLayout);
        enterTabSwitcher(cta);
        verifyTabSwitcherCardCount(cta, 1);

        checkTopToolbarButtonsExistence(true);
        checkTopToolbarButtonsVisibility(true);

        rotateDeviceToOrientation(cta, Configuration.ORIENTATION_LANDSCAPE);

        checkTopToolbarButtonsExistence(true);
        checkTopToolbarButtonsVisibility(true);
    }

    @Test
    @MediumTest
    public void testTopToolbar_WithoutGTS_WithBottomToolbar() throws InterruptedException {
        setupFlagsAndLaunchActivity(true, false);
        final ChromeTabbedActivity cta = mActivityTestRule.getActivity();
        Layout layout = cta.getLayoutManager().getOverviewLayout();
        assertFalse(layout instanceof StartSurfaceLayout);
        enterTabSwitcher(cta);

        checkTopToolbarButtonsExistence(false);

        rotateDeviceToOrientation(cta, Configuration.ORIENTATION_LANDSCAPE);

        checkTopToolbarButtonsExistence(false);
    }

    private void checkTopToolbarButtonsExistence(boolean isNotNull) {
        onView(withId(R.id.tab_switcher_toolbar)).check((v, noMatchingViewException) -> {
            View newTabButton = v.findViewById(R.id.new_tab_button);
            View menuButton = v.findViewById(R.id.menu_button_wrapper);
            if (isNotNull) {
                assertNotNull(newTabButton);
                assertNotNull(menuButton);
            } else {
                assertNull(newTabButton);
                assertNull(menuButton);
            }
        });
    }

    private void checkTopToolbarButtonsVisibility(boolean isVisible) {
        int visibility = isVisible ? View.VISIBLE : View.GONE;
        onView(withId(R.id.tab_switcher_toolbar)).check((v, noMatchingViewException) -> {
            View newTabButton = v.findViewById(R.id.new_tab_button);
            View menuButton = v.findViewById(R.id.menu_button_wrapper);
            assertEquals(visibility, newTabButton.getVisibility());
            assertEquals(visibility, menuButton.getVisibility());
        });
    }
}
