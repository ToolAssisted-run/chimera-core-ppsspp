// The memory stick as an in-memory filesystem (see ram-filesystem.h).
//
// Semantics follow DirectoryFileSystem-with-SIMULATE_FAT32 loosely: paths are
// case-insensitive (FAT), directories must be created one level at a time,
// timestamps are a fixed constant (determinism: the machine has no clock to
// draw them from). The whole tree lives in ordinary guest memory, so
// whole-machine savestates capture and restore it exactly.
#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

#include "Common/Serialize/Serializer.h"
#include "Core/FileSystems/FileSystem.h"
#include "Core/HLE/ErrorCodes.h"

#include "ram-filesystem.h"

namespace {

// A fixed FAT-ish timestamp: 2017-05-27 12:44:28 UTC, the sandbox epoch.
tm FixedTm() {
	tm t{};
	t.tm_year = 117;
	t.tm_mon = 4;
	t.tm_mday = 27;
	t.tm_hour = 12;
	t.tm_min = 44;
	t.tm_sec = 28;
	return t;
}

std::string UpperKey(std::string_view s) {
	std::string k(s);
	std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return (char)std::toupper(c); });
	return k;
}

// Normalize a PSP-side path to "/A/B" (no trailing slash; "/" for the root).
std::string Normalize(std::string_view in) {
	std::string out = "/";
	size_t i = 0;
	while (i < in.size()) {
		while (i < in.size() && (in[i] == '/' || in[i] == '\\'))
			i++;
		size_t j = i;
		while (j < in.size() && in[j] != '/' && in[j] != '\\')
			j++;
		if (j > i) {
			std::string_view part = in.substr(i, j - i);
			if (part == ".") {
				// stay
			} else if (part == "..") {
				size_t slash = out.rfind('/');
				out = slash == 0 ? "/" : out.substr(0, slash);
			} else {
				if (out.back() != '/')
					out += '/';
				out.append(part);
			}
		}
		i = j;
	}
	return out;
}

std::string ParentOf(const std::string &norm) {
	size_t slash = norm.rfind('/');
	return slash == 0 ? "/" : norm.substr(0, slash);
}

struct Node {
	std::string displayName;  // last path component as first created
	std::string displayPath;  // full normalized path, original case
	bool isDirectory = false;
	std::vector<u8> data;
};

struct OpenEntry {
	std::string key;   // upper-cased normalized path
	unsigned access = 0;
	s64 pos = 0;
};

// The card itself: like a physical memory stick, its contents survive the
// machine being switched off (PSP_Shutdown destroys the filesystem OBJECT,
// e.g. when a test finishes or the frontend reboots the core for a sync
// setting; the data must not go with it).
std::map<std::string, Node> g_nodes;

class RamFileSystem : public IFileSystem {
public:
	explicit RamFileSystem(IHandleAllocator *hAlloc) : hAlloc_(hAlloc), nodes_(g_nodes) {
		if (!nodes_.count("/")) {
			MkNode("/", "", true);
			// The standard tree the frontend-side CreateSysDirectories would make.
			for (const char *d : { "/PSP", "/PSP/GAME", "/PSP/SAVEDATA", "/PSP/SYSTEM" })
				MkNode(d, d, true);
		}
	}

	void DoState(PointerWrap &p) override {
		// Never used: chimera savestates snapshot the whole guest machine, and
		// this tree is ordinary guest memory. (PPSSPP's own savestates are not
		// part of the chimera reproduction contract.)
		auto s = p.Section("ChimeraRamFS", 1);
		if (!s)
			return;
	}

	std::vector<PSPFileInfo> GetDirListing(std::string_view path, bool *exists) override {
		std::vector<PSPFileInfo> out;
		std::string dir = Normalize(path);
		std::string dirKey = UpperKey(dir);
		auto it = nodes_.find(dirKey);
		if (it == nodes_.end() || !it->second.isDirectory) {
			if (exists)
				*exists = false;
			return out;
		}
		if (exists)
			*exists = true;
		std::string prefix = dirKey == "/" ? "/" : dirKey + "/";
		for (auto &kv : nodes_) {
			if (kv.first.size() <= prefix.size() || kv.first.compare(0, prefix.size(), prefix) != 0)
				continue;
			if (kv.first.find('/', prefix.size()) != std::string::npos)
				continue;  // deeper level
			out.push_back(InfoFor(kv.second));
		}
		return out;
	}

