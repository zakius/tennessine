// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/firefox_sync/firefox_sync_service_factory.h"

#include "chrome/browser/autofill/personal_data_manager_factory.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/firefox_sync/firefox_sync_service.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/password_manager/factories/profile_password_store_factory.h"
#include "chrome/browser/profiles/profile.h"

namespace firefox_sync {

// static
FirefoxSyncService* FirefoxSyncServiceFactory::GetForProfile(Profile* profile) {
  return static_cast<FirefoxSyncService*>(
      GetInstance()->GetServiceForBrowserContext(profile, true));
}

// static
FirefoxSyncServiceFactory* FirefoxSyncServiceFactory::GetInstance() {
  static base::NoDestructor<FirefoxSyncServiceFactory> instance;
  return instance.get();
}

FirefoxSyncServiceFactory::FirefoxSyncServiceFactory()
    : ProfileKeyedServiceFactory("FirefoxSyncService",
                                 ProfileSelections::BuildForRegularProfile()) {
  DependsOn(autofill::PersonalDataManagerFactory::GetInstance());
  DependsOn(BookmarkModelFactory::GetInstance());
  DependsOn(HistoryServiceFactory::GetInstance());
  DependsOn(ProfilePasswordStoreFactory::GetInstance());
}

FirefoxSyncServiceFactory::~FirefoxSyncServiceFactory() = default;

std::unique_ptr<KeyedService>
FirefoxSyncServiceFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  return std::make_unique<FirefoxSyncService>(
      Profile::FromBrowserContext(context));
}

bool FirefoxSyncServiceFactory::ServiceIsCreatedWithBrowserContext() const {
  return true;
}

void FirefoxSyncServiceFactory::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  FirefoxSyncService::RegisterProfilePrefs(registry);
}

}  // namespace firefox_sync
