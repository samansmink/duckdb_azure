#include "azure_storage_account_client.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/enums/statement_type.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "http_state_policy.hpp"

#include <azure/core/credentials/token_credential_options.hpp>
#include <azure/core/http/curl_transport.hpp>
#include <azure/identity/azure_cli_credential.hpp>
#include <azure/identity/chained_token_credential.hpp>
#include <azure/identity/client_certificate_credential.hpp>
#include <azure/identity/client_secret_credential.hpp>
#include <azure/identity/default_azure_credential.hpp>
#include <azure/identity/environment_credential.hpp>
#include <azure/identity/managed_identity_credential.hpp>
#include <azure/storage/blobs/blob_options.hpp>
#include <azure/storage/blobs/blob_service_client.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace duckdb {
const static std::string DEFAULT_ENDPOINT = "blob.core.windows.net";

static std::string TryGetCurrentSetting(FileOpener *opener, const std::string &name) {
	Value val;
	if (FileOpener::TryGetCurrentSetting(opener, name, val)) {
		return val.ToString();
	}
	return "";
}

static bool ConnectionStringMatchStorageAccountName(const std::string &connection_string,
                                                    const std::string &provided_storage_account) {
	if (provided_storage_account.empty()) {
		return true;
	}

	auto storage_account_name_pos = connection_string.find("AccountName=");
	if (storage_account_name_pos == std::string::npos) {
		throw InvalidInputException("A invalid connection string has been provided.");
	}
	return 0 == connection_string.compare(storage_account_name_pos + 12, provided_storage_account.size(),
	                                      provided_storage_account);
}

static std::string KVSEndpoint(const KeyValueSecret &secret, const std::string &provided_endpoint) {
	if (provided_endpoint.empty()) {
		auto endpoint_value = secret.TryGetValue("endpoint");
		if (endpoint_value.IsNull()) {
			return DEFAULT_ENDPOINT;
		} else {
			return endpoint_value.ToString();
		}
	}
	return provided_endpoint;
}

static std::string KVSStorageAccount(const KeyValueSecret &secret, const std::string &provided_storage_account) {
	if (provided_storage_account.empty()) {
		return secret.TryGetValue("account_name", true).ToString();
	}
	return provided_storage_account;
}

static std::string AccountUrl(const KeyValueSecret &secret, const std::string &provided_storage_account,
                              const std::string &provided_endpoint) {
	return "https://" + KVSStorageAccount(secret, provided_storage_account) + "." +
	       KVSEndpoint(secret, provided_endpoint);
}

static Azure::Storage::Blobs::BlobClientOptions
ToBlobClientOptions(const Azure::Core::Http::Policies::TransportOptions &transport_options,
                    std::shared_ptr<HTTPState> http_state) {
	Azure::Storage::Blobs::BlobClientOptions options;
	options.Transport = transport_options;
	if (nullptr != http_state) {
		// Because we mainly want to have stats on what has been needed and not on
		// what has been used on the network, we register the policy on `PerOperationPolicies`
		// part and not the `PerRetryPolicies`. Network issues will result in retry that can
		// increase the input/output but will not be displayed in the EXPLAIN summary.
		options.PerOperationPolicies.emplace_back(new HttpStatePolicy(std::move(http_state)));
	}
	return options;
}

static Azure::Core::Credentials::TokenCredentialOptions
ToTokenCredentialOptions(const Azure::Core::Http::Policies::TransportOptions &transport_options) {
	Azure::Core::Credentials::TokenCredentialOptions options;
	options.Transport = transport_options;
	return options;
}

static std::shared_ptr<HTTPState> GetHttpState(FileOpener *opener) {
	Value value;
	bool enable_http_stats = false;
	if (FileOpener::TryGetCurrentSetting(opener, "azure_http_stats", value)) {
		enable_http_stats = value.GetValue<bool>();
	}

	std::shared_ptr<HTTPState> http_state;
	if (enable_http_stats) {
		http_state = HTTPState::TryGetState(opener);
	}

	return http_state;
}

