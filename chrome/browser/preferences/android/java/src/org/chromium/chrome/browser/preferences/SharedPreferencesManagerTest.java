// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.preferences;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;

import android.support.test.filters.SmallTest;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.Mock;
import org.mockito.MockitoAnnotations;

import org.chromium.base.test.BaseRobolectricTestRunner;

import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.Set;

/**
 * Unit tests for {@link SharedPreferencesManager}.
 */
@RunWith(BaseRobolectricTestRunner.class)
public class SharedPreferencesManagerTest {
    @Mock
    private ChromePreferenceKeyChecker mChecker;

    private SharedPreferencesManager mSubject;

    @Before
    public void setUp() {
        MockitoAnnotations.initMocks(this);
        mSubject = new SharedPreferencesManager(mChecker);
    }

    @Test
    @SmallTest
    public void testWriteReadInt() {
        // Verify default return values when no value is written.
        assertEquals(0, mSubject.readInt("int_key"));
        assertEquals(987, mSubject.readInt("int_key", 987));
        assertFalse(mSubject.contains("int_key"));

        // Write a value.
        mSubject.writeInt("int_key", 123);

        // Verify value written can be read.
        assertEquals(123, mSubject.readInt("int_key"));
        assertEquals(123, mSubject.readInt("int_key", 987));
        assertTrue(mSubject.contains("int_key"));

        // Remove the value.
        mSubject.removeKey("int_key");

        // Verify the removed value is not returned anymore.
        assertEquals(0, mSubject.readInt("int_key"));
        assertFalse(mSubject.contains("int_key"));
    }

    @Test
    @SmallTest
    public void testIncrementInt() {
        mSubject.writeInt("int_key", 100);
        mSubject.incrementInt("int_key");

        assertEquals(101, mSubject.readInt("int_key"));
    }

    @Test
    @SmallTest
    public void testIncrementIntDefault() {
        mSubject.incrementInt("int_key");

        assertEquals(1, mSubject.readInt("int_key"));
    }

    @Test
    @SmallTest
    public void testWriteReadBoolean() {
        // Verify default return values when no value is written.
        assertEquals(false, mSubject.readBoolean("bool_key", false));
        assertEquals(true, mSubject.readBoolean("bool_key", true));
        assertFalse(mSubject.contains("bool_key"));

        // Write a value.
        mSubject.writeBoolean("bool_key", true);

        // Verify value written can be read.
        assertEquals(true, mSubject.readBoolean("bool_key", false));
        assertEquals(true, mSubject.readBoolean("bool_key", true));
        assertTrue(mSubject.contains("bool_key"));

        // Remove the value.
        mSubject.removeKey("bool_key");

        // Verify the removed value is not returned anymore.
        assertEquals(false, mSubject.readBoolean("bool_key", false));
        assertEquals(true, mSubject.readBoolean("bool_key", true));
        assertFalse(mSubject.contains("bool_key"));
    }

    @Test
    @SmallTest
    public void testWriteReadLong() {
        // Verify default return values when no value is written.
        assertEquals(0, mSubject.readLong("long_key"));
        assertEquals(9876543210L, mSubject.readLong("long_key", 9876543210L));
        assertFalse(mSubject.contains("long_key"));

        // Write a value.
        mSubject.writeLong("long_key", 9999999999L);

        // Verify value written can be read.
        assertEquals(9999999999L, mSubject.readLong("long_key"));
        assertEquals(9999999999L, mSubject.readLong("long_key", 9876543210L));
        assertTrue(mSubject.contains("long_key"));

        // Remove the value.
        mSubject.removeKey("long_key");

        // Verify the removed value is not returned anymore.
        assertEquals(0, mSubject.readLong("long_key"));
        assertFalse(mSubject.contains("long_key"));
    }

    @Test
    @SmallTest
    public void testWriteReadStringSet() {
        Set<String> defaultStringSet = new HashSet<>(Arrays.asList("a", "b", "c"));
        Set<String> exampleStringSet = new HashSet<>(Arrays.asList("d", "e"));

        // Verify default return values when no value is written.
        assertEquals(Collections.emptySet(), mSubject.readStringSet("string_set_key"));
        assertEquals(defaultStringSet, mSubject.readStringSet("string_set_key", defaultStringSet));
        assertNull(mSubject.readStringSet("string_set_key", null));
        assertFalse(mSubject.contains("string_set_key"));

        // Write a value.
        mSubject.writeStringSet("string_set_key", exampleStringSet);

        // Verify value written can be read.
        assertEquals(exampleStringSet, mSubject.readStringSet("string_set_key"));
        assertEquals(exampleStringSet, mSubject.readStringSet("string_set_key", defaultStringSet));
        assertEquals(exampleStringSet, mSubject.readStringSet("string_set_key", null));
        assertTrue(mSubject.contains("string_set_key"));

        // Remove the value.
        mSubject.removeKey("string_set_key");

        // Verify the removed value is not returned anymore.
        assertEquals(Collections.emptySet(), mSubject.readStringSet("string_set_key"));
        assertFalse(mSubject.contains("string_set_key"));
    }

    @Test
    @SmallTest
    public void testAddToStringSet() {
        mSubject.writeStringSet("string_set_key", new HashSet<>(Collections.singletonList("bar")));
        mSubject.addToStringSet("string_set_key", "foo");

        assertEquals(new HashSet<>(Arrays.asList("foo", "bar")),
                mSubject.readStringSet("string_set_key"));
    }

    @Test
    @SmallTest
    public void testAddToStringSetDefault() {
        mSubject.addToStringSet("string_set_key", "foo");

        assertEquals(new HashSet<>(Collections.singletonList("foo")),
                mSubject.readStringSet("string_set_key"));
    }

