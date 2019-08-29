#ifdef __linux__
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "EventHandler.h"
#include "Buffer.h"
#include "Log.h"
#include "EventActions.h"
#include "SocketImpl.h"
#include "Runnable.h"
#include "LinuxFunc.h"
#include "CppNetImpl.h"

using namespace cppnet;

CSocketImpl::CSocketImpl(std::shared_ptr<CEventActions>& event_actions) : CSocketBase(event_actions){
	_read_event = base::MakeNewSharedPtr<CEventHandler>(_pool.get());
	_write_event = base::MakeNewSharedPtr<CEventHandler>(_pool.get());

    _read_event->_data = _pool->PoolNew<epoll_event>();
    ((epoll_event*)_read_event->_data)->events = 0;
    _read_event->_buffer = base::MakeNewSharedPtr<base::CBuffer>(_pool.get(), _pool);

    _write_event->_data = _pool->PoolNew<epoll_event>();
    ((epoll_event*)_write_event->_data)->events = 0;
    _write_event->_buffer = base::MakeNewSharedPtr<base::CBuffer>(_pool.get(), _pool);
}

CSocketImpl::~CSocketImpl() {
    base::LOG_DEBUG("delete from epoll, socket : %d, TheadId : %d", _sock, std::this_thread::get_id());
	//delete from epoll
	if (_event_actions) {
		if (_event_actions->DelEvent(_read_event)) {
			base::CRunnable::Sleep(100);
			close(_sock);
		}
	}
	
    // remove from epoll
	if (_read_event && _read_event->_data) {
		epoll_event* temp = (epoll_event*)_read_event->_data;
		_pool->PoolDelete<epoll_event>(temp);
		_read_event->_data = nullptr;
	}
	if (_write_event && _write_event->_data) {
		epoll_event* temp = (epoll_event*)_write_event->_data;
		_pool->PoolDelete<epoll_event>(temp);
		_write_event->_data = nullptr;
	}
}

void CSocketImpl::SyncRead() {
	if (_event_actions) {
		_read_event->_event_flag_set |= EVENT_READ;
		_event_actions->AddRecvEvent(_read_event);
	}
}

void CSocketImpl::SyncWrite(const char* src, uint32_t len) {
	if (!_write_event->_client_socket) {
		_write_event->_client_socket = _read_event->_client_socket;
	}

    _write_event->_event_flag_set |= EVENT_WRITE;

	//try send
	_write_event->_buffer->Write(src, len);
	_Send(_write_event);

	// if not send complete. _Send function will add send event to action.
	// if (_write_event->_buffer->GetCanReadSize() == 0) {
	// 	return;
	// }

	// if (_event_actions) {
	// 	_event_actions->AddSendEvent(_write_event);
	// }
}

void CSocketImpl::SyncConnection(const std::string& ip, uint16_t port) {
	if (ip.length() > 16) {
        base::LOG_ERROR("a wrong ip! %s", ip.c_str());
		return;
	}
	strcpy(_ip, ip.c_str());
	_port = port;

	if (!_read_event->_client_socket){
		_read_event->_client_socket = memshared_from_this();
	}

	if (_event_actions) {
		_read_event->_event_flag_set |= EVENT_CONNECT;
		_event_actions->AddConnection(_read_event, ip, port);
	}
}

void CSocketImpl::SyncDisconnection() {
	if (!_read_event->_client_socket) {
		_read_event->_client_socket = memshared_from_this();
	}

	if (_event_actions) {
		_read_event->_event_flag_set |= EVENT_DISCONNECT;
		_event_actions->AddDisconnection(_read_event);
	}
}

void CSocketImpl::SyncRead(uint32_t interval) {

    SyncRead();

	if (_event_actions) {
		_read_event->_event_flag_set |= EVENT_TIMER;
		_event_actions->AddTimerEvent(interval, _read_event);
	}
}

void CSocketImpl::SyncWrite(uint32_t interval, const char* src, uint32_t len) {
	
    SyncWrite(src, len);

	if (_event_actions) {
		_write_event->_event_flag_set |= EVENT_TIMER;
		_event_actions->AddTimerEvent(interval, _write_event);
	}
}

void CSocketImpl::PostTask(std::function<void(void)>& func) {
	_event_actions->PostTask(func);
}