static std::shared_ptr<Azure::Core::Credentials::TokenCredential>
CreateChainedTokenCredential(const std::string &chain,
                             const Azure::Core::Http::Policies::TransportOptions &transport_options) {
	auto credential_options = ToTokenCredentialOptions(transport_options);

	// Create credential chain
	auto chain_list = StringUtil::Split(chain, ';');
	Azure::Identity::ChainedTokenCredential::Sources sources;
	for (const auto &item : chain_list) {
		if (item == "cli") {
			sources.push_back(std::make_shared<Azure::Identity::AzureCliCredential>(credential_options));
		} else if (item == "managed_identity") {
			sources.push_back(std::make_shared<Azure::Identity::ManagedIdentityCredential>(credential_options));
		} else if (item == "env") {
			sources.push_back(std::make_shared<Azure::Identity::EnvironmentCredential>(credential_options));
		} else if (item == "default") {
			sources.push_back(std::make_shared<Azure::Identity::DefaultAzureCredential>(credential_options));
		} else {
			throw InvalidInputException("Unknown credential provider found: " + item);
		}
	}
	return std::make_shared<Azure::Identity::ChainedTokenCredential>(sources);
}

static std::shared_ptr<Azure::Core::Credentials::TokenCredential>
CreateClientCredential(const std::string &tenant_id, const std::string &client_id, const std::string &client_secret,
                       const std::string &client_certificate_path,
                       const Azure::Core::Http::Policies::TransportOptions &transport_options) {
	auto credential_options = ToTokenCredentialOptions(transport_options);
	if (!client_secret.empty()) {
		return std::make_shared<Azure::Identity::ClientSecretCredential>(tenant_id, client_id, client_secret,
		                                                                 credential_options);
	} else if (!client_certificate_path.empty()) {
		return std::make_shared<Azure::Identity::ClientCertificateCredential>(
		    tenant_id, client_id, client_certificate_path, credential_options);
	}

	throw InvalidInputException("Failed to fetch key 'client_secret' or 'client_certificate_path' from secret "
	                            "'service_principal' of type 'azure'");
}

static std::shared_ptr<Azure::Core::Http::HttpTransport>
CreateCurlTransport(const std::string &proxy, const std::string &proxy_username, const std::string &proxy_password) {
	Azure::Core::Http::CurlTransportOptions curl_transport_options;

	if (!proxy.empty()) {
		curl_transport_options.Proxy = proxy;
	}

	if (!proxy_username.empty()) {
		curl_transport_options.ProxyUsername = proxy_username;
	}

	if (!proxy_password.empty()) {
		curl_transport_options.ProxyPassword = proxy_password;
	}

	const char *ca_info = std::getenv("CURL_CA_INFO");
#if !defined(_WIN32) && !defined(__APPLE__)
	if (!ca_info) {
		// https://github.com/Azure/azure-sdk-for-cpp/issues/4983
		// https://github.com/Azure/azure-sdk-for-cpp/issues/4738
		for (const auto *path : {
		         "/etc/ssl/certs/ca-certificates.crt",                // Debian/Ubuntu/Gentoo etc.
		         "/etc/pki/tls/certs/ca-bundle.crt",                  // Fedora/RHEL 6
		         "/etc/ssl/ca-bundle.pem",                            // OpenSUSE
		         "/etc/pki/tls/cacert.pem",                           // OpenELEC
		         "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // CentOS/RHEL 7
		         "/etc/ssl/cert.pem"                                  // Alpine Linux
		     }) {
			if (FILE *f = fopen(path, "r")) {
				fclose(f);
				ca_info = path;
				break;
			}
		}
	}
#endif
	if (ca_info) {
		curl_transport_options.CAInfo = ca_info;
	}

	const char *ca_path = std::getenv("CURL_CA_PATH");
	if (ca_path) {
		curl_transport_options.CAPath = ca_path;
	}

	return std::make_shared<Azure::Core::Http::CurlTransport>(curl_transport_options);
}

static Azure::Core::Http::Policies::TransportOptions GetTransportOptions(const std::string &transport_option_type,
                                                                         const std::string &proxy,
                                                                         const std::string &proxy_username,
                                                                         const std::string &proxy_password) {
	Azure::Core::Http::Policies::TransportOptions transport_options;
	if (transport_option_type == "default") {
		if (!proxy.empty()) {
			transport_options.HttpProxy = proxy;
		}

		if (!proxy_username.empty()) {
			transport_options.ProxyUserName = proxy_username;
		}

		if (!proxy_password.empty()) {
			transport_options.ProxyPassword = proxy_password;
		}
	} else if (transport_option_type == "curl") {
		transport_options.Transport = CreateCurlTransport(proxy, proxy_username, proxy_password);
	} else {
		throw InvalidInputException("transport_option_type cannot take value '%s'", transport_option_type);
	}

	return transport_options;
}

