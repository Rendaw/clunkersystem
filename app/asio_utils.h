#ifndef asio_utils_h
#define asio_utils_h

#include <asio.hpp>
#include <chrono>
#include <iostream>

#include "../ren-cxx-filesystem/file.h"
#include "../ren-cxx-basics/function.h"

template <typename CallbackT>
	void TCPListenInternal(
		asio::io_service &Service, 
		std::shared_ptr<asio::ip::tcp::acceptor> Acceptor, 
		CallbackT &&Callback,
		size_t RetryCount = 0)
{
	std::shared_ptr<asio::ip::tcp::socket> Connection;
	try
	{
		Connection = std::make_shared<asio::ip::tcp::socket>(Service);
	}
	catch (...)
	{
		if (RetryCount >= 5) throw;
		auto Retry = std::make_shared<asio::basic_waitable_timer<std::chrono::system_clock>>(Service, std::chrono::minutes(1));
		auto &RetryRef = *Retry;
		RetryRef.async_wait(
			[&, Retry = std::move(Retry), Acceptor = std::move(Acceptor), Callback = std::move(Callback)]
				(asio::error_code const &Error)
		{
			TCPListenInternal(Service, std::move(Acceptor), std::move(Callback), RetryCount + 1);
		});
		return;
	}
	auto &AcceptorRef = *Acceptor;
	auto &ConnectionRef = *Connection;
	auto Endpoint = std::make_shared<asio::ip::tcp::endpoint>();
	auto &EndpointRef = *Endpoint;
	std::cout << "Accepting" << std::endl;
	AcceptorRef.async_accept(
		ConnectionRef, 
		EndpointRef,
		[
			&Service, 
			Acceptor = std::move(Acceptor), 
			Connection = std::move(Connection), 
			Endpoint = std::move(Endpoint), 
			Callback = std::move(Callback)
		]
			(asio::error_code const &Error)
		{
			if (Error)
			{
				std::cerr << "Error accepting connection to " << *Endpoint << ": " 
					"(" << Error.value() << ") " << Error << std::endl;
				return;
			}
			std::cout << "Accepted" << std::endl;
			if (Callback(std::move(Connection)))
				TCPListenInternal(Service, std::move(Acceptor), std::move(Callback));
		});
}

template <typename CallbackT>
	void TCPListen(
		asio::io_service &Service, 
		asio::ip::tcp::endpoint &Endpoint,
		CallbackT &&Callback)
{
	auto Acceptor = std::make_shared<asio::ip::tcp::acceptor>(Service, Endpoint);
	TCPListenInternal(Service, std::move(Acceptor), std::move(Callback));
}

template <typename CallbackT>
	void TCPConnectInternal(
		asio::io_service &Service,
		std::shared_ptr<asio::ip::tcp::socket> &&Connection, 
		asio::ip::tcp::endpoint const &Endpoint, 
		CallbackT &&Callback, 
		uint8_t RetryCount)
{
	auto &ConnectionRef = *Connection;
	std::cout << "Connecting" << std::endl;
	ConnectionRef.async_connect(
		Endpoint,
		[
			&Service,
			&Endpoint,
			Connection = std::move(Connection), 
			Callback = std::move(Callback),
			RetryCount
		]
			(asio::error_code const &Error)
		{
			if (Error)
			{
				std::cerr << "Failed to connect to " << Endpoint << 
					" (attempt " << (int)RetryCount << ")"
					": " << Error <<
					std::endl;
				if (RetryCount >= 5) return;
				auto Retry = std::make_shared<asio::basic_waitable_timer<std::chrono::system_clock>>
					(Service, std::chrono::seconds(5));
				auto &RetryRef = *Retry;
				RetryRef.async_wait(
					[
						&Service, 
						Retry = std::move(Retry), 
						Connection = std::move(Connection), 
						&Endpoint,
						Callback = std::move(Callback),
						RetryCount
					]
						(asio::error_code const &Error) mutable
				{
					TCPConnectInternal(
						Service,
						std::move(Connection), 
						Endpoint, 
						std::move(Callback), 
						RetryCount + 1);
				});
				return;
			}
			std::cout << "Connected" << std::endl;
			Callback(std::move(Connection));
		});
}

