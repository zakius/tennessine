// Copyright 2026 The tennessine Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "chrome/browser/ublock_installer/ublock_installer.h"

#include <optional>
#include <string>
#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/one_shot_event.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"
#include "base/version.h"
#include "chrome/browser/profiles/profile.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/storage_partition.h"
#include "crypto/hash.h"
#include "extensions/browser/crx_installer.h"
#include "extensions/browser/extension_file_task_runner.h"
#include "extensions/browser/extension_registrar.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_registry_factory.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/extension_system_provider.h"
#include "extensions/browser/extensions_browser_client.h"
#include "extensions/browser/install/crx_install_error.h"
#include "extensions/browser/uninstall_reason.h"
#include "extensions/common/extension.h"
#include "extensions/common/mojom/manifest.mojom-shared.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "third_party/zlib/google/zip.h"
#include "url/gurl.h"

namespace ublock_installer {

namespace {

// The release installed, and its zip's SHA-256. tennessine.py upstream reports
// a newer release; moving to it means updating all three.
constexpr char kVersion[] = "1.75.0";
constexpr char kUrl[] =
    "https://github.com/imputnet/uBlock/releases/download/1.75.0/"
    "uBlock0_1.75.0.chromium.zip";
constexpr char kSha256[] =
    "49214674f7c6c474e60da647c05bd28463b1f7a55b2a2b0a53e9d297ef7bf3c6";
// The zip's top folder.
constexpr char kZipRoot[] = "uBlock0.chromium";

// The public key behind the extension ID Helium's built-in copy has, which
// keeps the settings it saved under that ID. The installer adds it to the
// manifest.
constexpr char kId[] = "blockjmkbacgjkknlgpkjjiijinjdanf";
constexpr char kKey[] =
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAyqQYDhKkM6NlQjOgb11DpHQHsqvK"
    "4IMPDIDhvmR47JDGOrf/UyI6+wmx9MfMuqzUBFigip+57UUr+mWoWtI4eNga1zKb8wEphTDm"
    "m0Oo0NHInSlN5t8FmUEPjJ0WVf+CiwA3ucf32OAe+1akOLtMAgHqe4jax7k/b69miKDz3gvi"
    "7lEPmP+H4XDoQ3dOSgEHyjyP4Nf7QgPPrvK2zkB+8q17T0w2bTApETDDkr45K563pxBqb7i9"
    "JmYPP+sXQEB8qDevA5BlPAYBAINFZPOO+SOPpMUguOSA/eb1mjNcfLMGwJs1Zn5ejxqT3Rgc"
    "eqjrN86AgpQvCVeqFtcal/LZFwIDAQAB";

// The release installed last, empty if none: a copy missing while this is set
// was removed by the user.
constexpr char kInstalledVersionPref[] = "tennessine.ublock.installed_version";

// 64 MB: the zip is about 4.
constexpr int64_t kMaxZipSize = 64 * 1024 * 1024;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("tennessine_ublock_installer", R"(
      semantics {
        sender: "tennessine uBlock Origin installer"
        description: "Downloads the pinned uBlock Origin release from GitHub."
        trigger: "Startup, while uBlock Origin is missing or older."
        data: "None."
        destination: OTHER
      }
      policy {
        cookies_allowed: NO
        setting: "Removing uBlock Origin stops it."
      })");

// On the extension file task runner: checks the zip and unpacks it next to
// itself, returning the extension's folder, or nothing on failure.
std::optional<base::FilePath> Unpack(const base::FilePath& zip) {
  std::optional<std::vector<uint8_t>> bytes = base::ReadFileToBytes(zip);
  base::FilePath unzipped = zip.AddExtensionASCII("d");
  bool ok = bytes &&
            base::HexEncodeLower(crypto::hash::Sha256(*bytes)) == kSha256 &&
            base::DeletePathRecursively(unzipped) && zip::Unzip(zip, unzipped);
  base::DeleteFile(zip);
  if (!ok) {
    LOG(WARNING) << "uBlock Origin: the download doesn't match its SHA-256 or "
                    "doesn't unpack";
    base::DeletePathRecursively(unzipped);
    return std::nullopt;
  }
  return unzipped.AppendASCII(kZipRoot);
}

}  // namespace

UBlockInstaller::UBlockInstaller(Profile* profile) : profile_(profile) {
  extensions::ExtensionSystem::Get(profile)->ready().Post(
      FROM_HERE, base::BindOnce(&UBlockInstaller::OnExtensionsReady,
                                weak_factory_.GetWeakPtr()));
}

UBlockInstaller::~UBlockInstaller() = default;

// static
void UBlockInstaller::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  registry->RegisterStringPref(kInstalledVersionPref, std::string());
}

