// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_FIREFOX_SYNC_FIREFOX_SYNC_SERVICE_FACTORY_H_
#define CHROME_BROWSER_FIREFOX_SYNC_FIREFOX_SYNC_SERVICE_FACTORY_H_

#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"

class Profile;

namespace firefox_sync {

class FirefoxSyncService;

// One FirefoxSyncService per regular profile, created with the profile so
// scheduled syncs run without opening settings.
class FirefoxSyncServiceFactory : public ProfileKeyedServiceFactory {
 public:
  static FirefoxSyncService* GetForProfile(Profile* profile);
  static FirefoxSyncServiceFactory* GetInstance();

 private:
  friend base::NoDestructor<FirefoxSyncServiceFactory>;

  FirefoxSyncServiceFactory();
  ~FirefoxSyncServiceFactory() override;

  // BrowserContextKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  bool ServiceIsCreatedWithBrowserContext() const override;
  void RegisterProfilePrefs(
      user_prefs::PrefRegistrySyncable* registry) override;
};

}  // namespace firefox_sync

#endif  // CHROME_BROWSER_FIREFOX_SYNC_FIREFOX_SYNC_SERVICE_FACTORY_H_