bool cppnet::operator>(const CSocketBase& s1, const CSocketBase& s2) {
	return s1._sock > s2._sock;
}

bool cppnet::operator<(const CSocketBase& s1, const CSocketBase& s2) {
	return s1._sock < s2._sock;
}

bool cppnet::operator==(const CSocketBase& s1, const CSocketBase& s2) {
	return s1._sock == s2._sock;
}

bool cppnet::operator!=(const CSocketBase& s1, const CSocketBase& s2) {
	return s1._sock != s2._sock;
}

void CSocketImpl::_Recv(base::CMemSharePtr<CEventHandler>& event) {
	if (!event->_client_socket) {
        base::LOG_WARN("the event with out socket");
		return;
	}
	auto socket_ptr = event->_client_socket.Lock();
	if (!socket_ptr) {
        base::LOG_WARN("the event'socket is destroyed");
		return;
	}
	int err = event->_event_flag_set;
	if (event->_event_flag_set & EVENT_TIMER) {
		err |= ERR_TIME_OUT;
		event->_event_flag_set &= ~EVENT_TIMER;

	//get a connection event
	} else if (event->_event_flag_set & EVENT_CONNECT) {
		event->_event_flag_set &= ~EVENT_CONNECT;

	} else if (event->_event_flag_set & EVENT_DISCONNECT) {
		event->_event_flag_set &= ~EVENT_DISCONNECT;

	} else {
		if (event->_event_flag_set & EVENT_READ) {
			event->_off_set = 0;
			//read all data.
			for (;;) {
				char buf[65536] = { 0 };
				int recv_len = 0;
				recv_len = recv(socket_ptr->GetSocket(), buf, 65536, 0);
				if (recv_len < 0) {
					if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) {
						break;

					} else if (errno == EBADMSG || errno == ECONNRESET) {
						err |= ERR_CONNECT_BREAK;
						base::LOG_ERROR("recv filed! %d", errno);
						break;

					} else {
						err |= ERR_CONNECT_BREAK;
                        base::LOG_ERROR("recv filed! %d", errno);
						break;
					}
				} else if (recv_len == 0) {
					err |= ERR_CONNECT_CLOSE;
					base::LOG_DEBUG("socket read 0 to close! %d", socket_ptr->GetSocket());
					break;
				}
				event->_buffer->Write(buf, recv_len);
				event->_off_set += recv_len;
				memset(buf, 0, recv_len);
			}
		}
	}
    CCppNetImpl::Instance()._ReadFunction(event, err);
}

void CSocketImpl::_Send(base::CMemSharePtr<CEventHandler>& event) {
	if (!event->_client_socket) {
        base::LOG_WARN("the event with out socket");
		return;
	}
	auto socket_ptr = event->_client_socket.Lock();
	if (!socket_ptr) {
        base::LOG_WARN("the event'socket is destroyed");
		return;
	}

	int err = event->_event_flag_set;
	if (event->_event_flag_set & EVENT_TIMER) {
		err |= ERR_TIME_OUT;
		event->_event_flag_set &= ~EVENT_TIMER;

	} else {
		event->_off_set = 0;
		if (event->_buffer && event->_buffer->GetCanReadSize()) {
			char buf[8912] = { 0 };
			int send_len = 0;
			send_len = event->_buffer->Read(buf, 8912);
			base::LOG_ERROR("buf : %s, len : %d", buf, send_len);
			int res = send(socket_ptr->GetSocket(), buf, send_len, 0);
			base::LOG_ERROR("res : %d err : %d", res, errno);
			if (res >= 0) {
				event->_buffer->Clear(res);
				//can send complete
				if (res < send_len) {
					_event_actions->AddSendEvent(_write_event);
				}

			} else {
				if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) {
					//wait next time to do

				} else if (errno == EBADMSG) {
					err |= ERR_CONNECT_BREAK;
					base::LOG_ERROR("send filed! %d", errno);

				} else {
					err |= ERR_CONNECT_CLOSE;
                    base::LOG_ERROR("send filed! %d", errno);
				}
			}
			event->_off_set = res;
		}
		
        CCppNetImpl::Instance()._WriteFunction(event, err);
	}
}
#endif