static Azure::Core::Http::Policies::TransportOptions GetTransportOptions(FileOpener *opener,
                                                                         const KeyValueSecret &secret) {
	auto transport_option_type = TryGetCurrentSetting(opener, "azure_transport_option_type");

	std::string http_proxy;
	auto http_proxy_val = secret.TryGetValue("http_proxy");
	if (!http_proxy_val.IsNull()) {
		http_proxy = http_proxy_val.ToString();
	} else {
		// Keep honoring the env variable if present
		auto *http_proxy_env = std::getenv("HTTP_PROXY");
		if (http_proxy_env != nullptr) {
			http_proxy = http_proxy_env;
		}
	}

	std::string http_proxy_username;
	auto http_proxy_username_val = secret.TryGetValue("proxy_user_name");
	if (!http_proxy_username_val.IsNull()) {
		http_proxy_username = http_proxy_username_val.ToString();
	}

	std::string http_proxy_password;
	auto http_proxy_password_val = secret.TryGetValue("proxy_password");
	if (!http_proxy_password_val.IsNull()) {
		http_proxy_password = http_proxy_password_val.ToString();
	}

	return GetTransportOptions(transport_option_type, http_proxy, http_proxy_username, http_proxy_password);
}

static Azure::Storage::Blobs::BlobServiceClient
GetStorageAccountClientFromConfigProvider(FileOpener *opener, const KeyValueSecret &secret,
                                          const std::string &provided_storage_account,
                                          const std::string &provided_endpoint) {
	auto transport_options = GetTransportOptions(opener, secret);

	// If connection string, we're done heres
	auto connection_string_val = secret.TryGetValue("connection_string");
	if (!connection_string_val.IsNull()) {
		auto connection_string = connection_string_val.ToString();
		if (!ConnectionStringMatchStorageAccountName(connection_string, provided_storage_account)) {
			throw InvalidInputException("The provided connection string does not match the storage account named %s",
			                            provided_storage_account);
		}

		auto blob_options = ToBlobClientOptions(transport_options, GetHttpState(opener));
		return Azure::Storage::Blobs::BlobServiceClient::CreateFromConnectionString(connection_string, blob_options);
	}

	// Default provider (config) with no connection string => public storage account

	auto account_url = AccountUrl(secret, provided_storage_account, provided_endpoint);
	auto blob_options = ToBlobClientOptions(transport_options, GetHttpState(opener));
	return Azure::Storage::Blobs::BlobServiceClient(account_url, blob_options);
}

static Azure::Storage::Blobs::BlobServiceClient
GetStorageAccountClientFromCredentialChainProvider(FileOpener *opener, const KeyValueSecret &secret,
                                                   const std::string &provided_storage_account,
                                                   const std::string &provided_endpoint) {
	auto transport_options = GetTransportOptions(opener, secret);

	std::string chain = "default";
	auto chain_value = secret.TryGetValue("chain");
	if (!chain_value.IsNull()) {
		chain = chain_value.ToString();
	}

	// Create credential chain
	auto credential = CreateChainedTokenCredential(chain, transport_options);

	// Connect to storage account
	auto account_url = AccountUrl(secret, provided_storage_account, provided_endpoint);
	auto blob_options = ToBlobClientOptions(transport_options, GetHttpState(opener));
	return Azure::Storage::Blobs::BlobServiceClient(account_url, std::move(credential), blob_options);
}

static Azure::Storage::Blobs::BlobServiceClient
GetStorageAccountClientFromServicePrincipalProvider(FileOpener *opener, const KeyValueSecret &secret,
                                                    const std::string &provided_storage_account,
                                                    const std::string &provided_endpoint) {
	auto transport_options = GetTransportOptions(opener, secret);

	constexpr bool error_on_missing = true;
	auto tenant_id = secret.TryGetValue("tenant_id", error_on_missing);
	auto client_id = secret.TryGetValue("client_id", error_on_missing);
	auto client_secret_val = secret.TryGetValue("client_secret");
	auto client_certificate_path_val = secret.TryGetValue("client_certificate_path");

	std::string client_secret = client_secret_val.IsNull() ? "" : client_secret_val.ToString();
	std::string client_certificate_path =
	    client_certificate_path_val.IsNull() ? "" : client_certificate_path_val.ToString();

	auto token_credential = CreateClientCredential(tenant_id.ToString(), client_id.ToString(), client_secret,
	                                               client_certificate_path, transport_options);

	auto account_url = AccountUrl(secret, provided_storage_account, provided_endpoint);
	auto blob_options = ToBlobClientOptions(transport_options, GetHttpState(opener));
	return Azure::Storage::Blobs::BlobServiceClient {account_url, token_credential, blob_options};
}

