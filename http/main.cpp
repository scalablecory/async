/*
	Experimental Async I/O
	Copyright (C) 2016 Cory Nelson

	This program is free software: you can redistribute it and/or modify
	it under the terms of version 3 of the GNU General Public License as
	published by the Free Software Foundation.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "stdafx.h"
#include "io.hpp"

class http_op : public io::async_op
{
	static const int RECVBUF_LEN = 128;

public:
	http_op(io::service &svc, char const *sendbuf, int sendbuf_len)
		: m_sendbuf(sendbuf)
		, m_sendleft(sendbuf_len)
		, m_svc(svc)
		, m_connect_ctx(this)
		, m_ctx(this)
		, m_state(on_start)
	{
	}

	void step()
	{
		// This realizes that I/O tends to be highly buffered, and many times an op can be completed
		// synchronously. An I/O call returning false means it -did not- complete synchronously, and
		// so the function must return to be resumed at a later time.
		// 
		// switch() is (ab)used to allow resumption from within deeper inside the function's loops
		// with minimal state tracking.

		switch (m_state)
		{
		case on_start:
			// connect and send our initial request.

			std::cout << "connecting..." << std::endl;

			m_state = on_connect;
			if (!io::connect_and_send(m_svc, m_sock, L"google.com", L"http", m_sendbuf, m_sendleft, m_connect_ctx))
				return;

		case on_connect:
			m_connect_ctx.check_error();
			std::cout << "connected and sent " << m_connect_ctx.transferred() << " bytes." << std::endl;
			m_sendbuf += m_connect_ctx.transferred();
			m_sendleft -= m_connect_ctx.transferred();

			// send out anything that didn't get sent during connect_and_send.

			while (m_sendleft)
			{
				std::cout << "sending " << m_sendleft << " bytes..." << std::endl;

				m_state = on_send;
				if (!m_sock.send(m_sendbuf, m_sendleft, 0, m_ctx))
					return;

			case on_send:
				m_ctx.check_error();
				std::cout << "sent " << m_ctx.transferred() << " bytes." << std::endl;
				m_sendbuf += m_ctx.transferred();
				m_sendleft -= m_ctx.transferred();
			}

			m_sock.shutdown(SD_SEND);

			// receive all data.

			m_recvbuf = std::unique_ptr<char []>(new char[RECVBUF_LEN]);

			do
			{
				std::cout << "receiving " << RECVBUF_LEN << " bytes..." << std::endl;

				m_state = on_receive;
				if (!m_sock.receive(m_recvbuf.get(), RECVBUF_LEN, 0, m_ctx))
					return;

			case on_receive:
				m_ctx.check_error();
				std::cout << "received " << m_ctx.transferred() << " bytes." << std::endl;
			} while (m_ctx.transferred());

			m_sock.shutdown(SD_RECEIVE);

			// finally disconnect the socket.

			std::cout << "disconnecting..." << std::endl;

			m_state = on_disconnect;
			if (!m_sock.disconnect(false, m_ctx))
				return;

		case on_disconnect:
			m_ctx.check_error();
			std::cout << "socket disconnected." << std::endl;
			m_sock.close();
			m_svc.shutdown();
		}
	}

private:
	enum { on_start, on_connect, on_send, on_receive, on_disconnect } m_state;

	ADDRINFOEXW m_dns_hints;

	char const *m_sendbuf;
	int m_sendleft;

	std::unique_ptr<char []> m_recvbuf;

	io::service& m_svc;

	io::async_context<io::resolve_and_connect_context> m_connect_ctx;
	io::async_context<io::socket_context> m_ctx;

	io::socket m_sock;
};

int main()
{
	static char const buffer [] = "GET / HTTP/1.1\r\nHost: google.com\r\n\r\n";

	io::service svc;

	try
	{
		http_op op{ svc, buffer, sizeof(buffer) - 1 };
		op.step();

		while (svc.run_many())
		{
			std::cout << "async op completed." << std::endl;
		}
	}
	catch (io::win32_error const &ex)
	{
		std::cerr << "win32 error: 0x" << std::hex << ex.error() << " (" << std::dec << ex.error() << ")" << std::endl;
	}
	catch (std::exception const &ex)
	{
		std::cerr << "exception: " << ex.what() << std::endl;
	}

	std::cout << "closing..." << std::endl;

    return 0;
}