template <typename CallbackT>
	void TCPConnect(asio::io_service &Service, asio::ip::tcp::endpoint const &Endpoint, CallbackT &&Callback)
{
	auto Connection = std::make_shared<asio::ip::tcp::socket>(Service);
	TCPConnectInternal(Service, std::move(Connection), Endpoint, std::move(Callback), 0);
}

template <typename BufferT, typename CallbackT>
        void UnifiedRead(asio::ip::tcp::socket &Connection, BufferT Buffer, CallbackT Callback)
        { Connection.async_receive(std::forward<BufferT>(Buffer), std::forward<CallbackT>(Callback)); }
template <typename BufferT, typename CallbackT>
        void UnifiedRead(asio::posix::stream_descriptor &Connection, BufferT Buffer, CallbackT Callback)
        { Connection.async_read_some(std::forward<BufferT>(Buffer), std::forward<CallbackT>(Callback)); }

template <typename ConnectionPointerT, typename CallbackT>
	void LoopRead(
		ConnectionPointerT Connection, 
		CallbackT &&Callback)
{
	auto Buffer = std::make_shared<ReadBufferT>();
	std::cout << "Reading" << std::endl;
	LoopReadInternal(
		std::move(Connection),
		std::move(Buffer),
		std::move(Callback));
}

template <typename ConnectionPointerT, typename CallbackT>
	void LoopReadInternal(
		ConnectionPointerT Connection, 
		std::shared_ptr<ReadBufferT> Buffer, 
		CallbackT &&Callback)
{
	Buffer->Ensure(256);
	auto BufferStart = Buffer->EmptyStart();
	auto BufferSize = Buffer->Available();
	auto &ConnectionRef = *Connection;
	UnifiedRead(
		ConnectionRef,
		asio::buffer(BufferStart, BufferSize), 
		[Connection = std::move(Connection), Buffer = std::move(Buffer), Callback = std::move(Callback)](
			asio::error_code const &Error, 
			size_t ReadSize) mutable
		{
			if (Error)
			{
				std::cerr << "Error reading: (" << Error.value() << ") " << Error << std::endl;
				return;
			}
			Buffer->Fill(ReadSize);
			if (Callback(*Buffer))
				LoopReadInternal(
					std::move(Connection), 
					std::move(Buffer), 
					std::move(Callback));
		});
}
			
template <typename ConnectionPointerT>
	void Write(ConnectionPointerT Connection, std::string const &Data)
{
	auto Buffer = std::make_shared<std::string>(Data);
	auto const &BufferArg = asio::buffer(Buffer->c_str(), Buffer->size());
	auto &ConnectionRef = *Connection;
	asio::async_write(
		ConnectionRef, 
		BufferArg, 
		[Connection = std::move(Connection), Buffer = std::move(Buffer)]
			(asio::error_code const &Error, std::size_t WroteSize)
				{});
}

struct CallbackChainT
{
	typedef function<void(void)> CallbackT;
	struct AddT
	{
		AddT Add(CallbackT &&Callback)
		{
			return AddT(
				Chain, 
				++Chain.Callbacks.insert(Next, std::move(Callback)));
		}
		
		friend struct CallbackChainT;
		private:
			AddT(CallbackChainT &Chain, std::list<CallbackT>::iterator Next) : 
				Chain(Chain),
				Next(Next)
			{
			}

			CallbackChainT &Chain;
			std::list<CallbackT>::iterator Next;
	};

	AddT Add(CallbackT &&Callback)
	{
		return AddT(*this, Callbacks.begin()).Add(std::move(Callback));
	}

	void Next(void)
	{
		if (Callbacks.empty()) 
		{
			std::cout << "No chain callbacks, next does nothing." << std::endl;
			return;
		}
		auto Callback = std::move(Callbacks.front());
		Callbacks.pop_front();
		Callback();
	}

	private:
		std::list<CallbackT> Callbacks;
};

#endif