void UBlockInstaller::OnExtensionsReady() {
  const extensions::Extension* installed =
      extensions::ExtensionRegistry::Get(profile_)->GetInstalledExtension(kId);
  PrefService* prefs = profile_->GetPrefs();
  // The first builds loaded it unpacked, which needs developer mode: installed
  // again from the download, as any other extension. Its settings are lost.
  if (installed &&
      installed->location() == extensions::mojom::ManifestLocation::kUnpacked) {
    extensions::ExtensionRegistrar::Get(profile_)->UninstallExtension(
        kId, extensions::UNINSTALL_REASON_REINSTALL, nullptr);
    installed = nullptr;
    prefs->ClearPref(kInstalledVersionPref);
  }
  if (installed && installed->version() >= base::Version(kVersion)) {
    prefs->SetString(kInstalledVersionPref, installed->version().GetString());
    return;
  }
  if (!installed && !prefs->GetString(kInstalledVersionPref).empty()) {
    return;  // The user removed it.
  }
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(kUrl);
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  loader_ = network::SimpleURLLoader::Create(std::move(request),
                                             kTrafficAnnotation);
  loader_->DownloadToTempFile(
      profile_->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess()
          .get(),
      base::BindOnce(&UBlockInstaller::OnDownloaded,
                     weak_factory_.GetWeakPtr()),
      kMaxZipSize);
}

void UBlockInstaller::OnDownloaded(base::FilePath zip) {
  loader_.reset();
  if (zip.empty()) {
    LOG(WARNING) << "uBlock Origin: download failed, trying at the next start";
    return;
  }
  extensions::GetExtensionFileTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&Unpack, zip),
      base::BindOnce(
          [](base::WeakPtr<UBlockInstaller> self,
             std::optional<base::FilePath> extension_dir) {
            if (self && extension_dir) {
              self->OnUnpacked(*extension_dir);
            }
          },
          weak_factory_.GetWeakPtr()));
}

void UBlockInstaller::OnUnpacked(base::FilePath extension_dir) {
  // Silent, like an update from the extension updater: no prompt, and no
  // developer mode, which an unpacked extension needs. The installer copies it
  // into the profile's extensions and deletes extension_dir.
  scoped_refptr<extensions::CrxInstaller> installer =
      extensions::CrxInstaller::CreateSilent(profile_);
  installer->set_allow_silent_install(true);
  installer->set_delete_source(true);
  installer->AddInstallerCallback(base::BindOnce(
      [](base::WeakPtr<UBlockInstaller> self, base::FilePath unzipped,
         const std::optional<extensions::CrxInstallError>& error) {
        extensions::GetExtensionFileTaskRunner()->PostTask(
            FROM_HERE, base::GetDeletePathRecursivelyCallback(unzipped));
        if (error) {
          LOG(WARNING) << "uBlock Origin: installing failed: "
                       << error->message();
          return;
        }
        if (self) {
          self->profile_->GetPrefs()->SetString(kInstalledVersionPref,
                                                kVersion);
        }
      },
      weak_factory_.GetWeakPtr(), extension_dir.DirName()));
  installer->InstallUnpackedCrx(kId, kKey, extension_dir);
}

// static
UBlockInstallerFactory* UBlockInstallerFactory::GetInstance() {
  static base::NoDestructor<UBlockInstallerFactory> instance;
  return instance.get();
}

UBlockInstallerFactory::UBlockInstallerFactory()
    : ProfileKeyedServiceFactory("UBlockInstaller",
                                 ProfileSelections::BuildForRegularProfile()) {
  DependsOn(extensions::ExtensionRegistryFactory::GetInstance());
  DependsOn(
      extensions::ExtensionsBrowserClient::Get()->GetExtensionSystemFactory());
}

UBlockInstallerFactory::~UBlockInstallerFactory() = default;

std::unique_ptr<KeyedService>
UBlockInstallerFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  return std::make_unique<UBlockInstaller>(
      Profile::FromBrowserContext(context));
}

bool UBlockInstallerFactory::ServiceIsCreatedWithBrowserContext() const {
  return true;
}

void UBlockInstallerFactory::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  UBlockInstaller::RegisterProfilePrefs(registry);
}

}  // namespace ublock_installer
