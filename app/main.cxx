// TODO readlink

#define BOOST_ASIO_ENABLE_HANDLER_TRACKING

#include <asio.hpp>
#include <mutex>
#include <luxem-cxx/luxem.h>
#include <thread>
#include <fcntl.h>
#include <set>
#include <sys/syscall.h>

#include "../ren-cxx-basics/error.h"
#include "../ren-cxx-basics/variant.h"
#include "../ren-cxx-filesystem/file.h"
#include "../ren-cxx-filesystem/path.h"

#include "fuse_wrapper.h"
#include "fuse_outofband.h"
#include "asio_utils.h"

std::vector<function<void(void)>> SignalHandlers;

void HandleSignal(int SignalNumber)
{ 
	std::cout << "Got signal " << SignalNumber << std::endl;
	for (auto const &Handler : SignalHandlers) Handler(); 
}

struct timespec Now(void)
{
	struct timespec Out;
	clock_gettime(CLOCK_REALTIME, &Out);
	return Out;
}

typedef std::vector<uint8_t> RegularFileDataT;
typedef std::string SymlinkPathT;

struct FileT
{
	struct stat stat;
	VariantT<SymlinkPathT, RegularFileDataT> Data;

	FileT(void) : stat() 
	{
		stat.st_atim = Now();
		stat.st_mtim = Now();
		stat.st_ctim = Now();
		stat.st_uid = 0;
		stat.st_gid = 0;
	}
};

struct FilesystemT : OutOfBandControlT
{
	std::set<pid_t> OutOfBandThreadIDs;

	FilesystemT(std::string MountPath, std::mutex &Mutex) : 
		MountPath(Filesystem::PathT::Qualify(MountPath)),
		Mutex(Mutex), 
		OperationCount(-1), 
		Root(std::make_shared<FileT>())
	{
		Files.emplace(std::string("/"), Root);
		Root->stat.st_uid = getuid();
		Root->stat.st_gid = getgid();
		Root->stat.st_mode = 
			S_IFDIR |
			S_IRUSR | S_IWUSR | S_IXUSR |
			S_IRGRP | S_IWGRP | S_IXGRP |
			S_IROTH | S_IWOTH | S_IXOTH;
	}

	bool Clean(void) 
	{
		std::lock_guard<std::mutex> Guard(Mutex);
		std::cout << "Cleaning list:" << std::endl;
		for (auto File = Files.rbegin(); File != Files.rend(); ++File)
			std::cout << "\t" << File->first << std::endl;
		for (auto File = Files.rbegin(); File != Files.rend(); ++File)
		{
			if (File->first == "/") continue;
			auto Path = MountPath.EnterRaw(File->first).Render();
			std::cout << "Cleaning " << Path << std::endl;
			if (File->second->Data)
			{
				if (!OOBRemoveFile(Path)) return false;
			}
			else 
			{
				if (!OOBRemoveDir(Path)) return false;
			}
		}
		Files.clear(); 
		Files.emplace(std::string("/"), Root);
		return true;
	}

	void SetCount(int64_t Count) 
	{ 
		std::lock_guard<std::mutex> Guard(Mutex);
		OperationCount = Count; 
		std::cout << "Count is now " << OperationCount << std::endl;
	}

	int64_t GetCount(void) const 
	{ 
		std::lock_guard<std::mutex> Guard(Mutex);
		return OperationCount; 
	}

	// FuseT interface
	void OperationBegin(bool const OutOfBand)
	{
		Assert(!OutOfBand);
		Mutex.lock();
	}

	void OperationEnd(bool const OutOfBand)
	{
		Assert(!OutOfBand);
		Mutex.unlock();
	}

#define OPER if (!DecrementCount()) return -EIO;

	int getattr(bool const OutOfBand, const char *path, struct stat *buf)
	{
		Assert(!OutOfBand);
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		*buf = Found->second->stat;
		return 0;
	}

