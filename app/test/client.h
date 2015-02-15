#include <luxem-cxx/luxem.h>

#include "../asio_utils.h"

struct ClunkerControlT
{
	typedef function<void(bool Success)> CleanCallbackT;
	void Clean(CleanCallbackT &&Callback)
	{
		Write(Connection, 
			luxem::writer()
				.type("clean")
				.value("")
				.dump());
		CleanCallbacks.emplace_back(std::move(Callback));
	}
		
	typedef function<void(int64_t)> GetOpCountCallbackT;
	void GetOpCount(GetOpCountCallbackT &&Callback)
	{
		Write(Connection, 
			luxem::writer()
				.type("get_count")
				.value("")
				.dump());
		GetOpCountCallbacks.push_back(std::move(Callback));
	}

	typedef function<void(bool Success)> SetOpCountCallbackT;
	void SetOpCount(int64_t Count, SetOpCountCallbackT &&Callback)
	{
		Write(Connection, 
			luxem::writer()
				.type("set_count")
				.value(Count)
				.dump());
		SetOpCountCallbacks.push_back(std::move(Callback));
	}

	friend void ConnectClunker(
		asio::io_service &Service, 
		asio::ip::tcp::endpoint &Endpoint, 
		function<void(std::shared_ptr<ClunkerControlT> Control)> &&Callback);
	private:
		std::shared_ptr<asio::ip::tcp::socket> Connection;
				
		std::list<CleanCallbackT> CleanCallbacks;
		std::list<GetOpCountCallbackT> GetOpCountCallbacks;
		std::list<SetOpCountCallbackT> SetOpCountCallbacks;
};

void ConnectClunker(
	asio::io_service &Service, 
	asio::ip::tcp::endpoint &Endpoint, 
	function<void(std::shared_ptr<ClunkerControlT> Control)> &&Callback)
{
	TCPConnect(
		Service, 
		Endpoint, 
		[Callback = std::move(Callback)](std::shared_ptr<asio::ip::tcp::socket> Connection)
		{
			auto Control = std::make_shared<ClunkerControlT>();
			Control->Connection = Connection;

			auto Reader = std::make_shared<luxem::reader>();
			Reader->element([Control, Connection](std::shared_ptr<luxem::value> &&Data)
			{
				if (!Data->has_type()) 
				{
					std::cerr << "Message has no type: [" << luxem::writer().value(Data).dump() << "]";
					return;
				}

				auto Type = Data->get_type();
				if (Type == "clean_result")
				{
					AssertGT(Control->CleanCallbacks.size(), 0u);
					auto Callback = std::move(Control->CleanCallbacks.front());
					Control->CleanCallbacks.pop_front();
					Callback(Data->as<luxem::primitive>().get_bool());
				}
				else if (Type == "set_result")
				{
					AssertGT(Control->SetOpCountCallbacks.size(), 0u);
					auto Callback = std::move(Control->SetOpCountCallbacks.front());
					Control->SetOpCountCallbacks.pop_front();
					Callback(Data->as<luxem::primitive>().get_bool());
				}
				else if (Type == "count")
				{
					AssertGT(Control->GetOpCountCallbacks.size(), 0u);
					auto Callback = std::move(Control->GetOpCountCallbacks.front());
					Control->GetOpCountCallbacks.pop_front();
					Callback(Data->as<luxem::primitive>().get_int());
				}
				else
				{
					throw SystemErrorT() << "Unknown message type [" << Type << "]";
				}
			});

			LoopRead(std::move(Connection), [Reader](ReadBufferT &Buffer)
			{
				auto Consumed = Reader->feed((char const *)Buffer.FilledStart(), Buffer.Filled(), false);
				Buffer.Consume(Consumed);
				return true;
			});

			Callback(std::move(Control));
		});
}

