// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.tab;

import org.chromium.base.ObservableSupplierImpl;
import org.chromium.content_public.common.BrowserControlsState;

/**
 * A delegate to determine visibility of the browser controls.
 */
public abstract class BrowserControlsVisibilityDelegate extends ObservableSupplierImpl<Integer> {
    /**
     * Constructs a delegate that controls the visibility of the browser controls.
     * @param initialValue The initial browser state visibility.
     */
    protected BrowserControlsVisibilityDelegate(@BrowserControlsState int initialValue) {
        set(initialValue);
    }

    @Override
    public void set(@BrowserControlsState Integer value) {
        assert value != null;
        super.set(value);
    }
}
