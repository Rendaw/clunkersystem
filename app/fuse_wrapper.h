#ifndef fuse_wrapper_h
#define fuse_wrapper_h

#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <fuse_lowlevel.h>

#include "../ren-cxx-basics/error.h"
			
struct CXXAbsurdity_LowPrecedence {};
struct CXXAbsurdity_HighPrecedence : CXXAbsurdity_LowPrecedence {};

template
<
	typename FilesystemT2,
	typename Enable = decltype(&FilesystemT2::OperationBegin)
>
	void CallOperationBegin(FilesystemT2 *Filesystem, CXXAbsurdity_HighPrecedence)
		{ Filesystem->OperationBegin(); }

template <typename FilesystemT2>
	void CallOperationBegin(FilesystemT2 *Filesystem, CXXAbsurdity_LowPrecedence)
		{ }

template
<
	typename FilesystemT2,
	typename Enable = decltype(&FilesystemT2::OperationEnd)
>
	void CallOperationEnd(FilesystemT2 *Filesystem, CXXAbsurdity_HighPrecedence)
		{ Filesystem->OperationEnd(); }

template <typename FilesystemT2>
	void CallOperationEnd(FilesystemT2 *Filesystem, CXXAbsurdity_LowPrecedence)
		{ }

template <typename MethodTypeT> struct GlueCallT;
template <typename FilesystemT, typename ReturnT, typename ...ArgsT>
	struct GlueCallT<ReturnT (FilesystemT::*)(ArgsT ...)> 
{
	template <ReturnT (FilesystemT::*Source)(ArgsT ...), char const *Name>
		static void Apply(ReturnT (*&Dest)(ArgsT ...))
	{
		Dest = [](ArgsT ...Args) -> ReturnT
		{ 
			//std::cout << "#Calling op " << Name << std::endl;
			auto &FuseContext = *fuse_get_context();
			auto Filesystem = static_cast<FilesystemT *>(FuseContext.private_data);

			// If from own process, noop with success
			// Requests to our own mounted fs are used to notify the os of out of band changes
			std::cout << "op tid " << FuseContext.pid << std::endl;
			if ((FuseContext.pid == 0) || Filesystem->OutOfBandThreadIDs.count(FuseContext.pid))
			{
				std::cout << "pid is oob" << std::endl;
				return 0;
			}

			// Otherwise, call the implementation's method
			CallOperationBegin(Filesystem, CXXAbsurdity_HighPrecedence());
			auto Result = (Filesystem->*Source)
				(std::forward<ArgsT>(Args)...); 
			CallOperationEnd(Filesystem, CXXAbsurdity_HighPrecedence());
			return Result;
		};
	}
};