	int OpenFile(std::string filename, FileAccess access, const char *devicename) override {
		std::string norm = Normalize(filename);
		std::string key = UpperKey(norm);
		auto it = nodes_.find(key);
		bool exists = it != nodes_.end();
		if (exists && it->second.isDirectory)
			return SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND;
		if (!exists) {
			if (!(access & FILEACCESS_CREATE))
				return SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND;
			auto parent = nodes_.find(UpperKey(ParentOf(norm)));
			if (parent == nodes_.end() || !parent->second.isDirectory)
				return SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND;
			MkNode(norm, norm, false);
		} else if ((access & FILEACCESS_CREATE) && (access & FILEACCESS_EXCL)) {
			return SCE_KERNEL_ERROR_ERRNO_FILE_ALREADY_EXISTS;
		}
		Node &n = nodes_[key];
		if (access & FILEACCESS_TRUNCATE)
			n.data.clear();
		OpenEntry e;
		e.key = key;
		e.access = (unsigned)access;
		e.pos = (access & FILEACCESS_APPEND) ? (s64)n.data.size() : 0;
		u32 handle = hAlloc_->GetNewHandle();
		open_[handle] = e;
		return (int)handle;
	}

	void CloseFile(u32 handle) override {
		hAlloc_->FreeHandle(handle);
		open_.erase(handle);
	}

	size_t ReadFile(u32 handle, u8 *pointer, s64 size) override {
		int ignored;
		return ReadFile(handle, pointer, size, ignored);
	}

	size_t ReadFile(u32 handle, u8 *pointer, s64 size, int &usec) override {
		auto it = open_.find(handle);
		if (it == open_.end() || size < 0)
			return 0;
		Node &n = nodes_[it->second.key];
		s64 avail = (s64)n.data.size() - it->second.pos;
		if (avail < 0)
			avail = 0;
		s64 take = size < avail ? size : avail;
		if (take > 0)
			memcpy(pointer, n.data.data() + it->second.pos, (size_t)take);
		it->second.pos += take;
		return (size_t)take;
	}

	size_t WriteFile(u32 handle, const u8 *pointer, s64 size) override {
		int ignored;
		return WriteFile(handle, pointer, size, ignored);
	}

	size_t WriteFile(u32 handle, const u8 *pointer, s64 size, int &usec) override {
		auto it = open_.find(handle);
		if (it == open_.end() || size < 0)
			return 0;
		if (!(it->second.access & (FILEACCESS_WRITE | FILEACCESS_APPEND)))
			return 0;
		Node &n = nodes_[it->second.key];
		if (it->second.access & FILEACCESS_APPEND)
			it->second.pos = (s64)n.data.size();
		size_t end = (size_t)(it->second.pos + size);
		if (n.data.size() < end)
			n.data.resize(end);
		memcpy(n.data.data() + it->second.pos, pointer, (size_t)size);
		it->second.pos += size;
		return (size_t)size;
	}

	size_t SeekFile(u32 handle, s32 position, FileMove type) override {
		auto it = open_.find(handle);
		if (it == open_.end())
			return (size_t)-1;
		Node &n = nodes_[it->second.key];
		s64 base = 0;
		switch (type) {
		case FILEMOVE_BEGIN: base = 0; break;
		case FILEMOVE_CURRENT: base = it->second.pos; break;
		case FILEMOVE_END: base = (s64)n.data.size(); break;
		}
		s64 np = base + position;
		if (np < 0)
			np = 0;
		it->second.pos = np;
		return (size_t)np;
	}

	PSPFileInfo GetFileInfo(std::string filename) override {
		auto it = nodes_.find(UpperKey(Normalize(filename)));
		if (it == nodes_.end())
			return PSPFileInfo();
		return InfoFor(it->second);
	}

	PSPFileInfo GetFileInfoByHandle(u32 handle) override {
		auto it = open_.find(handle);
		if (it == open_.end())
			return PSPFileInfo();
		return InfoFor(nodes_[it->second.key]);
	}

	bool OwnsHandle(u32 handle) override { return open_.find(handle) != open_.end(); }

	bool MkDir(const std::string &dirname) override {
		std::string norm = Normalize(dirname);
		std::string key = UpperKey(norm);
		if (nodes_.count(key))
			return false;
		auto parent = nodes_.find(UpperKey(ParentOf(norm)));
		if (parent == nodes_.end() || !parent->second.isDirectory)
			return false;
		MkNode(norm, norm, true);
		return true;
	}

	bool RmDir(const std::string &dirname) override {
		std::string key = UpperKey(Normalize(dirname));
		auto it = nodes_.find(key);
		if (it == nodes_.end() || !it->second.isDirectory || key == "/")
			return false;
		// must be empty
		std::string prefix = key + "/";
		for (auto &kv : nodes_)
			if (kv.first.compare(0, prefix.size(), prefix) == 0)
				return false;
		nodes_.erase(it);
		return true;
	}

