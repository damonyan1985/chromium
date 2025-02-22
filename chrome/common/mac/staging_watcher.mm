// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/common/mac/staging_watcher.h"

#include "base/mac/bundle_locations.h"
#include "base/mac/foundation_util.h"
#include "base/mac/mac_util.h"
#include "base/mac/scoped_block.h"
#include "base/mac/scoped_nsobject.h"

// Best documentation / Is unofficial documentation
//
// Required reading for CFPreferences/NSUserDefaults is at
// <http://dscoder.com/defaults.html>, a post by David "Catfish Man" Smith, who
// re-wrote NSUserDefaults for 10.12 and iPad Classroom support. It is important
// to note that KVO only notifies for changes made by other programs starting
// with that rewrite in 10.12. In macOS 10.11 and earlier, polling is the only
// option. Note that NSUserDefaultsDidChangeNotification never notifies about
// changes made by other programs, not even in 10.12 and later.
//
// On the other hand, KVO notification was broken in the NSUserDefaults rewrite
// for 10.14; see:
// - https://twitter.com/Catfish_Man/status/1116185288257105925
// - rdar://49812220
// The bug is that a change from "no value" to "value present" doesn't notify.
// To work around that, a default is registered to ensure that there never is
// a "no value" situation.

namespace {

NSString* const kStagingKey = @"UpdatePending";

}  // namespace

@interface CrStagingKeyWatcher () {
  base::scoped_nsobject<NSUserDefaults> _defaults;
  NSTimeInterval _pollingTime;
  base::scoped_nsobject<NSTimer> _pollingTimer;
  BOOL _observing;
  base::mac::ScopedBlock<StagingKeyChangedObserver> _callback;
  BOOL _lastStagingKeyValue;

  BOOL _lastWaitWasBlockedForTesting;
}

+ (NSString*)stagingLocationWithUserDefaults:(NSUserDefaults*)defaults;

@end

@implementation CrStagingKeyWatcher

- (instancetype)initWithPollingTime:(NSTimeInterval)pollingTime {
  return [self initWithUserDefaults:[NSUserDefaults standardUserDefaults]
                        pollingTime:pollingTime
               disableKVOForTesting:NO];
}

- (instancetype)initWithUserDefaults:(NSUserDefaults*)defaults
                         pollingTime:(NSTimeInterval)pollingTime
                disableKVOForTesting:(BOOL)disableKVOForTesting {
  if ((self = [super init])) {
    _pollingTime = pollingTime;
    _defaults.reset(defaults, base::scoped_policy::RETAIN);
    [_defaults registerDefaults:@{kStagingKey : @[]}];
    _lastStagingKeyValue = [self isStagingKeySet];
    if (base::mac::IsAtLeastOS10_12() && !disableKVOForTesting) {
      // If a change is made in another process (which is the use case here),
      // the prior value is never provided in the observation callback change
      // dictionary, whether or not NSKeyValueObservingOptionPrior is specified.
      // Therefore, pass in 0 for the NSKeyValueObservingOptions and rely on
      // keeping the previous value in |lastStagingKeyValue_|.
      [_defaults addObserver:self
                  forKeyPath:kStagingKey
                     options:0
                     context:nullptr];
      _observing = YES;
    }
  }
  return self;
}

+ (NSString*)stagingLocationWithUserDefaults:(NSUserDefaults*)defaults {
  NSDictionary<NSString*, id>* stagedPathPairs =
      [defaults dictionaryForKey:kStagingKey];
  if (!stagedPathPairs)
    return nil;

  NSString* appPath = [base::mac::OuterBundle() bundlePath];

  return base::mac::ObjCCast<NSString>([stagedPathPairs objectForKey:appPath]);
}

- (BOOL)isStagingKeySet {
  return [self stagingLocation] != nil;
}

+ (BOOL)isStagingKeySet {
  return [self stagingLocation] != nil;
}

- (NSString*)stagingLocation {
  return [CrStagingKeyWatcher stagingLocationWithUserDefaults:_defaults];
}

+ (NSString*)stagingLocation {
  return [self
      stagingLocationWithUserDefaults:[NSUserDefaults standardUserDefaults]];
}

- (void)waitForStagingKeyToClear {
  if (![self isStagingKeySet]) {
    _lastWaitWasBlockedForTesting = NO;
    return;
  }

  NSRunLoop* runloop = [NSRunLoop currentRunLoop];
  if (_observing) {
    _callback.reset(
        ^(BOOL stagingKeySet) {
          CFRunLoopStop([runloop getCFRunLoop]);
        },
        base::scoped_policy::RETAIN);

    while ([self isStagingKeySet] && [runloop runMode:NSDefaultRunLoopMode
                                           beforeDate:[NSDate distantFuture]]) {
      /* run! */
    }
  } else {
    while ([self isStagingKeySet] &&
           [runloop
                  runMode:NSDefaultRunLoopMode
               beforeDate:[NSDate dateWithTimeIntervalSinceNow:_pollingTime]]) {
      /* run! */
    }
  }

  _lastWaitWasBlockedForTesting = YES;
}

- (void)setStagingKeyChangedObserver:(StagingKeyChangedObserver)block {
  _callback.reset(block, base::scoped_policy::RETAIN);

  if (_observing) {
    // Nothing to be done; the observation is already started.
  } else {
    _pollingTimer.reset(
        [NSTimer scheduledTimerWithTimeInterval:_pollingTime
                                         target:self
                                       selector:@selector(timerFired:)
                                       userInfo:nil
                                        repeats:YES],
        base::scoped_policy::RETAIN);
  }
}

- (void)timerFired:(NSTimer*)timer {
  [self observeValueForKeyPath:nil ofObject:nil change:nil context:nil];
}

- (void)dealloc {
  if (_observing)
    [_defaults removeObserver:self forKeyPath:kStagingKey context:nullptr];
  if (_pollingTimer)
    [_pollingTimer invalidate];

  [super dealloc];
}

- (BOOL)lastWaitWasBlockedForTesting {
  return _lastWaitWasBlockedForTesting;
}

+ (NSString*)stagingKeyForTesting {
  return kStagingKey;
}

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary*)change
                       context:(void*)context {
  BOOL isStagingKeySet = [self isStagingKeySet];
  if (isStagingKeySet == _lastStagingKeyValue)
    return;

  _lastStagingKeyValue = isStagingKeySet;
  _callback.get()([self isStagingKeySet]);
}

@end
