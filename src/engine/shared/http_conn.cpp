#include <base/system.h>

#include "http.h"

CHttpConnection::CHttpConnection()
	: m_State(STATE_OFFLINE), m_LastActionTime(-1), m_pResponse(0), m_pRequest(0)
{
	mem_zero(&m_Addr, sizeof(m_Addr));
}

CHttpConnection::~CHttpConnection()
{
	Close();
	Reset();
}

void CHttpConnection::Reset()
{
	if(m_pResponse)
		delete m_pResponse;
	if(m_pRequest)
		delete m_pRequest;

	m_pResponse = 0;
	m_pRequest = 0;
}

void CHttpConnection::Close()
{
	if(m_State != STATE_OFFLINE)
		net_tcp_close(m_Socket);

	mem_zero(&m_Addr, sizeof(m_Addr));
	m_State = STATE_OFFLINE;
}

bool CHttpConnection::SetState(int State, const char *pMsg)
{
	if(pMsg)
		dbg_msg("http/conn", "%d: %s", m_ID, pMsg);
	bool Error = State == STATE_ERROR;
	if(Error)
		State = STATE_OFFLINE;

	if(State == STATE_WAITING || State == STATE_OFFLINE)
	{
		if(m_pRequest)
			m_pRequest->ExecuteCallback(m_pResponse, Error);
		Reset();
	}
	if(State == STATE_OFFLINE)
	{
		dbg_msg("http/conn", "%d: disconnecting", m_ID);
		Close();
	}
	m_State = State;
	return !Error;
}

bool CHttpConnection::CompareAddr(NETADDR Addr)
{
	if(m_State == STATE_OFFLINE)
		return false;
	return net_addr_comp(&m_Addr, &Addr) == 0;
}

bool CHttpConnection::Connect(NETADDR Addr)
{
	if(m_State != STATE_OFFLINE)
		return false;

	NETADDR BindAddr = { 0 };
	BindAddr.type = NETTYPE_IPV4;
	m_Socket = net_tcp_create(BindAddr);
	if(m_Socket.type == NETTYPE_INVALID)
		return SetState(STATE_ERROR, "error: could not create socket");

	net_set_non_blocking(m_Socket);

	m_Addr = Addr;
	net_tcp_connect(m_Socket, &m_Addr);
	m_LastActionTime = time_get();

	char aAddrStr[NETADDR_MAXSTRSIZE];
	net_addr_str(&m_Addr, aAddrStr, sizeof(aAddrStr), true);
	dbg_msg("http/conn", "%d: addr: %s", m_ID, aAddrStr);

	SetState(STATE_CONNECTING, "connecting");
	return true;
}

bool CHttpConnection::SetRequest(CRequest *pRequest)
{
	if(m_State != STATE_WAITING && m_State != STATE_CONNECTING)
		return false;
	m_pRequest = pRequest;
	m_pResponse = new CResponse();
	if(pRequest->Finalize())
	{
		m_LastActionTime = time_get();
		int NewState = m_State == STATE_CONNECTING ? STATE_CONNECTING : STATE_SENDING;
		return SetState(NewState, "new request");
	}
	return SetState(STATE_ERROR, "error: incomplete request");
}

bool CHttpConnection::Update()
{
	int Timeout = m_State == STATE_WAITING ? 90 : 5;
	if(m_State != STATE_OFFLINE && time_get() - m_LastActionTime > time_freq() * Timeout)
		return SetState(STATE_ERROR, "error: timeout");

	switch(m_State)
	{
		case STATE_CONNECTING:
		{
			int Result = net_socket_write_wait(m_Socket, 0);
			if(Result > 0)
			{
				m_LastActionTime = time_get();
				int NewState = m_pRequest ? STATE_SENDING : STATE_WAITING;
				return SetState(NewState, "connected");
			}
			else if(Result == -1)
				return SetState(STATE_ERROR, "error: could not connect");
		}
		break;

		case STATE_SENDING:
		{
			char aData[1024] = {0};
			int Bytes = m_pRequest->GetData(aData, sizeof(aData));
			if(Bytes < 0)
				return SetState(STATE_ERROR, "error: could not read request data");
			else if(Bytes > 0)
			{
				int Size = net_tcp_send(m_Socket, aData, Bytes);
				if(Size < 0)
					return SetState(STATE_ERROR, "error: sending data");

				m_LastActionTime = time_get();

				// resend if needed
				if(Size < Bytes)
					m_pRequest->MoveCursor(Size - Bytes);
			}
			else // Bytes = 0
				return SetState(STATE_RECEIVING, "sent request"); 
		}
		break;

		case STATE_RECEIVING:
		{
			bool Finished = false;
			char aBuf[1024] = {0};
			int Bytes = net_tcp_recv(m_Socket, aBuf, sizeof(aBuf));
			if(Bytes < 0)
			{
				if(!net_would_block())
					return SetState(STATE_ERROR, "error: receiving data");
				if(m_pResponse->IsComplete())
					Finished = true;
			}
			else if(Bytes > 0)
			{
				m_LastActionTime = time_get();
				if (!m_pResponse->Write(aBuf, Bytes))
					return SetState(STATE_ERROR, "error: could not read the response header");
			}
			else // Bytes = 0 (close)
				Finished = true;

			if(Finished)
			{
				if(!m_pResponse->Finalize())
					return SetState(STATE_ERROR, "error: incomplete response");

				const char *pConnStr = m_pResponse->GetField("Connection");
				bool Close = Bytes == 0 || (pConnStr && str_comp_nocase(pConnStr, "close") == 0);
				return SetState(Close ? STATE_OFFLINE : STATE_WAITING, "received response");
			}
		}
		break;

		case STATE_WAITING:
		{
			char aBuf[1024] = { 0 };
			int Bytes = net_tcp_recv(m_Socket, aBuf, sizeof(aBuf));
			if(Bytes < 0)
			{
				if (!net_would_block())
					return SetState(STATE_ERROR, "error: waiting");
			}
			else if(Bytes == 0)
				return SetState(STATE_OFFLINE, "remote closed");
		}
		break;
	}
	
	return 0;
}