    @Test
    @SmallTest
    public void testRemoveFromStringSet() {
        mSubject.writeStringSet("string_set_key", new HashSet<>(Arrays.asList("foo", "bar")));
        mSubject.removeFromStringSet("string_set_key", "foo");

        assertEquals(new HashSet<>(Collections.singletonList("bar")),
                mSubject.readStringSet("string_set_key"));
    }

    @Test
    @SmallTest
    public void testRemoveFromStringSetDefault() {
        mSubject.removeFromStringSet("string_set_key", "foo");

        assertEquals(Collections.emptySet(), mSubject.readStringSet("string_set_key"));
    }

    @Test
    @SmallTest
    public void testCheckerIsCalled() {
        mSubject.writeInt("int_key", 123);
        verify(mChecker, times(1)).checkIsKeyInUse("int_key");
        mSubject.readInt("int_key");
        verify(mChecker, times(2)).checkIsKeyInUse("int_key");
        mSubject.incrementInt("int_key");
        verify(mChecker, times(3)).checkIsKeyInUse("int_key");

        mSubject.writeBoolean("bool_key", true);
        verify(mChecker, times(1)).checkIsKeyInUse("bool_key");
        mSubject.readBoolean("bool_key", false);
        verify(mChecker, times(2)).checkIsKeyInUse("bool_key");

        mSubject.writeString("string_key", "foo");
        verify(mChecker, times(1)).checkIsKeyInUse("string_key");
        mSubject.readString("string_key", "");
        verify(mChecker, times(2)).checkIsKeyInUse("string_key");

        mSubject.writeLong("long_key", 999L);
        verify(mChecker, times(1)).checkIsKeyInUse("long_key");
        mSubject.readLong("long_key");
        verify(mChecker, times(2)).checkIsKeyInUse("long_key");

        mSubject.writeStringSet("string_set_key", new HashSet<>());
        verify(mChecker, times(1)).checkIsKeyInUse("string_set_key");
        mSubject.readStringSet("string_set_key");
        verify(mChecker, times(2)).checkIsKeyInUse("string_set_key");
        mSubject.addToStringSet("string_set_key", "bar");
        verify(mChecker, times(3)).checkIsKeyInUse("string_set_key");
        mSubject.removeFromStringSet("string_set_key", "bar");
        verify(mChecker, times(4)).checkIsKeyInUse("string_set_key");

        mSubject.removeKey("some_key");
        verify(mChecker, times(1)).checkIsKeyInUse("some_key");
        mSubject.contains("some_key");
        verify(mChecker, times(2)).checkIsKeyInUse("some_key");
    }

    private static class TestObserver implements SharedPreferencesManager.Observer {
        int mEventCount;

        @Override
        public void onPreferenceChanged(String key) {
            mEventCount++;
        }
    }

    @Test
    @SmallTest
    public void testObserver() {
        TestObserver observer = new TestObserver();
        mSubject.addObserver(observer);

        int expectedEventCount = 0;

        // Each write should issue an event.
        mSubject.writeInt("int_key", 99);
        expectedEventCount++;
        assertEquals(expectedEventCount, observer.mEventCount);

        mSubject.writeInt("int_key", 88);
        expectedEventCount++;
        assertEquals(expectedEventCount, observer.mEventCount);

        mSubject.incrementInt("int_key");
        expectedEventCount++;
        assertEquals(expectedEventCount, observer.mEventCount);

        // Reads should not trigger events.
        mSubject.readInt("int_key");
        assertEquals(expectedEventCount, observer.mEventCount);

        // Removing a key should trigger an event.
        mSubject.removeKey("int_key");
        expectedEventCount++;
        assertEquals(expectedEventCount, observer.mEventCount);

        // Modifying any key should trigger an event.
        mSubject.writeString("string_key", "foo");
        expectedEventCount++;
        assertEquals(expectedEventCount, observer.mEventCount);

        // After removing the observer, it should stop getting events.
        mSubject.removeObserver(observer);
        mSubject.writeString("string_key", "bar");
        assertEquals(expectedEventCount, observer.mEventCount);
    }

    @Test
    @SmallTest
    public void testPrefsAreWipedBetweenTests_1() {
        doTestPrefsAreWipedBetweenTests();
    }

    @Test
    @SmallTest
    public void testPrefsAreWipedBetweenTests_2() {
        doTestPrefsAreWipedBetweenTests();
    }

    /**
     * {@link #testPrefsAreWipedBetweenTests_1()} and {@link #testPrefsAreWipedBetweenTests_2()}
     * each set the same preference and fail if it has been set previously. Whichever order these
     * tests are run, either will fail if the prefs are not wiped between tests.
     */
    private void doTestPrefsAreWipedBetweenTests() {
        // Disable key checking for this test because "dirty_pref" isn't registered in the "in use"
        // list.
        BaseChromePreferenceKeyChecker checkerHeld =
                SharedPreferencesManager.getInstance().swapKeyCheckerForTesting(
                        new BaseChromePreferenceKeyChecker());

        try {
            // If the other test has set this flag and it was not wiped out, fail.
            assertFalse(SharedPreferencesManager.getInstance().readBoolean("dirty_pref", false));

            // Set the flag so the other test ensures it was wiped out.
            SharedPreferencesManager.getInstance().writeBoolean("dirty_pref", true);
        } finally {
            // Restore the key checker.
            SharedPreferencesManager.getInstance().swapKeyCheckerForTesting(checkerHeld);
        }
    }
}
