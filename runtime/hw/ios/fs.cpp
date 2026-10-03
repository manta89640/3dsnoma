// /dev/fs (ISFS) and NAND files, kept in a host directory that mirrors the Wii's NAND tree, but
// for /tmp, which is in memory.
#include <algorithm>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <memory>
#include <set>
#include <unistd.h>
#include <unordered_map>

#include "ios.h"

namespace guest {

static bool host_is_dir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool host_exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

static void mkdirs_for(const std::string& path) {
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/')
            mkdir(path.substr(0, i).c_str(), 0755);
}

// ---------------------------------------------------------------------------- files
// Writes are buffered (64 KB): the channel formats a 13 MB cache image 512 bytes at a time, and
// on the 3DS every unbuffered write is an SD card access. Handles of the same file see each
// other's writes: a handle flushes the others before it reads or measures the file.
class FsFile;
static std::vector<FsFile*> s_open_files;

class FsFile : public IosHandle {
public:
    FILE* f = nullptr;
    u32 mode = 0;
    std::string host;
    long size = 0;        // the file's size, kept up to date by writes
    bool writing = false;  // the last access wrote (stdio needs a seek before reading again)
    bool dirty = false;    // buffered writes not yet in the file

    ~FsFile() override {
        s_open_files.erase(std::remove(s_open_files.begin(), s_open_files.end(), this), s_open_files.end());
        if (f)
            fclose(f);
    }
    void sync_others() {
        for (FsFile* o : s_open_files)
            if (o != this && o->dirty && o->host == host) {
                fflush(o->f);
                o->dirty = false;
            }
    }
    s32 read(u32 buf, u32 len) override {
        if (!(mode & 1))
            return FS_EACCESS;
        sync_others();
        if (writing) {
            fseek(f, 0, SEEK_CUR);  // stdio: a write and a read need a seek in between
            writing = false;
        }
        std::vector<u8> tmp(len);
        size_t n = fread(tmp.data(), 1, len, f);
        write_g(buf, tmp.data(), (u32)n);
        return (s32)n;
    }
    s32 write(u32 buf, u32 len) override {
        if (!(mode & 2))
            return FS_EACCESS;
        if (!writing) {
            fseek(f, 0, SEEK_CUR);
            writing = true;
        }
        std::vector<u8> tmp(len);
        read_g(tmp.data(), buf, len);
        size_t n = fwrite(tmp.data(), 1, len, f);
        dirty = true;
        static u32 s_writes;
        static bool s_debug = getenv("NOMA_DEBUG_FS") != nullptr;
        if (s_debug && ++s_writes % 1000 == 0)
            log("fs: %u writes (now %s, %ld bytes)", s_writes, host.c_str(), size);
        // NOMA_FS_WRITE_DELAY_US: slow writes down (the 3DS's pace on a fast host, for timing bugs)
        static int s_delay = getenv("NOMA_FS_WRITE_DELAY_US") ? atoi(getenv("NOMA_FS_WRITE_DELAY_US")) : 0;
        if (s_delay > 0)
            host_sleep_us((u64)s_delay);
        size = std::max(size, ftell(f));
        return (s32)n;
    }
    s32 seek(s32 off, u32 whence) override {
        long cur = ftell(f);
        long pos = whence == 0 ? off : whence == 1 ? cur + off : size + off;
        if (pos < 0 || pos > size)
            return FS_EINVAL;
        if (pos != cur)
            fseek(f, pos, SEEK_SET);
        return (s32)pos;
    }
    s32 ioctl(u32 cmd, u32 in, u32 in_len, u32 out, u32 out_len) override {
        if (cmd == 0x0B) {  // GetFileStats: {size, offset}
            store32(out, (u32)size);
            store32(out + 4, (u32)ftell(f));
            return IOS_OK;
        }
        return IosHandle::ioctl(cmd, in, in_len, out, out_len);
    }
};

// ---------------------------------------------------------------------------- /tmp, in memory
// The Wii's IOS empties /tmp whenever it starts a title, and the channel formats its cache volumes
// there at every launch (/tmp/finch.vf and /tmp/skua.vf: 35 MB, nearly all zeros, written 512
// bytes at a time). It can't take them from an earlier launch: kept, they crash it at start. So
// /tmp is in memory: empty at every start, as on the Wii, and sparse, a page only ever written
// with zeros isn't stored. None of it reaches the SD card.
namespace {

const u32 PAGE = 4096;

struct RamFile {
    u32 size = 0;
    std::unordered_map<u32, std::unique_ptr<u8[]>> pages;  // page number -> PAGE bytes

