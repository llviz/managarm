#include <fcntl.h>
#include <unistd.h>
#include <set>

#include <helix/memory.hpp>
#include <protocols/fs/client.hpp>
#include <protocols/fs/server.hpp>
#include "common.hpp"
#include "device.hpp"
#include "tmp_fs.hpp"
#include "fifo.hpp"

// TODO: Remove dependency on those functions.
#include "extern_fs.hpp"
HelHandle __mlibc_getPassthrough(int fd);
HelHandle __raw_map(int fd);

namespace tmp_fs {

namespace {

struct Superblock;

struct Node : FsNode {
	Node(Superblock *superblock, FsNode::DefaultOps default_ops = 0);

protected:
	~Node() = default;

public:
	FutureMaybe<FileStats> getStats() override {
		std::cout << "\e[31mposix: Fix tmpfs getStats()\e[39m" << std::endl;
		FileStats stats{};
		stats.inodeNumber = _inodeNumber;
		co_return stats;
	}

private:
	int64_t _inodeNumber;
};

struct SymlinkNode final : Node {
private:
	VfsType getType() override {
		return VfsType::symlink;
	}

	expected<std::string> readSymlink(FsLink *) override {
		co_return _link;
	}

public:
	SymlinkNode(Superblock *superblock, std::string link);

private:
	std::string _link;
};

struct DeviceNode final : Node {
private:
	VfsType getType() override {
		return _type;
	}

	DeviceId readDevice() override {
		return _id;
	}

	FutureMaybe<SharedFilePtr> open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		return openDevice(_type, _id, std::move(mount), std::move(link), semantic_flags);
	}

public:
	DeviceNode(Superblock *superblock, VfsType type, DeviceId id);

private:
	VfsType _type;
	DeviceId _id;
};

struct SocketNode final : Node {
	SocketNode(Superblock *superblock);

	VfsType getType() override {
		return VfsType::socket;
	}
};

struct FifoNode final : Node {
private:
	VfsType getType() override {
		return VfsType::fifo;
	}

	FutureMaybe<SharedFilePtr> open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		co_return co_await fifo::openNamedChannel(mount, link, this, semantic_flags);
	}

public:
	FifoNode(Superblock *superblock, mode_t mode)
	:Node{superblock}, mode_{mode} {
		fifo::createNamedChannel(this);
	}

	~FifoNode() {
		fifo::unlinkNamedChannel(this);
	}

private:
	mode_t mode_;
};

struct Link final : FsLink {
public:
	explicit Link(std::shared_ptr<FsNode> target)
	: _target(std::move(target)) { }

	explicit Link(std::shared_ptr<FsNode> owner, std::string name, std::shared_ptr<FsNode> target)
	: _owner(std::move(owner)), _name(std::move(name)), _target(std::move(target)) {
		assert(_owner);
		assert(!_name.empty());
	}

	std::shared_ptr<FsNode> getOwner() override {
		return _owner;
	}

	std::string getName() override {
		// The root link does not have a name.
		assert(_owner);
		return _name;
	}

	std::shared_ptr<FsNode> getTarget() override {
		return _target;
	}

private:
	std::shared_ptr<FsNode> _owner;
	std::string _name;
	std::shared_ptr<FsNode> _target;
};

struct LinkCompare {
	struct is_transparent { };

	bool operator() (const std::shared_ptr<Link> &link, const std::string &name) const {
		return link->getName() < name;
	}
bool operator() (const std::string &name, const std::shared_ptr<Link> &link) const {
	return name < link->getName();
}

bool operator() (const std::shared_ptr<Link> &a, const std::shared_ptr<Link> &b) const {
	return a->getName() < b->getName();
}
};

struct DirectoryNode;

struct DirectoryFile final : File {
public:
	static void serve(smarter::shared_ptr<DirectoryFile> file);

	explicit DirectoryFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link);

	void handleClose() override;

	FutureMaybe<ReadEntriesResult> readEntries() override;
	helix::BorrowedDescriptor getPassthroughLane() override;

private:
	// TODO: Remove this and extract it from the associatedLink().
	DirectoryNode *_node;

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	std::set<std::shared_ptr<Link>, LinkCompare>::iterator _iter;
};

struct DirectoryNode final : Node, std::enable_shared_from_this<DirectoryNode> {
	friend struct Superblock;
	friend struct DirectoryFile;

	static std::shared_ptr<Link> createRootDirectory(Superblock *superblock);

private:
	VfsType getType() override {
		return VfsType::directory;
	}

