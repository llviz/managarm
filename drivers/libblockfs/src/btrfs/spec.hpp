#pragma once

#include <assert.h>
#include <format>
#include <generator>
#include <helix/memory.hpp>
#include <stdint.h>
#include <utility>

#include "types.hpp"

namespace blockfs::btrfs {

struct [[gnu::packed]] Timespec {
	int64_t sec;
	int32_t nsec;
};

struct [[gnu::packed]] Uuid {
	uint32_t data1;
	uint16_t data2;
	uint16_t data3;
	uint8_t data4[8];
};

enum class ItemType : uint8_t {
	INODE_ITEM = 0x01,
	INODE_REF = 0x0C,
	XATTR_ITEM = 0x18,
	DIR_ITEM = 0x54,
	DIR_INDEX = 0x60,
	EXTENTDATA_ITEM = 0x6C,
	EXTENT_CSUM = 0x80,
	ROOT_ITEM = 0x84,
	EXTENT_ITEM = 0xA0,
	BLOCK_GROUP_ITEM = 0xC0,
	FREE_SPACE_INFO = 0xC6,
	FREE_SPACE_EXTENT = 0xC7,
	DEV_EXTENT_ITEM = 0xCC,
	DEV_ITEM = 0xD8,
	CHUNK_ITEM = 0xE4,
	DEV_STATS_ITEM = 0xF9,
};

struct [[gnu::packed]] Key {
	uint64_t objectid;
	ItemType type;
	uint64_t offset;

	auto operator<=>(const Key&) const = default;

	// Return a key with zero offset.
	// This can be useful for comparing keys if you only care about objectid and type.
	Key noOffset() const {
		return {objectid, type};
	}
};

static_assert(sizeof(Key) == 0x11, "Bad key size");

struct [[gnu::packed]] KeyPtr {
	Key k;
	LogicalAddress addr;
	uint64_t generation;
};

struct [[gnu::packed]] DirItem {
	Key location;
	uint64_t transaction_id;
	uint16_t data_len;
	uint16_t name_len;
	uint8_t type;
};

static_assert(sizeof(DirItem) == 30, "Bad dir_item size");

struct [[gnu::packed]] device_item {
	uint64_t device_id;
	uint64_t bytes;
	uint64_t bytes_used;
	uint32_t preferred_io_alignment;
	uint32_t preferred_io_width;
	uint32_t minimum_io_size;
	uint64_t type;
	uint64_t generation;
	uint64_t start_offset;
	uint32_t dev_group;
	uint8_t seek_speed;
	uint8_t bandwidth;
	Uuid device_uuid;
	Uuid fd_uuid;
};

struct [[gnu::packed]] ChunkItem {
	uint64_t chunk_size;
	uint64_t object_id;
	uint64_t stripe_size;
	uint64_t type;
	uint32_t preferred_io_alignment;
	uint32_t preferred_io_width;
	uint32_t minimum_io_size;
	uint16_t stripe_count;
	uint16_t sub_stripes;
};

struct [[gnu::packed]] ChunkStripe {
	uint64_t device_id;
	PhysicalAddress offset;
	Uuid device_uuid;
};

struct [[gnu::packed]] InodeItem {
	uint64_t generation;
	uint64_t transaction_id;
	uint64_t size;
	uint64_t nbytes;
	uint64_t block_group;
	uint32_t nlink;
	uint32_t uid;
	uint32_t gid;
	uint32_t mode;
	uint64_t rdev;
	uint64_t flags;
	uint64_t sequence;
	uint64_t __reserved[4];
	Timespec atime;
	Timespec ctime;
	Timespec mtime;
	Timespec otime;
};

static_assert(sizeof(InodeItem) == 160, "Bad inode_item size");

struct [[gnu::packed]] ExtentData {
	uint64_t generation;
	uint64_t decoded_size;
	uint8_t compression;
	uint8_t encryption;
	uint16_t other_encoding;
	uint8_t type;
};

struct [[gnu::packed]] ExtentDataExtra {
	LogicalAddress extent_addr;
	uint64_t extent_size;
	uint64_t extent_offset;
	uint64_t num_bytes;
};

struct [[gnu::packed]] RootItem {
	InodeItem inode;
	uint64_t generation;
	uint64_t root_dir_id;
	uint64_t bytenr;
	uint64_t byte_limit;
	uint64_t bytes_used;
	uint64_t last_snapshot;
	uint64_t flags;
	uint32_t refs;
	uint8_t padding[219];
};

struct [[gnu::packed]] Superblock {
	char csum[0x20];
	Uuid fs_uuid;
	uint64_t physical_address;
	uint64_t flags;
	char magic[8];
	uint64_t generation;
	LogicalAddress root_tree_root;
	LogicalAddress chunk_tree_root;
	LogicalAddress log_tree_root;
	uint64_t log_root_transid;
	uint64_t total_bytes;
	uint64_t bytes_used;
	uint64_t root_dir_objectid;
	uint64_t num_devices;
	uint32_t sector_size;
	uint32_t node_size;
	uint32_t leaf_size;
	uint32_t stripe_size;
	uint32_t sys_chunk_array_size;
	uint64_t chunk_root_generation;
	uint64_t compat_flags;
	uint64_t compat_ro_flags;
	uint64_t inompat_flags;
	uint16_t checksum_type;
	uint8_t root_level;
	uint8_t chunk_root_level;
	uint8_t log_root_level;
	device_item dev_item_data;
	char label[0x100];
	uint64_t cache_generation;
	uint64_t uuid_tree_generation;
	char __padding[0xF0];
};

static_assert(
    sizeof(Superblock) == 811, "The superblock size at the bootstrap chunks must be 824 bytes"
);

struct [[gnu::packed]] Item {
	Key k;
	uint32_t data_offset;
	uint32_t data_size;
};

static_assert(sizeof(Item) == 25, "Bad item size");

struct [[gnu::packed]] BlockHeader {
	uint8_t csum[0x20];
	Uuid fs_uuid;
	uint64_t bytenr;
	uint64_t flags;
	Uuid chunk_tree_uuid;
	uint64_t generation;
	uint64_t owner;
	uint32_t nritems;
	uint8_t level;

	static struct BlockHeader *fromMapping(helix::Mapping &mapping) {
		return reinterpret_cast<BlockHeader *>(mapping.get());
	}

	std::span<KeyPtr> keyPtrs() const {
		assert(this->level);
		return std::span<KeyPtr>{
		    reinterpret_cast<KeyPtr *>(reinterpret_cast<uintptr_t>(this) + sizeof(BlockHeader)),
		    this->nritems
		};
	}

	std::span<Item> items() const {
		assert(this->level == 0);
		return std::span<Item>{
		    reinterpret_cast<Item *>(reinterpret_cast<uintptr_t>(this) + sizeof(BlockHeader)),
		    this->nritems
		};
	}
};

static_assert(sizeof(BlockHeader) == 101, "Bad block_header size");

} // namespace blockfs::btrfs