    void read(u8* dst, u32 pos, u32 len) const {
        while (len) {
            u32 off = pos % PAGE, n = std::min(len, PAGE - off);
            auto it = pages.find(pos / PAGE);
            if (it != pages.end())
                memcpy(dst, it->second.get() + off, n);
            else
                memset(dst, 0, n);
            dst += n, pos += n, len -= n;
        }
    }
    void write(const u8* src, u32 pos, u32 len) {
        size = std::max(size, pos + len);
        while (len) {
            u32 off = pos % PAGE, n = std::min(len, PAGE - off);
            auto it = pages.find(pos / PAGE);
            if (it == pages.end() && std::any_of(src, src + n, [](u8 b) { return b != 0; }))
                it = pages.emplace(pos / PAGE, std::make_unique<u8[]>(PAGE)).first;  // (zeroed)
            if (it != pages.end())
                memcpy(it->second.get() + off, src, n);
            src += n, pos += n, len -= n;
        }
    }
};

std::map<std::string, std::shared_ptr<RamFile>> s_tmp_files;  // by NAND path
std::set<std::string> s_tmp_dirs = {"/tmp"};

bool in_tmp(const std::string& p) { return p == "/tmp" || p.rfind("/tmp/", 0) == 0; }
bool tmp_exists(const std::string& p) { return s_tmp_files.count(p) || s_tmp_dirs.count(p); }

// a file's directories under /tmp, as ISFS would have them
void tmp_add_parents(const std::string& p) {
    for (size_t i = 5; (i = p.find('/', i)) != std::string::npos; i++)
        s_tmp_dirs.insert(p.substr(0, i));
}

class TmpFile : public IosHandle {
public:
    std::shared_ptr<RamFile> file;  // (still there if it's deleted while open)
    u32 mode = 0, pos = 0;

    s32 read(u32 buf, u32 len) override {
        if (!(mode & 1))
            return FS_EACCESS;
        len = std::min(len, file->size - std::min(pos, file->size));
        std::vector<u8> tmp(len);
        file->read(tmp.data(), pos, len);
        write_g(buf, tmp.data(), len);
        pos += len;
        return (s32)len;
    }
    s32 write(u32 buf, u32 len) override {
        if (!(mode & 2))
            return FS_EACCESS;
        std::vector<u8> tmp(len);
        read_g(tmp.data(), buf, len);
        file->write(tmp.data(), pos, len);
        pos += len;
        return (s32)len;
    }
    s32 seek(s32 off, u32 whence) override {
        s64 p = whence == 0 ? off : whence == 1 ? (s64)pos + off : (s64)file->size + off;
        if (p < 0 || p > file->size)
            return FS_EINVAL;
        pos = (u32)p;
        return (s32)pos;
    }
    s32 ioctl(u32 cmd, u32 in, u32 in_len, u32 out, u32 out_len) override {
        if (cmd == 0x0B) {  // GetFileStats: {size, offset}
            store32(out, file->size);
            store32(out + 4, pos);
            return IOS_OK;
        }
        return IosHandle::ioctl(cmd, in, in_len, out, out_len);
    }
};

// a file moved between /tmp and the card (Rename across the two)
std::shared_ptr<RamFile> load_host_file(const std::string& host) {
    auto file = std::make_shared<RamFile>();
    if (FILE* f = fopen(host.c_str(), "rb")) {
        std::vector<u8> buf(64 * 1024);
        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), f)) > 0)
            file->write(buf.data(), file->size, (u32)n);
        fclose(f);
    }
    return file;
}
bool save_host_file(const RamFile& file, const std::string& host) {
    FILE* f = fopen(host.c_str(), "wb");
    if (!f)
        return false;
    std::vector<u8> buf(PAGE);
    for (u32 pos = 0; pos < file.size; pos += PAGE) {
        u32 n = std::min(PAGE, file.size - pos);
        file.read(buf.data(), pos, n);
        fwrite(buf.data(), 1, n, f);
    }
    fclose(f);
    return true;
}

}  // namespace