	std::shared_ptr<FsLink> treeLink() override {
		return _treeLink;
	}

	FutureMaybe<SharedFilePtr>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		assert(!(semantic_flags & ~(semanticRead | semanticWrite)));

		auto file = smarter::make_shared<DirectoryFile>(std::move(mount), std::move(link));
		file->setupWeakFile(file);
		DirectoryFile::serve(file);
		co_return File::constructHandle(std::move(file));
	}


	FutureMaybe<std::shared_ptr<FsLink>> getLink(std::string name) override {
		auto it = _entries.find(name);
		if(it != _entries.end())
			co_return *it;
		co_return nullptr; // TODO: Return an error code.
	}

	FutureMaybe<std::shared_ptr<FsLink>> link(std::string name,
			std::shared_ptr<FsNode> target) override {
		assert(_entries.find(name) == _entries.end());
		auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(target));
		_entries.insert(link);
		co_return link;
	}

	async::result<std::variant<Error, std::shared_ptr<FsLink>>> mkdir(std::string name) override;

	async::result<std::shared_ptr<FsLink>> symlink(std::string name, std::string path) override;

	async::result<std::shared_ptr<FsLink>> mkdev(std::string name,
			VfsType type, DeviceId id) override;

	async::result<std::shared_ptr<FsLink>> mkfifo(std::string name, mode_t mode) override;

	FutureMaybe<void> unlink(std::string name) override {
		auto it = _entries.find(name);
		assert(it != _entries.end());
		_entries.erase(it);

		notifyObservers(FsObserver::deleteEvent, name, 0);
		co_return;
	}

public:
	DirectoryNode(Superblock *superblock);

private:
	// TODO: This creates a circular reference -- fix this.
	std::shared_ptr<Link> _treeLink;
	std::set<std::shared_ptr<Link>, LinkCompare> _entries;
};

// TODO: Remove this class in favor of MemoryNode.
struct InheritedNode final : Node {
private:
	VfsType getType() override {
		return VfsType::regular;
	}

	FutureMaybe<SharedFilePtr>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		assert(!(semantic_flags & ~(semanticRead | semanticWrite)));
		auto fd = ::open(_path.c_str(), O_RDONLY);
		assert(fd != -1);

		helix::UniqueDescriptor passthrough(__mlibc_getPassthrough(fd));
		co_return extern_fs::createFile(std::move(passthrough), std::move(mount), std::move(link));
	}

public:
	InheritedNode(Superblock *superblock, std::string path);

private:
	std::string _path;
};

struct MemoryFile final : File {
public:
	static void serve(smarter::shared_ptr<MemoryFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->_cancelServe));
	}

	MemoryFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
	: File{StructName::get("tmpfs.regular"), std::move(mount), std::move(link)}, _offset{0} { }

	void handleClose() override;

	expected<off_t> seek(off_t delta, VfsSeek whence) override;

	expected<size_t> readSome(Process *, void *buffer, size_t max_length) override;

	async::result<void> writeAll(Process *, const void *buffer, size_t length) override;

	FutureMaybe<void> truncate(size_t size) override;

	FutureMaybe<void> allocate(int64_t offset, size_t size) override;

	FutureMaybe<helix::UniqueDescriptor> accessMemory() override;

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	uint64_t _offset;
};

struct MemoryNode final : Node {
	friend struct MemoryFile;

	MemoryNode(Superblock *superblock);

	VfsType getType() override {
		return VfsType::regular;
	}

	FutureMaybe<SharedFilePtr>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		assert(!(semantic_flags & ~(semanticRead | semanticWrite | semanticNonBlock)));
		auto file = smarter::make_shared<MemoryFile>(std::move(mount), std::move(link));
		file->setupWeakFile(file);
		MemoryFile::serve(file);
		co_return File::constructHandle(std::move(file));
	}

	FutureMaybe<FileStats> getStats() override {
		std::cout << "\e[31mposix: Fix tmpfs getStats() in MemoryNode\e[39m" << std::endl;
		FileStats stats{};
		stats.fileSize = _fileSize;
		co_return stats;
	}

private:
	void _resizeFile(size_t new_size) {
		_fileSize = new_size;

		size_t aligned_size = (new_size + 0xFFF) & ~size_t(0xFFF);
		if(aligned_size <= _areaSize)
			return;

		if(_memory) {
			HEL_CHECK(helResizeMemory(_memory.getHandle(), aligned_size));
		}else{
			HelHandle handle;
			HEL_CHECK(helAllocateMemory(aligned_size, 0, nullptr, &handle));
			_memory = helix::UniqueDescriptor{handle};
		}

		_mapping = helix::Mapping{_memory, 0, aligned_size};
		_areaSize = aligned_size;
	}

	helix::UniqueDescriptor _memory;
	helix::Mapping _mapping;
	size_t _areaSize;
	size_t _fileSize;
};