	int RenameFile(const std::string &from, const std::string &to) override {
		std::string fromKey = UpperKey(Normalize(from));
		// "to" may be just the new name, relative to from's directory.
		std::string toNorm = (to.find('/') != std::string::npos || to.find('\\') != std::string::npos)
			? Normalize(to)
			: ParentOf(Normalize(from)) + "/" + to;
		std::string toKey = UpperKey(Normalize(toNorm));
		auto it = nodes_.find(fromKey);
		if (it == nodes_.end())
			return -1;
		if (nodes_.count(toKey) && toKey != fromKey)
			return -1;
		if (it->second.isDirectory) {
			// move the subtree
			std::string prefix = fromKey + "/";
			std::map<std::string, Node> moved;
			for (auto kv = nodes_.begin(); kv != nodes_.end();) {
				if (kv->first == fromKey || kv->first.compare(0, prefix.size(), prefix) == 0) {
					std::string nk = toKey + kv->first.substr(fromKey.size());
					Node n = kv->second;
					n.displayPath = Normalize(toNorm) + kv->first.substr(fromKey.size());
					if (kv->first == fromKey)
						n.displayName = toNorm.substr(toNorm.rfind('/') + 1);
					moved[nk] = std::move(n);
					kv = nodes_.erase(kv);
				} else {
					++kv;
				}
			}
			for (auto &kv : moved)
				nodes_[kv.first] = std::move(kv.second);
		} else {
			Node n = std::move(it->second);
			n.displayName = toNorm.substr(toNorm.rfind('/') + 1);
			n.displayPath = Normalize(toNorm);
			nodes_.erase(it);
			nodes_[toKey] = std::move(n);
		}
		return 0;
	}

	bool RemoveFile(const std::string &filename) override {
		std::string key = UpperKey(Normalize(filename));
		auto it = nodes_.find(key);
		if (it == nodes_.end() || it->second.isDirectory)
			return false;
		for (auto &kv : open_)
			if (kv.second.key == key)
				return false;  // in use
		nodes_.erase(it);
		return true;
	}

	int Ioctl(u32 handle, u32 cmd, u32 indataPtr, u32 inlen, u32 outdataPtr, u32 outlen, int &usec) override {
		return SCE_KERNEL_ERROR_ERRNO_FUNCTION_NOT_SUPPORTED;
	}

	PSPDevType DevType(u32 handle) override { return PSPDevType::FILE; }

	FileSystemFlags Flags() const override {
		return FileSystemFlags::SIMULATE_FAT32 | FileSystemFlags::CARD;
	}

	u64 FreeDiskSpace(const std::string &path) override {
		// A constant "plenty" (real free space would leak allocation details
		// into the machine). 1GiB, FAT-aligned.
		return 1024ull * 1024 * 1024;
	}

	bool ComputeRecursiveDirSizeIfFast(const std::string &path, int64_t *size) override {
		std::string key = UpperKey(Normalize(path));
		std::string prefix = key == "/" ? "/" : key + "/";
		int64_t total = 0;
		for (auto &kv : nodes_)
			if (kv.first.compare(0, prefix.size(), prefix) == 0 && !kv.second.isDirectory)
				total += (int64_t)kv.second.data.size();
		*size = total;
		return true;
	}

	void Describe(char *buf, size_t size) const override {
		snprintf(buf, size, "RamFS: %d nodes", (int)nodes_.size());
	}

private:
	void MkNode(const std::string &norm, const std::string &display, bool dir) {
		Node n;
		size_t slash = display.rfind('/');
		n.displayName = slash == std::string::npos ? display : display.substr(slash + 1);
		n.displayPath = norm;
		n.isDirectory = dir;
		nodes_[UpperKey(norm)] = std::move(n);
	}

	PSPFileInfo InfoFor(const Node &n) {
		PSPFileInfo info;
		info.name = n.displayName;
		info.size = (s64)n.data.size();
		info.access = n.isDirectory ? 0777 : 0666;
		info.exists = true;
		info.type = n.isDirectory ? FILETYPE_DIRECTORY : FILETYPE_NORMAL;
		info.atime = FixedTm();
		info.ctime = FixedTm();
		info.mtime = FixedTm();
		return info;
	}

	IHandleAllocator *hAlloc_;
	std::map<u32, OpenEntry> open_;
	std::map<std::string, Node> &nodes_;
};

bool IsStandardDir(const std::string &key) {
	return key == "/" || key == "/PSP" || key == "/PSP/GAME" || key == "/PSP/SAVEDATA" || key == "/PSP/SYSTEM";
}

}  // namespace

std::shared_ptr<IFileSystem> Chimera_CreateRamMemstick(IHandleAllocator *hAlloc) {
	return std::make_shared<RamFileSystem>(hAlloc);
}

