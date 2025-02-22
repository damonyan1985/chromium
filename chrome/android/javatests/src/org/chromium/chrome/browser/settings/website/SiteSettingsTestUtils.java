// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.settings.website;

import android.content.Intent;
import android.os.Bundle;
import android.support.test.InstrumentationRegistry;

import org.chromium.chrome.browser.settings.SettingsActivity;
import org.chromium.chrome.browser.settings.SettingsLauncher;

/**
 * Util functions for testing SiteSettings functionality.
 */
public class SiteSettingsTestUtils {
    public static SettingsActivity startSiteSettingsMenu(String category) {
        Bundle fragmentArgs = new Bundle();
        fragmentArgs.putString(SingleCategorySettings.EXTRA_CATEGORY, category);
        Intent intent = SettingsLauncher.createIntentForSettingsPage(
                InstrumentationRegistry.getTargetContext(), SiteSettings.class.getName(),
                fragmentArgs);
        return (SettingsActivity) InstrumentationRegistry.getInstrumentation().startActivitySync(
                intent);
    }

    public static SettingsActivity startSiteSettingsCategory(@SiteSettingsCategory.Type int type) {
        Bundle fragmentArgs = new Bundle();
        fragmentArgs.putString(
                SingleCategorySettings.EXTRA_CATEGORY, SiteSettingsCategory.preferenceKey(type));
        Intent intent = SettingsLauncher.createIntentForSettingsPage(
                InstrumentationRegistry.getTargetContext(), SingleCategorySettings.class.getName(),
                fragmentArgs);
        return (SettingsActivity) InstrumentationRegistry.getInstrumentation().startActivitySync(
                intent);
    }

    public static SettingsActivity startSingleWebsitePreferences(Website site) {
        Bundle fragmentArgs = new Bundle();
        fragmentArgs.putSerializable(SingleWebsiteSettings.EXTRA_SITE, site);
        Intent intent = SettingsLauncher.createIntentForSettingsPage(
                InstrumentationRegistry.getTargetContext(), SingleWebsiteSettings.class.getName(),
                fragmentArgs);
        return (SettingsActivity) InstrumentationRegistry.getInstrumentation().startActivitySync(
                intent);
    }
}