static Azure::Storage::Blobs::BlobServiceClient GetStorageAccountClient(FileOpener *opener,
                                                                        const KeyValueSecret &secret,
                                                                        const std::string &provided_storage_account,
                                                                        const std::string &provided_endpoint) {
	auto &provider = secret.GetProvider();
	// default provider
	if (provider == "config") {
		return GetStorageAccountClientFromConfigProvider(opener, secret, provided_storage_account, provided_endpoint);
	} else if (provider == "credential_chain") {
		return GetStorageAccountClientFromCredentialChainProvider(opener, secret, provided_storage_account,
		                                                          provided_endpoint);
	} else if (provider == "service_principal") {
		return GetStorageAccountClientFromServicePrincipalProvider(opener, secret, provided_storage_account,
		                                                           provided_endpoint);
	}

	throw InvalidInputException("Unsupported provider type %s for azure", provider);
}

static Azure::Core::Http::Policies::TransportOptions GetTransportOptions(FileOpener *opener) {
	auto azure_transport_option_type = TryGetCurrentSetting(opener, "azure_transport_option_type");

	// Load proxy options
	auto http_proxy = TryGetCurrentSetting(opener, "azure_http_proxy");
	auto http_proxy_user_name = TryGetCurrentSetting(opener, "azure_proxy_user_name");
	auto http_proxy_password = TryGetCurrentSetting(opener, "azure_proxy_password");

	return GetTransportOptions(azure_transport_option_type, http_proxy, http_proxy_user_name, http_proxy_password);
}

static Azure::Storage::Blobs::BlobServiceClient GetStorageAccountClient(FileOpener *opener,
                                                                        const std::string &provided_storage_account,
                                                                        const std::string &provided_endpoint) {
	auto transport_options = GetTransportOptions(opener);
	auto blob_options = ToBlobClientOptions(transport_options, GetHttpState(opener));

	auto connection_string = TryGetCurrentSetting(opener, "azure_storage_connection_string");
	if (!connection_string.empty() &&
	    ConnectionStringMatchStorageAccountName(connection_string, provided_storage_account)) {
		return Azure::Storage::Blobs::BlobServiceClient::CreateFromConnectionString(connection_string, blob_options);
	}

	std::string endpoint;
	if (provided_endpoint.empty()) {
		endpoint = TryGetCurrentSetting(opener, "azure_endpoint");
		if (endpoint.empty()) {
			endpoint = DEFAULT_ENDPOINT;
		}
	} else {
		endpoint = provided_endpoint;
	}

	std::string azure_account_name;
	if (provided_storage_account.empty()) {
		azure_account_name = TryGetCurrentSetting(opener, "azure_account_name");
	} else {
		azure_account_name = provided_storage_account;
	}
	if (azure_account_name.empty()) {
		throw InvalidInputException("No valid Azure credentials found!");
	}

	auto account_url = "https://" + azure_account_name + "." + endpoint;
	// Credential chain secret equivalent
	auto credential_chain = TryGetCurrentSetting(opener, "azure_credential_chain");
	if (!credential_chain.empty()) {
		auto credential = CreateChainedTokenCredential(credential_chain, transport_options);

		return Azure::Storage::Blobs::BlobServiceClient(account_url, std::move(credential), blob_options);
	}

	// Anonymous
	return Azure::Storage::Blobs::BlobServiceClient {account_url, blob_options};
}

Azure::Storage::Blobs::BlobServiceClient ConnectToStorageAccount(FileOpener *opener, const std::string &path,
                                                                 const AzureParsedUrl &azure_parsed_url) {
	// Lookup Secret
	auto context = opener->TryGetClientContext();

	// Firstly, try to use the auth from the secret
	if (context) {
		auto transaction = CatalogTransaction::GetSystemCatalogTransaction(*context);
		auto secret_lookup = context->db->config.secret_manager->LookupSecret(transaction, path, "azure");
		if (secret_lookup.HasMatch()) {
			const auto &base_secret = secret_lookup.GetSecret();
			return GetStorageAccountClient(opener, dynamic_cast<const KeyValueSecret &>(base_secret),
			                               azure_parsed_url.storage_account_name, azure_parsed_url.endpoint);
		}
	}

	// No secret found try to connect with variables
	return GetStorageAccountClient(opener, azure_parsed_url.storage_account_name, azure_parsed_url.endpoint);
}

} // namespace duckdb