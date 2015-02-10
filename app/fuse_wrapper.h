#ifndef fuse_wrapper_h
#define fuse_wrapper_h

#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <fuse_lowlevel.h>

#include "../ren-cxx-basics/error.h"


template <typename FilesystemT, typename ReturnT, typename ...ArgsT, ReturnT (FilesystemT::*Source)(ArgsT ...)> 
	ReturnT GlueCall(ReturnT (*&Dest)(ArgsT ...))
{
	Dest = [](ArgsT ...Args)
	{ 
		return static_cast<FilesystemT *>(fuse_get_context()->private_data)
			->*Source(std::forward<ArgsT>(Args)...); 
	};
}

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

			template <typename AnyT> static constexpr void *EnableNoop(AnyT Any) { return nullptr; }
#define PREP_SET_CALLBACK(name) \
			template < \
				typename FilesystemT2 = FilesystemT, \
				void * = EnableNoop(&FilesystemT2::name)> void SetCallback_##name(void) \
			{ \
				GlueCall<&FilesystemT2::name>(Callbacks.name); \
			} \
			void SetCallback_##name(...) {}

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
				SetCallback_getattr();
				SetCallback_readlink();
				SetCallback_mknod();
				SetCallback_mkdir();
				SetCallback_unlink();
				SetCallback_rmdir();
				SetCallback_symlink();
				SetCallback_rename();
				SetCallback_link();
				SetCallback_chmod();
				SetCallback_chown();
				SetCallback_truncate();
				SetCallback_open();
				SetCallback_read();
				SetCallback_write();
				SetCallback_statfs();
				SetCallback_flush();
				SetCallback_release();
				SetCallback_fsync();
				SetCallback_setxattr();
				SetCallback_getxattr();
				SetCallback_listxattr();
				SetCallback_removexattr();
				SetCallback_opendir();
				SetCallback_readdir();
				SetCallback_releasedir();
				SetCallback_fsyncdir();
				SetCallback_init();
				SetCallback_destroy();
				SetCallback_access();
				SetCallback_create();
				SetCallback_ftruncate();
				SetCallback_fgetattr();
				SetCallback_lock();
				SetCallback_utimens();
				SetCallback_bmap();
				SetCallback_ioctl();
				SetCallback_poll();
				SetCallback_write_buf();
				SetCallback_read_buf();
				SetCallback_flock();
				SetCallback_fallocate();
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