std::unique_ptr<IosHandle> fs_open_file(const std::string& path, u32 mode, s32* err) {
    if (in_tmp(path)) {
        auto it = s_tmp_files.find(path);
        if (it == s_tmp_files.end()) {
            *err = FS_ENOENT;
            return nullptr;
        }
        auto h = std::make_unique<TmpFile>();
        h->file = it->second;
        h->mode = mode;
        return h;
    }
    std::string host = nand_host_path(path);
    if (!host_exists(host) || host_is_dir(host)) {
        *err = FS_ENOENT;
        return nullptr;
    }
    auto h = std::make_unique<FsFile>();
    h->mode = mode;
    h->host = host;
    for (FsFile* o : s_open_files)  // what other handles wrote is in the file for this one
        if (o->dirty && o->host == host) {
            fflush(o->f);
            o->dirty = false;
        }
    h->f = fopen(host.c_str(), (mode & 2) ? "r+b" : "rb");
    if (!h->f) {
        *err = FS_EACCESS;
        return nullptr;
    }
    setvbuf(h->f, nullptr, _IOFBF, 64 * 1024);
    fseek(h->f, 0, SEEK_END);
    h->size = ftell(h->f);
    fseek(h->f, 0, SEEK_SET);
    s_open_files.push_back(h.get());
    return h;
}

// ---------------------------------------------------------------------------- /dev/fs
class FsDevice : public IosHandle {
public:
    s32 ioctl(u32 cmd, u32 in, u32 in_len, u32 out, u32 out_len) override {
        switch (cmd) {
        case 0x02: {  // GetStats
            u32 stats[7] = {0x4000, 0x5DEC, 0x1000, 0, 0, 0x146B, 0x0400};
            for (int i = 0; i < 7 && (u32)(i * 4) < out_len; i++)
                store32(out + i * 4, stats[i]);
            return IOS_OK;
        }
        case 0x03:    // CreateDir {owner, group, path[64], perms...}
        case 0x09: {  // CreateFile
            std::string path = read_string(in + 6, 64);
            if (in_tmp(path)) {
                if (tmp_exists(path)) {
                    log("fs: create %s %s: exists", cmd == 3 ? "dir" : "file", path.c_str());
                    return FS_EEXIST;
                }
                tmp_add_parents(path);
                if (cmd == 0x03)
                    s_tmp_dirs.insert(path);
                else
                    s_tmp_files[path] = std::make_shared<RamFile>();
                log("fs: create %s %s (in memory)", cmd == 3 ? "dir" : "file", path.c_str());
                return IOS_OK;
            }
            std::string host = nand_host_path(path);
            if (host_exists(host)) {
                log("fs: create %s %s: exists", cmd == 3 ? "dir" : "file", path.c_str());
                return FS_EEXIST;
            }
            mkdirs_for(host);
            if (cmd == 0x03) {
                mkdir(host.c_str(), 0755);
            } else {
                FILE* f = fopen(host.c_str(), "wb");
                if (!f)
                    return FS_EACCESS;
                fclose(f);
            }
            log("fs: create %s %s", cmd == 3 ? "dir" : "file", path.c_str());
            return IOS_OK;
        }
        case 0x05:  // SetAttr
            return IOS_OK;
        case 0x06: {  // GetAttr: path in, attr block out
            std::string path = read_string(in, 64);
            if (in_tmp(path) ? !tmp_exists(path) : !host_exists(nand_host_path(path)))
                return FS_ENOENT;
            store32(out, 0x1000);  // owner (uid)
            store16(out + 4, 0x3031);  // group "01"
            write_g(out + 6, path.c_str(), (u32)std::min<size_t>(path.size() + 1, 64));
            store8(out + 0x46, 3);  // owner rw
            store8(out + 0x47, 3);
            store8(out + 0x48, 3);
            store8(out + 0x49, 0);
            return IOS_OK;
        }
        case 0x07: {  // Delete (a directory with all it holds)
            std::string path = read_string(in, 64);
            if (in_tmp(path)) {
                if (!tmp_exists(path) || path == "/tmp")
                    return path == "/tmp" ? FS_EACCESS : FS_ENOENT;
                log("fs: delete %s", path.c_str());
                s_tmp_files.erase(path);
                s_tmp_dirs.erase(path);
                std::string under = path + "/";
                for (auto it = s_tmp_files.lower_bound(under); it != s_tmp_files.end() && it->first.rfind(under, 0) == 0;)
                    it = s_tmp_files.erase(it);
                for (auto it = s_tmp_dirs.lower_bound(under); it != s_tmp_dirs.end() && it->rfind(under, 0) == 0;)
                    it = s_tmp_dirs.erase(it);
                return IOS_OK;
            }
            std::string host = nand_host_path(path);
            if (!host_exists(host))
                return FS_ENOENT;
            log("fs: delete %s", path.c_str());
            if (host_is_dir(host))
                rmdir(host.c_str());
            else
                unlink(host.c_str());
            return IOS_OK;
        }
        case 0x08: {  // Rename
            std::string from = read_string(in, 64), to = read_string(in + 64, 64);
            if (in_tmp(from) || in_tmp(to)) {  // (files: the channel makes them in /tmp and moves them)
                std::shared_ptr<RamFile> file;
                if (in_tmp(from)) {
                    auto it = s_tmp_files.find(from);
                    if (it == s_tmp_files.end())
                        return s_tmp_dirs.count(from) ? FS_EINVAL : FS_ENOENT;
                    file = it->second;
                    s_tmp_files.erase(it);
                } else {
                    std::string hf = nand_host_path(from);
                    if (!host_exists(hf) || host_is_dir(hf))
                        return host_exists(hf) ? FS_EINVAL : FS_ENOENT;
                    file = load_host_file(hf);
                    unlink(hf.c_str());
                }
                if (in_tmp(to)) {
                    tmp_add_parents(to);
                    s_tmp_files[to] = file;
                } else {
                    std::string ht = nand_host_path(to);
                    mkdirs_for(ht);
                    if (!save_host_file(*file, ht))
                        return FS_EACCESS;
                }
                return IOS_OK;
            }
            std::string hf = nand_host_path(from), ht = nand_host_path(to);
            if (!host_exists(hf))
                return FS_ENOENT;
            mkdirs_for(ht);
            ::rename(hf.c_str(), ht.c_str());
            return IOS_OK;
        }
        case 0x0D:  // Shutdown
        case 0x0A:  // SetFileVerCtrl
            return IOS_OK;
        default:
            return IosHandle::ioctl(cmd, in, in_len, out, out_len);
        }
    }

