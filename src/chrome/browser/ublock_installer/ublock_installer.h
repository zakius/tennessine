// Copyright 2026 The tennessine Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef CHROME_BROWSER_UBLOCK_INSTALLER_UBLOCK_INSTALLER_H_
#define CHROME_BROWSER_UBLOCK_INSTALLER_UBLOCK_INSTALLER_H_

#include <memory>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "components/keyed_service/core/keyed_service.h"

class Profile;

namespace network {
class SimpleURLLoader;
}

namespace user_prefs {
class PrefRegistrySyncable;
}

namespace ublock_installer {

// Installs uBlock Origin, Helium's build (github.com/imputnet/uBlock), in a
// profile. It is a separate GPL-3.0 program, so it isn't built in: Vanadium's
// patches are GPL-2.0-only and exclude GPL-3.0 code. Once extensions are
// ready, a missing or older copy is replaced by the pinned release: the zip is
// downloaded, checked against its SHA-256, unpacked into the profile and
// loaded as an unpacked extension. The key added to its manifest gives it the
// ID Helium's built-in copy had, so earlier settings stay. A failed attempt
// (offline, say) is tried again at the next start; a copy the user removed
// stays removed.
class UBlockInstaller : public KeyedService {
 public:
  explicit UBlockInstaller(Profile* profile);
  ~UBlockInstaller() override;

  static void RegisterProfilePrefs(user_prefs::PrefRegistrySyncable* registry);

 private:
  void OnExtensionsReady();
  void OnDownloaded(base::FilePath zip);
  void OnUnpacked(base::FilePath extension_dir);

  const raw_ptr<Profile> profile_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  base::WeakPtrFactory<UBlockInstaller> weak_factory_{this};
};

class UBlockInstallerFactory : public ProfileKeyedServiceFactory {
 public:
  static UBlockInstallerFactory* GetInstance();

 private:
  friend base::NoDestructor<UBlockInstallerFactory>;

  UBlockInstallerFactory();
  ~UBlockInstallerFactory() override;

  // BrowserContextKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  bool ServiceIsCreatedWithBrowserContext() const override;
  void RegisterProfilePrefs(
      user_prefs::PrefRegistrySyncable* registry) override;
};

}  // namespace ublock_installer

#endif  // CHROME_BROWSER_UBLOCK_INSTALLER_UBLOCK_INSTALLER_H_
