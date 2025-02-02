#pragma once

#include "duckdb.hpp"
#include "duckdb/main/client_context.hpp"
#include "azure_parsed_url.hpp"
#include <azure/storage/blobs/blob_client.hpp>
#include <azure/storage/blobs/blob_service_client.hpp>
#include <string>

namespace duckdb {
class HTTPState;

struct AzureReadOptions {
	int32_t transfer_concurrency = 5;
	int64_t transfer_chunk_size = 1 * 1024 * 1024;
	idx_t buffer_size = 1 * 1024 * 1024;
};

class AzureContextState : public ClientContextState {
public:
	const AzureReadOptions read_options;

private:
	Azure::Storage::Blobs::BlobServiceClient service_client;
	bool is_valid;

public:
	AzureContextState(Azure::Storage::Blobs::BlobServiceClient client, const AzureReadOptions &azure_read_options);
	Azure::Storage::Blobs::BlobContainerClient GetBlobContainerClient(const std::string &blobContainerName) const;
	bool IsValid() const;
	void QueryEnd() override;
};

class AzureStorageFileHandle : public FileHandle {
public:
	AzureStorageFileHandle(FileSystem &fs, string path, uint8_t flags, Azure::Storage::Blobs::BlobClient blob_client,
	                       const AzureReadOptions &read_options);
	~AzureStorageFileHandle() override = default;

public:
	void Close() override {
	}

	uint8_t flags;
	idx_t length;
	time_t last_modified;

	// Read info
	idx_t buffer_available;
	idx_t buffer_idx;
	idx_t file_offset;
	idx_t buffer_start;
	idx_t buffer_end;

	// Read buffer
	duckdb::unique_ptr<data_t[]> read_buffer;

	// Azure Blob Client
	Azure::Storage::Blobs::BlobClient blob_client;

	const AzureReadOptions read_options;
};

class AzureStorageFileSystem : public FileSystem {
public:
	duckdb::unique_ptr<FileHandle> OpenFile(const string &path, uint8_t flags, FileLockType lock = DEFAULT_LOCK,
	                                        FileCompressionType compression = DEFAULT_COMPRESSION,
	                                        FileOpener *opener = nullptr) final;

	vector<string> Glob(const string &path, FileOpener *opener = nullptr) override;

	// FS methods
	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	void FileSync(FileHandle &handle) override;
	int64_t GetFileSize(FileHandle &handle) override;
	time_t GetLastModifiedTime(FileHandle &handle) override;
	bool FileExists(const string &filename) override;
	void Seek(FileHandle &handle, idx_t location) override;
	bool CanHandleFile(const string &fpath) override;
	bool CanSeek() override {
		return true;
	}
	bool OnDiskFile(FileHandle &handle) override {
		return false;
	}
	bool IsPipe(const string &filename) override {
		return false;
	}
	string GetName() const override {
		return "AzureStorageFileSystem";
	}

	static void Verify();

protected:
	static AzureParsedUrl ParseUrl(const string &url);
	static std::shared_ptr<AzureContextState> GetOrCreateStorageContext(FileOpener *opener, const string &path,
	                                                                    const AzureParsedUrl &parsed_url);
	static std::shared_ptr<AzureContextState> CreateStorageContext(FileOpener *opener, const string &path,
	                                                               const AzureParsedUrl &parsed_url);
	static void ReadRange(FileHandle &handle, idx_t file_offset, char *buffer_out, idx_t buffer_out_len);
	virtual duckdb::unique_ptr<AzureStorageFileHandle> CreateHandle(const string &path, uint8_t flags,
	                                                                FileLockType lock, FileCompressionType compression,
	                                                                FileOpener *opener);
};

} // namespace duckdb
