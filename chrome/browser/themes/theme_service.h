// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_THEMES_THEME_SERVICE_H_
#define CHROME_BROWSER_THEMES_THEME_SERVICE_H_

#include <map>
#include <memory>
#include <string>

#include "base/compiler_specific.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observer.h"
#include "base/sequence_checker.h"
#include "base/task/cancelable_task_tracker.h"
#include "chrome/common/buildflags.h"
#include "components/keyed_service/core/keyed_service.h"
#include "content/public/browser/notification_observer.h"
#include "content/public/browser/notification_registrar.h"
#include "extensions/buildflags/buildflags.h"
#include "extensions/common/extension_id.h"
#include "ui/base/theme_provider.h"
#include "ui/native_theme/native_theme.h"
#include "ui/native_theme/native_theme_observer.h"

class BrowserThemePack;
class CustomThemeSupplier;
class ThemeSyncableService;
class Profile;

namespace color_utils {
struct HSL;
}

namespace extensions {
class Extension;
}

namespace gfx {
class Image;
}

namespace theme_service_internal {
class ThemeServiceTest;
}

class ThemeService : public content::NotificationObserver,
                     public KeyedService,
                     public ui::NativeThemeObserver {
 public:
  // This class keeps track of the number of existing |ThemeReinstaller|
  // objects. When that number reaches 0 then unused themes will be deleted.
  class ThemeReinstaller {
   public:
    ThemeReinstaller(Profile* profile, base::OnceClosure installer);
    ~ThemeReinstaller();

    void Reinstall();

   private:
    base::OnceClosure installer_;
    ThemeService* const theme_service_;

    DISALLOW_COPY_AND_ASSIGN(ThemeReinstaller);
  };

  // Public constants used in ThemeService and its subclasses:
  static const char kDefaultThemeID[];

  // Constant ID to use for all autogenerated themes.
  static const char kAutogeneratedThemeID[];

  explicit ThemeService(Profile* profile);
  ~ThemeService() override;

  void Init();

  // KeyedService:
  void Shutdown() override;

  // Overridden from content::NotificationObserver:
  void Observe(int type,
               const content::NotificationSource& source,
               const content::NotificationDetails& details) override;

  // Overridden from ui::NativeThemeObserver:
  void OnNativeThemeUpdated(ui::NativeTheme* observed_theme) override;

  // Set the current theme to the theme defined in |extension|.
  // |extension| must already be added to this profile's
  // ExtensionService.
  void SetTheme(const extensions::Extension* extension);

  // Similar to SetTheme, but doesn't show an undo infobar.
  void RevertToExtensionTheme(const std::string& extension_id);

  // Reset the theme to default.
  virtual void UseDefaultTheme();

  // Set the current theme to the system theme. On some platforms, the system
  // theme is the default theme.
  virtual void UseSystemTheme();

  // Returns true if the default theme and system theme are not the same on
  // this platform.
  virtual bool IsSystemThemeDistinctFromDefaultTheme() const;

  // Whether we're using the chrome default theme. Virtual so linux can check
  // if we're using the GTK theme.
  virtual bool UsingDefaultTheme() const;

  // Whether we are using the system theme. On GTK, the system theme is the GTK
  // theme, not the "Classic" theme.
  virtual bool UsingSystemTheme() const;

  // Whether we are using theme installed through extensions.
  // |UsingExtensionTheme| and |UsingDefaultTheme| are not mutually exclusive as
  // default theme can be installed through extensions.
  virtual bool UsingExtensionTheme() const;

  // Whether we are using an autogenerated theme.
  virtual bool UsingAutogeneratedTheme() const;

  // Gets the id of the last installed theme. (The theme may have been further
  // locally customized.)
  virtual std::string GetThemeID() const;

  // Uninstall theme extensions which are no longer in use.
  void RemoveUnusedThemes();

  // Returns the syncable service for syncing theme. The returned service is
  // owned by |this| object.
  virtual ThemeSyncableService* GetThemeSyncableService() const;

  // Gets the ThemeProvider for |profile|. This will be different for an
  // incognito profile and its original profile, even though both profiles use
  // the same ThemeService.
  static const ui::ThemeProvider& GetThemeProviderForProfile(Profile* profile);

  // Gets the ThemeProvider for |profile| that represents the default colour
  // scheme for the OS.
  static const ui::ThemeProvider& GetDefaultThemeProviderForProfile(
      Profile* profile);

  // Builds an autogenerated theme from a given |color| and applies it.
  virtual void BuildAutogeneratedThemeFromColor(SkColor color);

  // Returns the theme color for an autogenerated theme.
  virtual SkColor GetAutogeneratedThemeColor() const;

  // Returns |ThemeService::ThemeReinstaller| for the current theme.
  std::unique_ptr<ThemeService::ThemeReinstaller>
  BuildReinstallerForCurrentTheme();

 protected:
  // Set a custom default theme instead of the normal default theme.
  virtual void SetCustomDefaultTheme(
      scoped_refptr<CustomThemeSupplier> theme_supplier);

  // Returns true if the ThemeService should use the system theme on startup.
  virtual bool ShouldInitWithSystemTheme() const;

  // Decides if the IncreasedContrastThemeSupplier should be used according
  // to |native_theme|.
  virtual bool ShouldUseIncreasedContrastThemeSupplier(
      ui::NativeTheme* native_theme) const;

  // Returns the color to use for |id| and |incognito| if the theme service does
  // not provide an override.
  virtual SkColor GetDefaultColor(int id, bool incognito) const;

  // Get the specified tint - |id| is one of the TINT_* enum values.
  color_utils::HSL GetTint(int id, bool incognito) const;

  // Clears all the override fields and saves the dictionary.
  virtual void ClearAllThemeData();

  // Initialize current theme state data from preferences.
  virtual void InitFromPrefs();

  // Let all the browser views know that themes have changed.
  virtual void NotifyThemeChanged();

  // Implementation for ui::ThemeProvider (see block of functions in private
  // section).
  virtual bool ShouldUseNativeFrame() const;
  bool HasCustomImage(int id) const;

  // If there is an inconsistency in preferences, change preferences to a
  // consistent state.
  virtual void FixInconsistentPreferencesIfNeeded();

  Profile* profile() const { return profile_; }

  void set_ready() { ready_ = true; }

  const CustomThemeSupplier* get_theme_supplier() const {
    return theme_supplier_.get();
  }

  // True if the theme service is ready to be used.
  // TODO(pkotwicz): Add DCHECKS to the theme service's getters once
  // ThemeSource no longer uses the ThemeService when it is not ready.
  bool ready_ = false;

 private:
  // This class implements ui::ThemeProvider on behalf of ThemeService and keeps
  // track of the incognito state of the calling code.
  class BrowserThemeProvider : public ui::ThemeProvider {
   public:
    BrowserThemeProvider(const ThemeService& theme_service,
                         bool incognito,
                         bool use_default);
    ~BrowserThemeProvider() override;

    // Overridden from ui::ThemeProvider:
    gfx::ImageSkia* GetImageSkiaNamed(int id) const override;
    SkColor GetColor(int original_id) const override;
    color_utils::HSL GetTint(int original_id) const override;
    int GetDisplayProperty(int id) const override;
    bool ShouldUseNativeFrame() const override;
    bool HasCustomImage(int id) const override;
    bool HasCustomColor(int id) const override;
    base::RefCountedMemory* GetRawData(int id, ui::ScaleFactor scale_factor)
        const override;

   private:
    class DefaultScope;
    friend class DefaultScope;

    const ThemeService& theme_service_;
    bool incognito_;
    bool use_default_;

    DISALLOW_COPY_AND_ASSIGN(BrowserThemeProvider);
  };
  friend class BrowserThemeProvider;
  friend class theme_service_internal::ThemeServiceTest;

  // Key for cache of separator colors; pair is <tab color, frame color>.
  using SeparatorColorKey = std::pair<SkColor, SkColor>;
  using SeparatorColorCache = std::map<SeparatorColorKey, SkColor>;

  // Computes the "toolbar top separator" color.  This color is drawn atop the
  // frame to separate it from tabs, the toolbar, and the new tab button, as
  // well as atop background tabs to separate them from other tabs or the
  // toolbar.  We use semitransparent black or white so as to darken or lighten
  // the frame, with the goal of contrasting with both the frame color and the
  // active tab (i.e. toolbar) color.  (It's too difficult to try to find colors
  // that will contrast with both of these as well as the background tab color,
  // and contrasting with the foreground tab is the most important).
  static SkColor GetSeparatorColor(SkColor tab_color, SkColor frame_color);

  // virtual for testing.
  virtual void DoSetTheme(const extensions::Extension* extension,
                          bool suppress_infobar);

  // These methods provide the implementation for ui::ThemeProvider (exposed
  // via BrowserThemeProvider).
  gfx::ImageSkia* GetImageSkiaNamed(int id, bool incognito) const;
  SkColor GetColor(int id,
                   bool incognito,
                   bool* has_custom_color = nullptr) const;
  int GetDisplayProperty(int id) const;
  base::RefCountedMemory* GetRawData(int id,
                                     ui::ScaleFactor scale_factor) const;

  // Returns a cross platform image for an id.
  //
  // TODO(erg): Make this part of the ui::ThemeProvider and the main way to get
  // theme properties out of the theme provider since it's cross platform.
  gfx::Image GetImageNamed(int id, bool incognito) const;

  // Called when the extension service is ready.
  void OnExtensionServiceReady();

  // Migrate the theme to the new theme pack schema by recreating the data pack
  // from the extension.
  void MigrateTheme();

  // Replaces the current theme supplier with a new one and calls
  // StopUsingTheme() or StartUsingTheme() as appropriate.
  void SwapThemeSupplier(scoped_refptr<CustomThemeSupplier> theme_supplier);

  // Implementation of SetTheme() (and the fallback from InitFromPrefs() in
  // case we don't have a theme pack). |new_theme| indicates whether this is a
  // newly installed theme or a migration.
  void BuildFromExtension(const extensions::Extension* extension,
                          bool new_theme);

  // Callback when |pack| has finished or failed building.
  void OnThemeBuiltFromExtension(const extensions::ExtensionId& extension_id,
                                 scoped_refptr<BrowserThemePack> pack,
                                 bool new_theme);

#if BUILDFLAG(ENABLE_SUPERVISED_USERS)
  // Returns true if the profile belongs to a supervised user.
  bool IsSupervisedUser() const;

  // Sets the current theme to the supervised user theme. Should only be used
  // for supervised user profiles.
  void SetSupervisedUserTheme();
#endif

  // Functions that modify theme prefs.
  void ClearThemePrefs();
  void SetThemePrefsForExtension(const extensions::Extension* extension);
  void SetThemePrefsForColor(SkColor color);

  bool DisableExtension(const std::string& extension_id);

  // Whether what users would think of as a "custom theme" (that is, an
  // extension or autogenerated theme) is in use.
  bool UsingCustomTheme() const;

  // Whether the default incognito color/tint for |id| should be used, if
  // available.
  bool UseIncognitoColor(int id) const;

  // Whether dark default colors/tints should be used, if available.
  bool UseDarkModeColors() const;

  // Whether the color from the theme supplier (if any) should be ignored for
  // the given |id| and |incognito| state.
  bool ShouldIgnoreThemeSupplier(int id, bool incognito) const;

  // Given a theme property ID |id|, returns the corresponding omnibox color
  // overridden by the system theme.  Returns base::nullopt if the color is not
  // overridden, or if |id| does not correspond to an omnibox color.
  base::Optional<SkColor> GetOmniboxColor(int id,
                                          bool incognito,
                                          bool* has_custom_color) const;

  Profile* profile_;

  scoped_refptr<CustomThemeSupplier> theme_supplier_;

  // The id of the theme extension which has just been installed but has not
  // been loaded yet. The theme extension with |installed_pending_load_id_| may
  // never be loaded if the install is due to updating a disabled theme.
  // |pending_install_id_| should be set to |kDefaultThemeID| if there are no
  // recently installed theme extensions
  std::string installed_pending_load_id_ = kDefaultThemeID;

  // The number of infobars currently displayed.
  int number_of_reinstallers_ = 0;

  // A cache of already-computed values for COLOR_TOOLBAR_TOP_SEPARATOR, which
  // can be expensive to compute.
  mutable SeparatorColorCache separator_color_cache_;

  content::NotificationRegistrar registrar_;

  std::unique_ptr<ThemeSyncableService> theme_syncable_service_;

#if BUILDFLAG(ENABLE_EXTENSIONS)
  class ThemeObserver;
  std::unique_ptr<ThemeObserver> theme_observer_;
#endif

  BrowserThemeProvider original_theme_provider_{*this, false, false};
  BrowserThemeProvider incognito_theme_provider_{*this, true, false};
  BrowserThemeProvider default_theme_provider_{*this, false, true};

  SEQUENCE_CHECKER(sequence_checker_);

  // Allows us to cancel building a theme pack from an extension.
  base::CancelableTaskTracker build_extension_task_tracker_;

  // The ID of the theme that's currently being built on a different thread.
  // We hold onto this just to be sure not to uninstall the extension view
  // RemoveUnusedThemes while it's still being built.
  std::string building_extension_id_;

  ScopedObserver<ui::NativeTheme, ui::NativeThemeObserver>
      native_theme_observer_{this};

  base::WeakPtrFactory<ThemeService> weak_ptr_factory_{this};

  DISALLOW_COPY_AND_ASSIGN(ThemeService);
};

#endif  // CHROME_BROWSER_THEMES_THEME_SERVICE_H_