struct Superblock final : FsSuperblock {
	FutureMaybe<std::shared_ptr<FsNode>> createRegular() override {
		auto node = std::make_shared<MemoryNode>(this);
		co_return std::move(node);
	}

	FutureMaybe<std::shared_ptr<FsNode>> createSocket() override {
		auto node = std::make_shared<SocketNode>(this);
		co_return std::move(node);
	}

	async::result<std::shared_ptr<FsLink>> rename(FsLink *src_fs_link,
			FsNode *dest_fs_dir, std::string dest_name) override {
		auto src_link = static_cast<Link *>(src_fs_link);
		auto dest_dir = static_cast<DirectoryNode *>(dest_fs_dir);

		auto src_dir = static_cast<DirectoryNode *>(src_link->getOwner().get());
		auto it = src_dir->_entries.find(src_link->getName());
		assert(it != src_dir->_entries.end() && it->get() == src_link);

		// Unlink an existing link if such a link exists.
		if(auto dest_it = dest_dir->_entries.find(dest_name);
				dest_it != dest_dir->_entries.end())
			dest_dir->_entries.erase(dest_it);

		auto new_link = std::make_shared<Link>(dest_dir->shared_from_this(),
				std::move(dest_name), src_link->getTarget());
		src_dir->_entries.erase(it);
		dest_dir->_entries.insert(new_link);
		co_return new_link;
	}

	int64_t allocateInode() {
		return _inodeCounter++;
	}

private:
	int64_t _inodeCounter = 1;
};

// ----------------------------------------------------------------------------
// MemoryNode and MemoryFile implementation.
// ----------------------------------------------------------------------------

MemoryNode::MemoryNode(Superblock *superblock)
: Node{superblock}, _areaSize{0}, _fileSize{0} { }

void MemoryFile::handleClose() {
	_cancelServe.cancel();
}

expected<off_t>
MemoryFile::seek(off_t delta, VfsSeek whence) {
	if(whence == VfsSeek::absolute) {
		_offset = delta;
	}else if(whence == VfsSeek::relative){
		_offset += delta;
	}else if(whence == VfsSeek::eof) {
		assert(whence == VfsSeek::eof);
		auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());
		_offset += delta + node->_fileSize;
	}
	co_return _offset;
}

expected<size_t>
MemoryFile::readSome(Process *, void *buffer, size_t max_length) {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	// TODO: Return end-of-file otherwise.
	assert(_offset <= node->_fileSize);
	auto chunk = std::min(node->_fileSize - _offset, max_length);

	memcpy(buffer, reinterpret_cast<char *>(node->_mapping.get()) + _offset, chunk);
	_offset += chunk;

	co_return chunk;
}

async::result<void>
MemoryFile::writeAll(Process *, const void *buffer, size_t length) {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	if(_offset + length > node->_fileSize)
		node->_resizeFile(_offset + length);

	memcpy(reinterpret_cast<char *>(node->_mapping.get()) + _offset, buffer, length);
	_offset += length;
	co_return;
}

async::result<void>
MemoryFile::truncate(size_t size) {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	node->_resizeFile(size);
	co_return;
}

async::result<void>
MemoryFile::allocate(int64_t offset, size_t size) {
	assert(!offset);

	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());

	// TODO: Careful about overflow.
	if(offset + size <= node->_fileSize)
		co_return;
	node->_resizeFile(offset + size);
}

FutureMaybe<helix::UniqueDescriptor>
MemoryFile::accessMemory() {
	auto node = static_cast<MemoryNode *>(associatedLink()->getTarget().get());
	co_return node->_memory.dup();
}

// ----------------------------------------------------------------------------

InheritedNode::InheritedNode(Superblock *superblock, std::string path)
: Node{superblock}, _path{std::move(path)} { }

// ----------------------------------------------------------------------------
// DirectoryNode and DirectoryFile implementation.
// ----------------------------------------------------------------------------

void DirectoryFile::serve(smarter::shared_ptr<DirectoryFile> file) {
//TODO:		assert(!file->_passthrough);

	helix::UniqueLane lane;
	std::tie(lane, file->_passthrough) = helix::createStream();
	async::detach(protocols::fs::servePassthrough(std::move(lane),
			file, &File::fileOperations, file->_cancelServe));
}