	int opendir(bool const OutOfBand, const char *path, struct fuse_file_info *fi)
	{
		Assert(!OutOfBand);
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		if (!CheckPermission(
			*Found->second,
			(fi->flags == O_RDONLY) || (fi->flags == O_RDWR),
			(fi->flags == O_WRONLY) || (fi->flags == O_RDWR),
			false)) return -EACCES;
		if (Found->second->Data) 
		{
			/*if (Found->second->Data.Is<SymlinkPathT>())
			{
				auto Found2 = Files.find(Found->second->Data.Get<SymlinkPathT>());
				if (Found2 == Files.end()) return -ENOENT;
				if (Found2->second->Data) return -ENOTDIR;
			}
			else*/ return -ENOTDIR;
		}
		return 0;
	}

	int readdir(bool const OutOfBand, const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
	{
		std::cout << "reading dir [" << path << "]" << std::endl;
		Assert(!OutOfBand);
		OPER
		std::string Path(path);
		off_t Count = 0;
		for (auto Test = ++Files.lower_bound(Path); IterInDir(Test, Path); ++Test)
		{
			OPER
			Count += 1;
			std::cout << "rd " << Test->first << " @" << Count << std::endl;
			if (Count <= offset) continue;
			auto Filename = Test->first.substr(Path.size() == 1 ? 1 : Path.size() + 1);
			std::cout << "\tfn " << Filename << std::endl;
			if (Filename.find_first_of('/') != std::string::npos) continue;
			std::cout << "\tg" << std::endl;
			if (filler(buf, Filename.c_str(), &Test->second->stat, Count)) break;
		}
		return 0;
	}

	int mkdir(bool const OutOfBand, const char *path, mode_t mode)
	{
		Assert(!OutOfBand);
		OPER
		auto Root = Files.emplace(std::string(path), std::make_shared<FileT>()).first;
		auto const &fuse_context = *fuse_get_context();
		Root->second->stat.st_uid = fuse_context.uid;
		Root->second->stat.st_gid = fuse_context.gid;
		Root->second->stat.st_mode = 
			mode |
			S_IFDIR;
		this->IBCreate(path, true);
		return 0;
	}

	int rmdir(bool const OutOfBand, const char *path)
	{
		Assert(!OutOfBand);
		OPER
		std::string Path(path);
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		if (Found->second->Data) return -ENOTDIR;
		auto Next = Found;
		++Next;
		if (IterInDir(Next, Path)) return -ENOTEMPTY;
		Files.erase(Found);
		this->IBRemove(Path);
		return 0;
	}

	int create(bool const OutOfBand, const char *path, mode_t mode, struct fuse_file_info *fi)
	{
		Assert(!OutOfBand);
		OPER
		auto Root = Files.emplace(std::string(path), std::make_shared<FileT>()).first;
		auto const &fuse_context = *fuse_get_context();
		Root->second->stat.st_uid = fuse_context.uid;
		Root->second->stat.st_gid = fuse_context.gid;
		Root->second->stat.st_mode = 
			mode |
			S_IFREG;
		Root->second->Data = RegularFileDataT();
		SetFile(fi, Root->second);
		this->IBCreate(path, false);
		return 0;
	}
	
	int release(bool const OutOfBand, const char *path, struct fuse_file_info *fi)
	{
		Assert(!OutOfBand);
		ClearFile(fi);
		return 0;
	}

	int utimens(bool const OutOfBand, const char *path, const struct timespec tv[2])
	{
		Assert(!OutOfBand);
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		auto &stat = Found->second->stat;
		stat.st_atim = tv[0];
		stat.st_mtim = tv[1];
		return 0;
	}

	int access(bool const OutOfBand, const char *path, int amode)
	{
		Assert(!OutOfBand);
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		if (amode == F_OK) return 0;
		if (!CheckPermission(
			*Found->second, 
			amode & R_OK,
			amode & W_OK,
			amode & X_OK)) return -EACCES;
		return 0;
	}

	int unlink(bool const OutOfBand, const char *path)
	{
		Assert(!OutOfBand);
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		if (!Found->second->Data) return -EPERM;
		Files.erase(Found);
		this->IBRemove(path);
		return 0;
	}

	int open(bool const OutOfBand, const char *path, struct fuse_file_info *fi)
	{
		Assert(!OutOfBand);
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		if (!Found->second->Data) return -EPERM;
		if (Found->second->Data.Is<SymlinkPathT>())
		{
			/*Found = Files.find(Found->second->Data.Get<SymlinkPathT>());
			if (Found == Files.end())*/ return -ENOENT;
		}
		if (!CheckPermission(
			*Found->second,
			(fi->flags == O_RDONLY) || (fi->flags == O_RDWR),
			(fi->flags == O_WRONLY) || (fi->flags == O_RDWR),
			false)) return -EACCES;
		SetFile(fi, Found->second);
		return 0;
	}

	int read(bool const OutOfBand, const char *path, char *out, size_t count, off_t start, struct fuse_file_info *fi)
	{
		Assert(!OutOfBand);
		OPER
		auto &File = GetFile(fi);
		auto &Data = File.Data.Get<RegularFileDataT>();
		size_t Good = 0;
		size_t Zero = 0;
		if ((unsigned int)start >= Data.size())
			Zero = count;
		else 
		{
			Good = std::min(count, Data.size() - start);
			Zero = count - Good;
		}
		if (Good)
			memcpy(out, &Data[start], Good);
		if (Zero)
			memset(out + Good, 0, Zero);
		return Good;
	}

	int write(bool const OutOfBand, const char *path, const char *out, size_t count, off_t start, struct fuse_file_info *fi)
	{
		Assert(!OutOfBand);
		OPER
		auto &File = GetFile(fi);
		auto &Data = File.Data.Get<RegularFileDataT>();
		auto const End = start + count;
		if (Data.size() < End)
		{
			Data.resize(End);
			File.stat.st_size = End;
		}
		memcpy(&Data[start], out, count);
		return count;
	}

	int truncate(bool const OutOfBand, const char *path, off_t size)
	{
		Assert(!OutOfBand);
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		if (!Found->second->Data) return -EPERM;
		if (Found->second->Data.Is<SymlinkPathT>())
		{
			/*Found = Files.find(Found->second->Data.Get<SymlinkPathT>());
			if (Found == Files.end())*/ return -ENOENT;
		}
		auto &File = *Found->second;
		auto &Data = File.Data.Get<RegularFileDataT>();
		off_t OldLength = Data.size();
		size_t Zero = 0;
		if (OldLength < size)
			Zero = size - OldLength;
		Data.resize(size);
		if (Zero) memset(&Data[OldLength], 0, Zero);
		File.stat.st_size = size;
		return 0;
	}

	int chmod(bool const OutOfBand, const char *path, mode_t mode)
	{
		Assert(!OutOfBand);
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		Found->second->stat.st_mode = mode;
		return 0;
	}

	int chown(bool const OutOfBand, const char *path, uid_t uid, gid_t gid)
	{
		Assert(!OutOfBand);
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		Found->second->stat.st_uid = uid;
		Found->second->stat.st_gid = gid;
		return 0;
	}

	int rename(bool const OutOfBand, const char *from, const char *to)
	{
		Assert(!OutOfBand);
		OPER
		auto Found = Files.find(from);
		if (Found == Files.end()) return -ENOENT;
		Files.emplace(to, Found->second);
		Files.erase(Found);
		this->IBRename(from, to);
		return 0;
	}

	int link(bool const OutOfBand, const char *from, const char *to)
	{
		Assert(!OutOfBand);
		OPER
		auto Found = Files.find(from);
		if (Found == Files.end()) return -ENOENT;
		Files.emplace(to, Found->second);
		this->IBLink(from, to);
		return 0;
	}
	
	int symlink(bool const OutOfBand, const char *to, const char *from)
	{
		Assert(!OutOfBand);
		OPER
		auto Result = Files.emplace(std::string(from), std::make_shared<FileT>());
		auto Root = Result.first;
		Root->second->Data = SymlinkPathT(to);
		auto const &fuse_context = *fuse_get_context();
		Root->second->stat.st_uid = fuse_context.uid;
		Root->second->stat.st_gid = fuse_context.gid;
		Root->second->stat.st_mode = 
			S_IFLNK |
			S_IRUSR | S_IWUSR | S_IXUSR |
			S_IRGRP | S_IWGRP | S_IXGRP |
			S_IROTH | S_IWOTH | S_IXOTH;
		this->IBLink(from, to);
		return 0;
	}

	int readlink(bool const OutOfBand, char const *path, char *out, size_t out_size)
	{
		Assert(!OutOfBand);
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		if (!Found->second->Data) return -EINVAL;
		if (!Found->second->Data.Is<SymlinkPathT>()) return -EINVAL;
		auto &Target = Found->second->Data.Get<SymlinkPathT>();
		memcpy(out, Target.c_str(), std::min(out_size, Target.size()));
		return 0;
	}

	friend struct FuseT<FilesystemT>;
	private:
		// Utility methods
		bool DecrementCount(void)
		{
			if (OperationCount < 0) return true;
			if (OperationCount == 0) return false;
			OperationCount -= 1;
			return true;
		}

		template <typename IteratorT>
			bool IterInDir(IteratorT const &Test, std::string const &Dir)
		{
			return 
				(Test != Files.end()) &&
				InDir(Test->first, Dir);
		}
	
		bool InDir(std::string const &Test, std::string const &Dir)
		{
			return
				(Test.size() > Dir.size()) &&
				(Test.substr(0, Dir.size()) == Dir);
		}
	
		bool CheckPermission(FileT &File, bool Read, bool Write, bool Execute)
		{
			auto const &st_mode = File.stat.st_mode;
			auto const &st_uid = File.stat.st_uid;
			auto const &st_gid = File.stat.st_gid;
			auto const &fuse_context = *fuse_get_context();
			auto const uid = fuse_context.uid;
			auto const gid = fuse_context.gid;
			return
				(
					!Read ||
					(
						((st_mode & S_IRUSR) && (st_uid == uid)) ||
						((st_mode & S_IRGRP) && (st_gid == gid)) ||
						(st_mode & S_IROTH)
					)
				) ||
				(
					!Write ||
					(
						((st_mode & S_IWUSR) && (st_uid == uid)) ||
						((st_mode & S_IWGRP) && (st_gid == gid)) ||
						!(st_mode & S_IWOTH)
					)
				) ||
				(
					!Execute ||
					(
						((st_mode & S_IXUSR) && (st_uid == uid)) ||
						((st_mode & S_IXGRP) && (st_gid == gid)) ||
						!(st_mode & S_IXOTH)
					)
				);
		}

		void SetFile(struct fuse_file_info *fi, std::shared_ptr<FileT> File)
		{
			fi->fh = reinterpret_cast<uint64_t>(new std::shared_ptr<FileT>(std::move(File)));
		}

		FileT &GetFile(struct fuse_file_info *fi)
		{
			return **reinterpret_cast<std::shared_ptr<FileT> *>(fi->fh);
		}

		void ClearFile(struct fuse_file_info *fi)
		{
			delete reinterpret_cast<std::shared_ptr<FileT> *>(fi->fh);
		}

		Filesystem::PathT MountPath;

		std::mutex &Mutex;
		int64_t OperationCount;

		std::shared_ptr<FileT> Root;

		std::map<std::string, std::shared_ptr<FileT>> Files;
};

int main(int argc, char **argv)
{
	try
	{
		// Configure
		if (argc < 2) throw UserErrorT() << "You must specify the mount point on the command line.";

		OptionalT<FinallyT> RemoveRoot;
		if (mkdir(argv[1], 0777) == 0)
		{
			RemoveRoot = FinallyT([&argv](void)
			{
				if (rmdir(argv[1]) != 0)
					throw UserErrorT() << "Could not remove mount directory [" << argv[1] << "]: " << strerror(errno);
			});
		}
		else
		{
			std::cerr << "Could not create mount directory [" << argv[1] << "]: " << strerror(errno) << std::endl;
		}
		
		uint16_t Port;
		{
			auto EnvPort = getenv("CLUNKER_PORT");
			if (!EnvPort) 
			{
				throw UserErrorT() << "The environment variable CLUNKER_PORT must contain the desired IPC port number.";
			}
			if (!(StringT(EnvPort) >> Port)) throw UserErrorT() << "Environment variable CLUNKER_PORT has invalid port number: " << EnvPort;
		}

		struct SharedT
		{
			bool Die = false;

			asio::io_service MainService;

			std::mutex Mutex;
			OutOfBandFilesystemT<FilesystemT> Filesystem;
			FuseT<OutOfBandFilesystemT<FilesystemT>> Fuse;

			SharedT(std::string const &Path) : 
				Filesystem(Path, Mutex), 
				Fuse(Path, Filesystem) {}
		} Shared(argv[1]);

		{
			struct sigaction HandlerInfo;
			memset(&HandlerInfo, 0, sizeof(struct sigaction));
			HandlerInfo.sa_handler = HandleSignal;
			sigemptyset(&(HandlerInfo.sa_mask));
			HandlerInfo.sa_flags = 0;
			sigaction(SIGINT, &HandlerInfo, NULL);
			sigaction(SIGTERM, &HandlerInfo, NULL);
			sigaction(SIGHUP, &HandlerInfo, NULL);
		}
		SignalHandlers.push_back([&Shared](void)
		{
			Shared.Die = true;
			Shared.Fuse.Kill();
			Shared.MainService.stop();
		});
		FinallyT SignalCleanup([](void)
		{
			SignalHandlers.clear();
		});

		// Start listeners on IPC thread
		asio::ip::tcp::endpoint TCPEndpoint(asio::ip::tcp::v4(), Port);
		TCPListen(Shared.MainService, TCPEndpoint, [&Shared](std::shared_ptr<asio::ip::tcp::socket> Connection)
		{
			auto Reader = std::make_shared<luxem::reader>();
			Reader->element([&Shared, Connection](std::shared_ptr<luxem::value> &&Data)
			{
				auto Error = [&](std::string Message)
				{
					Write(Connection,
						luxem::writer()
							.type("error")
							.value(Message)
							.dump());
				};

				if (!Data->has_type()) 
				{
					Error(StringT() 
						<< "Message has no type: [" << luxem::writer().value(Data).dump() << "]");
					return;
				}

				auto Type = Data->get_type();
				if (Type == "clean")
				{
					auto Success = Shared.Filesystem.Clean();
					Write(Connection, 
						luxem::writer()
							.type("clean_result")
							.value(Success)
							.dump());
				}
				else if (Type == "set_count")
				{
					bool Success = false;
					try
					{
						Shared.Filesystem.SetCount(Data->as<luxem::primitive>().get_int());
						Success = true;
					}
					catch (...)
					{
						Error(StringT()
							<< "Bad count [" << luxem::writer().value(Data).dump() << "]");
						Success = false;
					}
					Write(Connection, 
						luxem::writer()
							.type("set_result")
							.value(Success)
							.dump());
				}
				else if (Type == "get_count")
				{
					Write(Connection,
						luxem::writer()
							.type("count")
							.value(Shared.Filesystem.GetCount())
							.dump());
				}
				else
				{
					Error(StringT() <<
						"Unknown message type [" << Type << "]");
					return;
				}
			});
			LoopRead(std::move(Connection), [&Shared, Reader](ReadBufferT &Buffer)
			{
				auto Consumed = Reader->feed(
					(char const *)Buffer.FilledStart(), 
					Buffer.Filled(), 
					false);
				Buffer.Consume(Consumed);
				return !Shared.Die;
			});
			return !Shared.Die;
		});

		std::thread IPCThread([&Shared](void) 
		{ 
#ifdef SYS_gettid
			auto tid = syscall(SYS_gettid);
#else
#error "SYS_gettid unavailable on this system"
#endif
			Shared.Filesystem.OutOfBandThreadIDs.insert(tid);
			Shared.MainService.run();
			std::cout << "IPC stopped " << std::endl;
		});

		// Start fuse on other thread
		auto Result = Shared.Fuse.Run(); 
		std::cout << "Fuse stopped " << std::endl;

		IPCThread.join();

		return Result;
	}
	catch (UserErrorT const &Error)
	{
		std::cerr << "Error: " << Error << std::endl;
		return 1;
	}
	catch (SystemErrorT const &Error)
	{
		std::cerr << "System error: " << Error << std::endl;
		return 1;
	}
	catch (ConstructionErrorT const &Error)
	{
		std::cerr << "Uncaught error: " << Error << std::endl;
		return 1;
	}
	catch (std::runtime_error const &Error)
	{
		std::cerr << "Uncaught error: " << Error.what() << std::endl;
		return 1;
	}
}