template <typename FilesystemT> struct FuseT
{
	FuseT(std::string const &Path, FilesystemT &Filesystem) : 
		Mount(Path), Context(Filesystem, Mount)
		{ }

	int Run(void)
	{
		auto Result = fuse_loop(Context.Context);
		return Result;
	}

	void Kill(void)
	{
		fuse_session_exit(Context.Session);
	}

	private:

		struct ArgsT : fuse_args
		{
			ArgsT(void)
			{
				allocated = false;
				argv = nullptr;
				argc = 0;
			}

			void Add(std::string const &Arg)
			{
				ArgStrings.push_back(Arg);
				ArgArray.clear();
				for (auto const &Arg : ArgStrings)
					ArgArray.push_back(Arg.c_str());
				argv = const_cast<char **>(&ArgArray[0]);
				argc = ArgArray.size();
			}

			std::vector<char const *> ArgArray;
			std::vector<std::string> ArgStrings;
		};

		struct MountT
		{
			std::string const Path;
			fuse_chan *Channel;

			MountT(std::string const &Path) : Path(Path), Channel(nullptr)
			{
				ArgsT Args;
				Channel = fuse_mount(Path.c_str(), &Args);
				if (!Channel) throw ConstructionErrorT() << "Couldn't mount filesystem.";
			}

			void Destroy(void)
			{
				if (Channel)
				{
					fuse_unmount(Path.c_str(), Channel);
					Channel = nullptr;
				}
			}

			~MountT(void)
			{
				Destroy();
			}
		};

		struct ContextT
		{
			MountT &Mount;
			static fuse_operations Callbacks;
			FilesystemT &Filesystem;
			fuse *Context;
			fuse_session *Session;

#define PREP_SET_CALLBACK(name) \
			template \
			< \
				typename FilesystemT2, \
				typename Enable = decltype(&FilesystemT2::name) \
			> \
				void SetCallback_##name(FilesystemT2 const *, CXXAbsurdity_HighPrecedence) \
			{ \
				static constexpr char Name[] = #name; \
				GlueCallT<decltype(&FilesystemT2::name)>::template Apply<&FilesystemT2::name, Name>(Callbacks.name); \
			} \
			template <typename FilesystemT2> \
				void SetCallback_##name(FilesystemT2 const *, CXXAbsurdity_LowPrecedence) \
			{ \
			}

			PREP_SET_CALLBACK(getattr)
			PREP_SET_CALLBACK(readlink)
			PREP_SET_CALLBACK(mknod)
			PREP_SET_CALLBACK(mkdir)
			PREP_SET_CALLBACK(unlink)
			PREP_SET_CALLBACK(rmdir)
			PREP_SET_CALLBACK(symlink)
			PREP_SET_CALLBACK(rename)
			PREP_SET_CALLBACK(link)
			PREP_SET_CALLBACK(chmod)
			PREP_SET_CALLBACK(chown)
			PREP_SET_CALLBACK(truncate)
			PREP_SET_CALLBACK(open)
			PREP_SET_CALLBACK(read)
			PREP_SET_CALLBACK(write)
			PREP_SET_CALLBACK(statfs)
			PREP_SET_CALLBACK(flush)
			PREP_SET_CALLBACK(release)
			PREP_SET_CALLBACK(fsync)
			PREP_SET_CALLBACK(setxattr)
			PREP_SET_CALLBACK(getxattr)
			PREP_SET_CALLBACK(listxattr)
			PREP_SET_CALLBACK(removexattr)
			PREP_SET_CALLBACK(opendir)
			PREP_SET_CALLBACK(readdir)
			PREP_SET_CALLBACK(releasedir)
			PREP_SET_CALLBACK(fsyncdir)
			PREP_SET_CALLBACK(init)
			PREP_SET_CALLBACK(destroy)
			PREP_SET_CALLBACK(access)
			PREP_SET_CALLBACK(create)
			PREP_SET_CALLBACK(ftruncate)
			PREP_SET_CALLBACK(fgetattr)
			PREP_SET_CALLBACK(lock)
			PREP_SET_CALLBACK(utimens)
			PREP_SET_CALLBACK(bmap)
			PREP_SET_CALLBACK(ioctl)
			PREP_SET_CALLBACK(poll)
			PREP_SET_CALLBACK(write_buf)
			PREP_SET_CALLBACK(read_buf)
			PREP_SET_CALLBACK(flock)
			PREP_SET_CALLBACK(fallocate)

			ContextT(FilesystemT &Filesystem, MountT &Mount) : 
				Mount(Mount), 
				Filesystem(Filesystem),
				Context(nullptr)
			{
				SetCallback_getattr(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_readlink(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_mknod(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_mkdir(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_unlink(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_rmdir(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_symlink(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_rename(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_link(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_chmod(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_chown(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_truncate(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_open(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_read(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_write(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_statfs(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_flush(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_release(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_fsync(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_setxattr(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_getxattr(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_listxattr(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_removexattr(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_opendir(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_readdir(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_releasedir(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_fsyncdir(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_init(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_destroy(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_access(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_create(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_ftruncate(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_fgetattr(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_lock(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_utimens(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_bmap(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_ioctl(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_poll(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_write_buf(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_read_buf(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_flock(&Filesystem, CXXAbsurdity_HighPrecedence());
				SetCallback_fallocate(&Filesystem, CXXAbsurdity_HighPrecedence());
				ArgsT Args;
				Context = fuse_new(
					Mount.Channel,
					&Args,
					&Callbacks,
					sizeof(Callbacks),
					&Filesystem);
				if (!Context)
					throw ConstructionErrorT() << "Failed to initialize fuse context.";
				Session = fuse_get_session(Context);
			}

			~ContextT(void)
			{
				Mount.Destroy();
				fuse_destroy(Context);
			}
		};

		MountT Mount;
		ContextT Context;
};

template <typename FilesystemT> fuse_operations FuseT<FilesystemT>::ContextT::Callbacks = {};

#endif

