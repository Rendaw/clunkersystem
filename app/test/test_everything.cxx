#include "../../ren-cxx-basics/variant.h"
#include "../../ren-cxx-basics/error.h"
#include "../../ren-cxx-subprocess/subprocess.h"
#include "../asio_utils.h"
#include "client.h"

int main(int argc, char **argv)
{
	try
	{
		// Check arguments, prepare config
		if (argc < 2) throw UserErrorT() << "Missing clunkersystem executable argument.";

		uint16_t ControlPort = 0;
		if (!getenv("CLUNKER_PORT")) throw UserErrorT() << "CLUNKER_PORT env variable is not set.";
		StringT(getenv("CLUNKER_PORT")) >> ControlPort;
		asio::ip::tcp::endpoint ControlEndpoint(
			asio::ip::address_v4::loopback(),
			ControlPort);

		// Test call chain
		CallbackChainT Chain;
		
		// Start signal listening
		asio::io_service MainService;

		asio::signal_set Signals(MainService, SIGTERM, SIGINT, SIGHUP);
		Signals.async_wait([&MainService](asio::error_code const &Error, int SignalNumber)
		{
			MainService.stop();
		});

		// Start clunker
		auto Root = Filesystem::PathT::Qualify("test_root");
		Root.GoTo();
		/*SubprocessT FilesystemProcess(
			MainService, 
			Filesystem::PathT::Qualify(argv[1]), 
			{
				Root.Render()
			});
		LoopRead(&FilesystemProcess.Out, [](ReadBufferT &Buffer)
		{
			while (true)
			{
				auto Filled = Buffer.FilledStart();
				size_t Consume = 0;
				for (size_t At = 0; At < Buffer.Filled(); ++At)
				{
					if (Filled[At] == '\n')
					{
						std::cout << "Filesystem: " << std::string((char const *)&Filled[0], At + 1) << std::flush;
						Consume = At + 1;
						break;
					}
				}
				if (Consume > 0)
					Buffer.Consume(Consume);
				else break;
			}
			return true;
		});
		auto ClunkerCleanup = FinallyT([&Root, &FilesystemProcess](void)
		{
			FilesystemProcess.Terminate();
			FilesystemProcess.GetResult();
		});
		Root.GoTo();*/
		
		// Connect to clunker
		std::shared_ptr<ClunkerControlT> Control;
		ConnectClunker(
			MainService, 
			ControlEndpoint, 
			[&Chain, &Control](std::shared_ptr<ClunkerControlT> NewControl) 
			{ 
				std::cout << "Got connection, starting chain." << std::endl;
				Control = std::move(NewControl); 
				Chain.Next(); 
			});

		// Prepare tests
		Chain.Add([&MainService](void) 
		{ 
			std::cout << "Tests completed successfully." << std::endl;
			MainService.stop(); 
		});

		auto WrapTest = [&Chain, &Control](CallbackChainT::CallbackT &&Callback) mutable
		{
			return [&Chain, &Control, Callback = std::move(Callback)](void) mutable
			{
				Chain
					.Add([&Control, &Chain](void) mutable
					{
						Control->Clean([&Chain](bool Success)
						{
							if (!Success) throw SystemErrorT() << "Clean failed - test case may be broken.";
							Chain.Next();
						});
					})
					.Add([&Control, &Chain](void)
					{
						Control->SetOpCount(-1, [&Chain](bool Success) { Chain.Next(); });
					})
					.Add([&Control, &Chain, Callback = std::move(Callback)](void) mutable
					{ 
						Callback(); 
					});
				Chain.Next();
			};
		};
		size_t TestIndex = 1;
		Chain
			.Add(WrapTest([&TestIndex, &Chain](void) 
			{ 
				std::cout << TestIndex++ << " Noop test" << std::endl; 
				Chain.Next();
			}))
			.Add(WrapTest([&TestIndex, &Chain, &Control](void) 
			{ 
				std::cout << TestIndex++ << " Test clearing" << std::endl; 
				auto Path = Filesystem::PathT::Qualify("roast beef");
				Filesystem::FileT::OpenWrite(Path).Write("logos");
				auto Dir = Filesystem::PathT::Qualify("ultimate_dir");
				Dir.CreateDirectory();
				auto Path2 = Dir.Enter("electrical.tape");
				Filesystem::FileT::OpenWrite(Path2).Write("morose");
				Chain
					.Add([&Control, &Chain](void)
					{
						Control->Clean([&Chain](bool Success) { Chain.Next(); });
					})
					.Add([&Control, &Chain, Path, Path2](void)
					{
						try
						{
							Filesystem::FileT::OpenRead(Path);
							Assert(false);
						}
						catch (ConstructionErrorT const &Error) {}
						try
						{
							Filesystem::FileT::OpenRead(Path2);
							Assert(false);
						}
						catch (ConstructionErrorT const &Error) {}
						Chain.Next();
					})
					;
				Chain.Next();
			}))
			.Add(WrapTest([&TestIndex, &Chain, &Control](void) 
			{ 
				std::cout << TestIndex++ << " Test op count reset" << std::endl; 
				auto Path = Filesystem::PathT::Qualify("roast beef");
				Chain
					.Add([&Control, &Chain](void)
					{
						Control->SetOpCount(0, [&Chain](bool Success) { Chain.Next(); });
					})
					.Add([&Control, &Chain, Path](void)
					{
						try
						{
							Filesystem::FileT::OpenWrite(Path).Write("logos");
							Assert(false);
						}
						catch (ConstructionErrorT const &Error) {}
						Chain.Next();
					})
					.Add([&Control, &Chain](void)
					{
						Control->SetOpCount(-1, [&Chain](bool Success) { Chain.Next(); });
					})
					.Add([&Control, &Chain, Path](void)
					{
						try
						{
							Filesystem::FileT::OpenWrite(Path).Write("logos");
						}
						catch (ConstructionErrorT const &Error) 
						{
							Assert(false);
						}
						Chain.Next();
					})
					;
				Chain.Next();
			}))
			.Add(WrapTest([&TestIndex, &Chain, &Control](void) 
			{ 
				std::cout << TestIndex++ << " Test op count decrement" << std::endl; 
				auto Path = Filesystem::PathT::Qualify("plaster");
				Chain
					.Add([&Control, &Chain](void)
					{
						Control->SetOpCount(2000, [&Chain](bool Success) { Chain.Next(); });
					})
					.Add([&Control, &Chain, Path](void)
					{
						Filesystem::FileT::OpenWrite(Path).Write("logos");
						Chain.Next();
					})
					.Add([&Control, &Chain](void)
					{
						Control->GetOpCount([&Chain](int64_t Count) 
						{ 
							AssertLT(Count, 2000);
							Chain.Next(); 
						});
					})
					;
				Chain.Next();
			}))
			.Add(WrapTest([&TestIndex, &Chain](void) 
			{ 
				std::cout << TestIndex++ << " Test various file ops" << std::endl; 
				// TODO
				// Create file
				// Create dir
				// Create file in dir
				// Write to file in dir
				// Write to file in dir
				// Create subdir
				// ls dir
				// ls root
				// rm nonempty dir
				// mv file, original doesn't exist
				// rm file, file doesn't exist
				// create new file, empty
				Chain.Next();
			}))
			.Add(WrapTest([&TestIndex, &Chain, &Control](void) 
			{ 
				std::cout << TestIndex++ << " Test scheduled clunk" << std::endl; 
				auto Path = Filesystem::PathT::Qualify("chicken");
				auto LastWritten = std::make_shared<std::string>();
				Chain
					.Add([&Control, &Chain](void)
					{
						Control->SetOpCount(200, [&Chain](bool Success) { Chain.Next(); });
					})
					.Add([&Control, &Chain, Path, LastWritten](void)
					{
						for (size_t Count = 0; Count < 1000; ++Count)
						{
							try
							{
								*LastWritten = StringT() << Count;
								auto File = Filesystem::FileT::OpenWrite(Path);
								File.Write(*LastWritten);
							}
							catch (...) {}
						}
						Chain.Next();
					})
					.Add([&Control, &Chain](void)
					{
						Control->SetOpCount(-1, [&Chain](bool Success) { Chain.Next(); });
					})
					.Add([&Control, &Chain, Path, LastWritten](void)
					{
						auto Buffer = Filesystem::FileT::OpenRead(Path).ReadAll();
						AssertNE(
							std::string((char const *)&Buffer[0], Buffer.size()),
							*LastWritten);
						Chain.Next();
					})
					;
				Chain.Next();
			}))
			.Add(WrapTest([&TestIndex, &Chain](void) 
			{ 
				std::cout << TestIndex++ << " Read all valid" << std::endl; 
				auto Path = Filesystem::PathT::Qualify("chicken");
				Filesystem::FileT::OpenWrite(Path).Write("frog man eats cat");
				auto File = open(Path.Render().c_str(), O_RDONLY);
				std::vector<char> Buffer(8);
				auto ReadSize = pread(File, &Buffer[0], 8, 0);
				close(File);
				AssertE(ReadSize, 8);
				std::vector<char> Expected({'f', 'r', 'o', 'g', ' ', 'm', 'a', 'n'});
				AssertE(Buffer, Expected);
				Chain.Next();
			}))
			.Add(WrapTest([&TestIndex, &Chain](void) 
			{ 
				std::cout << TestIndex++ << " Read part valid" << std::endl; 
				auto Path = Filesystem::PathT::Qualify("chicken");
				Filesystem::FileT::OpenWrite(Path).Write("frog man eats cat");
				auto File = open(Path.Render().c_str(), O_RDONLY);
				std::vector<char> Buffer(8);
				auto ReadSize = pread(File, &Buffer[0], 8, 12);
				close(File);
				AssertE(ReadSize, 5);
				std::vector<char> Expected({'s', ' ', 'c', 'a', 't', 0, 0, 0});
				AssertE(Buffer, Expected);
				Chain.Next();
			}))
			.Add(WrapTest([&TestIndex, &Chain](void) 
			{ 
				std::cout << TestIndex++ << " Read all invalid" << std::endl; 
				auto Path = Filesystem::PathT::Qualify("chicken");
				Filesystem::FileT::OpenWrite(Path).Write("frog man eats cat");
				auto File = open(Path.Render().c_str(), O_RDONLY);
				std::vector<char> Buffer(8);
				auto ReadSize = pread(File, &Buffer[0], 8, 20);
				close(File);
				AssertE(ReadSize, 0);
				std::vector<char> Expected({0, 0, 0, 0, 0, 0, 0, 0});
				AssertE(Buffer, Expected);
				Chain.Next();
			}))
			;

		MainService.run();
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

	return 0;
}