    s32 ioctlv(u32 cmd, const std::vector<IoVec>& in, const std::vector<IoVec>& io) override {
        if (cmd == 0x04) {  // ReadDir
            std::string path = read_string(in[0].addr, 64);
            std::vector<std::string> names;
            if (in_tmp(path)) {
                if (!s_tmp_dirs.count(path))
                    return FS_ENOENT;
                std::string under = path + "/";
                for (const auto& kv : s_tmp_files)
                    if (kv.first.rfind(under, 0) == 0 && kv.first.find('/', under.size()) == std::string::npos)
                        names.push_back(kv.first.substr(under.size()));
                for (const std::string& d : s_tmp_dirs)
                    if (d.rfind(under, 0) == 0 && d.find('/', under.size()) == std::string::npos)
                        names.push_back(d.substr(under.size()));
            } else {
                std::string host = nand_host_path(path);
                if (!host_is_dir(host))
                    return FS_ENOENT;
                if (DIR* d = opendir(host.c_str())) {
                    while (dirent* e = readdir(d))
                        if (e->d_name[0] != '.')
                            names.push_back(e->d_name);
                    closedir(d);
                }
                if (path == "/")
                    names.push_back("tmp");
            }
            if (in.size() == 1) {  // count only
                store32(io[0].addr, (u32)names.size());
                return IOS_OK;
            }
            u32 max = load32(in[1].addr);
            u32 p = io[0].addr, n = 0;
            for (auto& nm : names) {
                if (n >= max)
                    break;
                write_g(p, nm.c_str(), (u32)nm.size() + 1);
                p += (u32)nm.size() + 1;
                n++;
            }
            store32(io[1].addr, n);
            return IOS_OK;
        }
        if (cmd == 0x0C) {  // GetUsage: blocks, inodes
            store32(io[0].addr, 0);
            store32(io[1].addr, 0);
            return IOS_OK;
        }
        return IosHandle::ioctlv(cmd, in, io);
    }
};

std::unique_ptr<IosHandle> fs_open_device() { return std::make_unique<FsDevice>(); }

}  // namespace guest