void DirectoryFile::handleClose() {
	_cancelServe.cancel();
}

DirectoryFile::DirectoryFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
: File{StructName::get("tmpfs.dir"), std::move(mount), std::move(link)},
		_node{static_cast<DirectoryNode *>(associatedLink()->getTarget().get())},
		_iter{_node->_entries.begin()} { }

// TODO: This iteration mechanism only works as long as _iter is not concurrently deleted.
async::result<ReadEntriesResult>
DirectoryFile::readEntries() {
	if(_iter != _node->_entries.end()) {
		auto name = (*_iter)->getName();
		_iter++;
		co_return name;
	}else{
		co_return std::nullopt;
	}
}

helix::BorrowedDescriptor DirectoryFile::getPassthroughLane() {
	return _passthrough;
}

// ----------------------------------------------------------------------------
// Node implementation.
// ----------------------------------------------------------------------------

Node::Node(Superblock *superblock, FsNode::DefaultOps default_ops)
: FsNode{superblock, default_ops} {
	_inodeNumber = superblock->allocateInode();
}

// ----------------------------------------------------------------------------
// SymlinkNode implementation.
// ----------------------------------------------------------------------------

SymlinkNode::SymlinkNode(Superblock *superblock, std::string link)
: Node{superblock}, _link(std::move(link)) { }

// ----------------------------------------------------------------------------
// DeviceNode implementation.
// ----------------------------------------------------------------------------

DeviceNode::DeviceNode(Superblock *superblock, VfsType type, DeviceId id)
: Node{superblock}, _type(type), _id(id) {
	assert(type == VfsType::charDevice || type == VfsType::blockDevice);
}

// ----------------------------------------------------------------------------
// SocketNode implementation.
// ----------------------------------------------------------------------------

SocketNode::SocketNode(Superblock *superblock)
: Node{superblock} { }

// ----------------------------------------------------------------------------
// DirectoryNode implementation.
// ----------------------------------------------------------------------------

std::shared_ptr<Link> DirectoryNode::createRootDirectory(Superblock *superblock) {
	auto node = std::make_shared<DirectoryNode>(superblock);
	auto the_node = node.get();
	auto link = std::make_shared<Link>(std::move(node));
	the_node->_treeLink = link;
	return link;
}

DirectoryNode::DirectoryNode(Superblock *superblock)
: Node{superblock, FsNode::defaultSupportsObservers} { }

async::result<std::variant<Error, std::shared_ptr<FsLink>>>
DirectoryNode::mkdir(std::string name) {
	assert(_entries.find(name) == _entries.end());
	auto node = std::make_shared<DirectoryNode>(static_cast<Superblock *>(superblock()));
	auto the_node = node.get();
	auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(node));
	the_node->_treeLink = link;
	_entries.insert(link);
	co_return link;
}

async::result<std::shared_ptr<FsLink>>
DirectoryNode::symlink(std::string name, std::string path) {
	assert(_entries.find(name) == _entries.end());
	auto node = std::make_shared<SymlinkNode>(static_cast<Superblock *>(superblock()),
			std::move(path));
	auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(node));
	_entries.insert(link);
	co_return link;
}

async::result<std::shared_ptr<FsLink>>
DirectoryNode::mkdev(std::string name, VfsType type, DeviceId id) {
	assert(_entries.find(name) == _entries.end());
	auto node = std::make_shared<DeviceNode>(static_cast<Superblock *>(superblock()),
			type, id);
	auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(node));
	_entries.insert(link);
	co_return link;
}

async::result<std::shared_ptr<FsLink>>
DirectoryNode::mkfifo(std::string name, mode_t mode) {
	assert(_entries.find(name) == _entries.end());
	auto node = std::make_shared<FifoNode>(static_cast<Superblock *>(superblock()), mode);
	auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(node));
	_entries.insert(link);
	co_return link;
}


// TODO: File system should not have global superblocks.
static Superblock globalSuperblock;

} // anonymous namespace

// Ironically, this function does not create a MemoryNode.
std::shared_ptr<FsNode> createMemoryNode(std::string path) {
	return std::make_shared<InheritedNode>(&globalSuperblock, std::move(path));
}

std::shared_ptr<FsLink> createRoot() {
	return DirectoryNode::createRootDirectory(&globalSuperblock);
}

} // namespace tmp_fs
