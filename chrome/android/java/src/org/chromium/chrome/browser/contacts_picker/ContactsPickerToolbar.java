// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.contacts_picker;

import android.content.Context;
import android.support.v4.widget.ImageViewCompat;
import android.support.v7.widget.AppCompatImageView;
import android.util.AttributeSet;

import org.chromium.base.ApiCompatibilityUtils;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.widget.selection.SelectableListToolbar;
import org.chromium.chrome.browser.widget.selection.SelectionDelegate;
import org.chromium.ui.widget.ButtonCompat;

import java.util.List;

/**
 * Handles toolbar functionality for the {@ContactsPickerDialog}.
 */
public class ContactsPickerToolbar extends SelectableListToolbar<ContactDetails> {
    /**
     * A delegate that handles dialog actions.
     */
    public interface ContactsToolbarDelegate {
        /**
         * Called when the back arrow is clicked in the toolbar.
         */
        void onNavigationBackCallback();
    }

    // A delegate to notify when the dialog should close.
    private ContactsToolbarDelegate mDelegate;

    // Whether any filter chips are selected. Default to true because all filter chips are selected
    // by default when opening the dialog.
    private boolean mFilterChipsSelected = true;

    public ContactsPickerToolbar(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    /**
     * Set the {@ContactToolbarDelegate} for this toolbar.
     */
    public void setDelegate(ContactsToolbarDelegate delegate) {
        mDelegate = delegate;
    }

    /**
     * Shows the Back arrow navigation button in the upper left corner.
     */
    public void showBackArrow() {
        setNavigationButton(NAVIGATION_BUTTON_BACK);
    }

    /**
     * Sets whether any filter chips are |selected| in the dialog.
     */
    public void setFilterChipsSelected(boolean selected) {
        mFilterChipsSelected = selected;
        updateToolbarUI();
    }

    // SelectableListToolbar:

    @Override
    public void onNavigationBack() {
        if (isSearching()) {
            super.onNavigationBack();
        } else {
            mDelegate.onNavigationBackCallback();
        }
    }

    @Override
    public void initialize(SelectionDelegate<ContactDetails> delegate, int titleResId,
            int normalGroupResId, int selectedGroupResId, boolean updateStatusBarColor) {
        super.initialize(
                delegate, titleResId, normalGroupResId, selectedGroupResId, updateStatusBarColor);

        showBackArrow();
    }

    @Override
    public void onSelectionStateChange(List<ContactDetails> selectedItems) {
        super.onSelectionStateChange(selectedItems);
        updateToolbarUI();
    }

    /**
     * Update the UI elements of the toolbar, based on whether contacts & filter chips are selected.
     */
    private void updateToolbarUI() {
        boolean contactsSelected = !mSelectionDelegate.getSelectedItems().isEmpty();

        boolean doneEnabled = contactsSelected && mFilterChipsSelected;
        ButtonCompat done = findViewById(R.id.done);
        done.setEnabled(doneEnabled);

        AppCompatImageView search = findViewById(R.id.search);
        ImageViewCompat.setImageTintList(search,
                useDarkIcons() ? getDarkIconColorStateList() : getLightIconColorStateList());

        if (doneEnabled) {
            ApiCompatibilityUtils.setTextAppearance(done, R.style.TextAppearance_Body_Inverse);
        } else {
            ApiCompatibilityUtils.setTextAppearance(
                    done, R.style.TextAppearance_BlackDisabledText3);
            if (contactsSelected) {
                setNavigationButton(NAVIGATION_BUTTON_SELECTION_BACK);
            } else {
                showBackArrow();
            }
        }
    }
}
