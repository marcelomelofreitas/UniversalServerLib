//
//Copyright(c) 2016. Huan Xia
//
//Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated 
//documentation files(the "Software"), to deal in the Software without restriction, including without limitation
//the rights to use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies of the Software,
//and to permit persons to whom the Software is furnished to do so, subject to the following conditions :
//
//The above copyright notice and this permission notice shall be included in all copies or substantial portions
//of the Software.
//
//THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
//TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL
//THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
//CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//DEALINGS IN THE SOFTWARE.

#include "STXIOCPServer.h"
#include "STXServerBase.h"
//#include "Global.h"
#include <shlwapi.h>
#include <iterator>

#pragma comment(lib,"shlwapi.lib")
#pragma comment(lib,"wininet.lib")

//////////////////////////////////////////////////////////////////////////
// Function, macro and structure used for setting thread name. used for debugging.

const DWORD MS_VC_EXCEPTION = 0x406D1388;

#pragma pack(push,8)
typedef struct tagTHREADNAME_INFO
{
	DWORD dwType; // Must be 0x1000.
	LPCSTR szName; // Pointer to name (in user addr space).
	DWORD dwThreadID; // Thread ID (-1=caller thread).
	DWORD dwFlags; // Reserved for future use, must be zero.
} THREADNAME_INFO;
#pragma pack(pop)

void SetThreadName(DWORD dwThreadID, char* threadName)
{
	THREADNAME_INFO info;
	info.dwType = 0x1000;
	info.szName = threadName;
	info.dwThreadID = dwThreadID;
	info.dwFlags = 0;

	__try
	{
		RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
}

//////////////////////////////////////////////////////////////////////////
// Helper Macros

#define NEW_OPERATION(xPtrVariableName)		LPSTXIOCPOPERATION xPtrVariableName = new STXIOCPOPERATION(_T(__FILE__), __LINE__);

#define BEGIN_TRY()\
__try\
{\
	__try\
	{

#define END_TRY()\
	}\
	__except (ProcessException(GetExceptionInformation()))\
	{\
	}\
}\
__finally\
{\
}


//////////////////////////////////////////////////////////////////////////
// CSTXIOCPServer
//////////////////////////////////////////////////////////////////////////

LONG CSTXIOCPServer::s_nTcpConnectionIDBase = 0;

CSTXIOCPServer::CSTXIOCPServer(void)
{
	_lpfnDisconnectEx = NULL;
	_lpfnConnectEx = NULL;

	m_hIOCP = NULL;
	m_dwIOWait = 1000;		// in milliseconds

	m_hModuleHandle = NULL;

	WSADATA wsadata;
	WSAStartup(MAKEWORD(2, 2), &wsadata);

	InitializeCriticalSection(&m_csServersMap);
	InitializeCriticalSection(&m_csPending);
	InitializeCriticalSection(&m_csConnections);
	InitializeCriticalSection(&m_csKillAllConnectionsLock);
	InitializeCriticalSection(&m_csMonitoredDirSet);
	InitializeCriticalSection(&m_csFileDictionary);

	m_hServerUnlockEvent = CreateEvent(NULL, TRUE, TRUE, NULL);
}

CSTXIOCPServer::~CSTXIOCPServer(void)
{
//	STXTRACELOGE(_T("CSTXIOCPServer::~CSTXIOCPServer() begin"));
	//WSACleanup();
	DeleteCriticalSection(&m_csFileDictionary);
	DeleteCriticalSection(&m_csServersMap);
	DeleteCriticalSection(&m_csPending);
	DeleteCriticalSection(&m_csConnections);
	DeleteCriticalSection(&m_csKillAllConnectionsLock);
	DeleteCriticalSection(&m_csMonitoredDirSet);

	CloseHandle(m_hServerUnlockEvent);

	if(m_hIOCP)
	{
		::CloseHandle(m_hIOCP);
		m_hIOCP = NULL;
	}
}

BOOL CSTXIOCPServer::OnInitialization()
{
	if(!CSTXServerBase::OnInitialization())
	{
		OutputLogDirect(_T("\nCSTXServerBase::OnInitialization Failed!"));
		return FALSE;
	}

	OutputLogDirect(_T("\r\n---------------------------------- OnInitialization ----------------------------------"));

	m_hIOCP = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);

	if(m_hIOCP == NULL)
	{
		STXTRACELOGE(_T("[r][i]Failed to create IO Completion Port!"));
		return FALSE;
	}

	STXTRACELOGE(_T("IO Completion Port:\t0x%p"), m_hIOCP);

	if(CreateWorkerThreads() == 0)
	{
		CloseHandle(m_hIOCP);
		m_hIOCP = NULL;
		return FALSE;
	}

	STXTRACELOGE(_T("Worker Threads created!:\t%d Threads"), m_arrWorkerThreads.size());

	if(m_Buffers.CreateBuffers(m_BaseServerInfo.dwBufferInitialCount, m_BaseServerInfo.dwBufferSize, m_BaseServerInfo.dwBufferMaxCount) == 0)
	{
		CloseHandle(m_hIOCP);
		return FALSE;
	}

	TCHAR szBufferSizeFormated[64];
	StrFormatByteSize(m_BaseServerInfo.dwBufferSize, szBufferSizeFormated, 64);
	STXTRACELOGE(_T("R/W Buffer Size:\t%d Bytes (%s)"), m_BaseServerInfo.dwBufferSize, szBufferSizeFormated);
	STXTRACELOGE(_T("R/W Buffer Number:\t%d/%d\t\tMaximum: %u"), m_Buffers.GetBufferAvailableCount(), m_Buffers.GetBufferTotalCount(), m_BaseServerInfo.dwBufferMaxCount);

	OnServerInitialized();

	return TRUE;
}

void CSTXIOCPServer::OnServerInitialized()
{

}

LRESULT CSTXIOCPServer::Run()
{
	HANDLE *arrhWorkerThreads = new HANDLE[m_arrWorkerThreads.size()];
	int iPos = 0;
	vector<HANDLE>::iterator it = m_arrWorkerThreads.begin();
	for(;it!=m_arrWorkerThreads.end();it++)
	{
		HANDLE hThread = (HANDLE)(*it);
		arrhWorkerThreads[iPos++] = hThread;
	}

//	::SetEvent(m_ServerInfo.hTerminateSignal);
	::WaitForMultipleObjects((DWORD)m_arrWorkerThreads.size(), arrhWorkerThreads, TRUE, INFINITE);

	STXTRACELOGE(_T("All %d Worker Threads terminated successfully."), m_arrWorkerThreads.size());

	delete []arrhWorkerThreads;

	ClearAllServers();
	ClearAllClient();
	ClearAllFolderMonitor();

	it = m_arrWorkerThreads.begin();
	for(;it!=m_arrWorkerThreads.end();it++)
	{
		HANDLE hThread = (HANDLE)(*it);
		CloseHandle(hThread);
	}
	m_arrWorkerThreads.erase(m_arrWorkerThreads.begin(), m_arrWorkerThreads.end());

	STXTRACELOGE(_T("Server Terminated!"));
	STXTRACELOGE(_T("Current Client Context: \t%d"), m_mapClientContext.size());
	STXTRACELOGE(_T("Current R/W Buffers: \t%d/%d  (Available/Maximum)"), m_Buffers.GetBufferAvailableCount(), m_Buffers.GetBufferTotalCount());
	STXTRACELOGE(_T("Current queued Read: \t%d"), m_queueRead.size());
	STXTRACELOGE(_T("Current uncomplete Operation: \t%d"), m_setOperation.size());

	OnShutDown();
	STXTRACELOGE(_T("OnShutDown Called."));

	m_Buffers.ClearBuffers();

	STXTRACELOGE(_T("OnFinalShutDown..."));
	OnFinalShutDown();
	SetEvent(m_hServerUnlockEvent);

	return 0;
}

#ifdef MSC_VER
__declspec(thread) static LPVOID g_pThreadTlsUserData = NULL;
#else
thread_local static LPVOID g_pThreadTlsUserData = NULL;
#endif

UINT WINAPI CSTXIOCPServer::WorkThreadProc(LPVOID lpParam)
{
	SetThreadName(-1, "IOCPWorkerThread");

	CSTXIOCPServer *pThis = (CSTXIOCPServer*)lpParam;
	BOOL bResult = FALSE;
	DWORD dwNumReadWrite = 0;
	LPSTXIOCPCONTEXTKEY pContextKey = NULL;
	HANDLE hCompletionPort = pThis->m_hIOCP;
	DWORD dwLastError = 0;

	LPSTXIOCPOPERATION pOperation = NULL;

#ifdef MSC_VER
	__declspec(thread) static int nOpProcessCount  = 0;
#else
	thread_local static int nOpProcessCount = 0;
#endif

	g_pThreadTlsUserData = pThis->OnAllocateWorkerThreadLocalStorageWrapper();
	pThis->OnWorkerThreadInitializeWrapper(g_pThreadTlsUserData);

	while (TRUE)
	{
		pContextKey = NULL;
		dwNumReadWrite = 0;
		pOperation = NULL;

		SetLastError(0);
		bResult = GetQueuedCompletionStatus(hCompletionPort, &dwNumReadWrite, (PULONG_PTR)&pContextKey, (LPOVERLAPPED*)&pOperation, pThis->m_dwIOWait);
		dwLastError = GetLastError();

		if(dwLastError != 0 && dwLastError != WAIT_TIMEOUT && dwLastError != ERROR_OPERATION_ABORTED && !bResult)
		{
			STXTRACE(_T("[r][g][E]"));
		}

		if(pOperation)
		{
			pOperation->MarkProcessed();
		}


		// Check pending Reading
		//EnterCriticalSection(&pThis->m_csPending);
		//if(pThis->m_queueRead.size() > 0)
		//{
		//	STXTRACE(_T("Dequeue Pending Read Operation..."));
		//	READPENDING &readOp = pThis->m_queueRead.front();
		//	pThis->m_queueRead.pop();
		//}
		//LeaveCriticalSection(&pThis->m_csPending);


		//GetQueuedCompletionStatus TimeOut, Do not do further process
		if(!bResult && dwLastError == WAIT_TIMEOUT)
		{
			//STXTRACE(_T("Queued operation: %d"), pThis->m_queueOperationToCheck.size());

			if((pThis->m_BaseServerInfo.dwStatus & STXSS_TERMINATE) && pThis->CanTerminateWorkThreads())
			{
				pThis->OnWorkerThreadUninitializeWrapper(g_pThreadTlsUserData);
				STXTRACELOGE(_T("[g][i] Work Thread 0x%x Terminated! %7d Operations processed."), GetCurrentThreadId(), nOpProcessCount);

				if(pOperation)
					pOperation->Release();

				return 0;
			}

			if(pOperation)
				pOperation->Release();

			continue;
		}

		pThis->OnWorkerThreadPreOperationProcessWrapper(g_pThreadTlsUserData);

		if(pOperation->nSocketType == STXIOCP_CONTEXTKEY_SOCKET_TYPE_FAKE)
		{
			pThis->OnInternalCustomOperationComplete(pOperation->nOpType, pOperation->dwOperationID, pOperation->dwUserData, pOperation->dwUserData2, pOperation->dwCompleteType);
			pThis->OnCustomOperationCompleteWrapper(pOperation->dwOperationID, pOperation->dwUserData, pOperation->dwCompleteType);
			STXLOGVERIFY(pThis->DequeueOperation(pOperation));
			pOperation->Release();
			continue;
		}

		STXLOGASSERT(pContextKey != NULL);
		EXCEPTION_POINTERS *pExp = NULL;
		//__try
		{
			//__try
			{

				switch(pOperation->nSocketType)
				{
				case STXIOCP_CONTEXTKEY_SOCKET_TYPE_TCPSERVER:
					pThis->WTProcessTcpServer(bResult, dwLastError, dwNumReadWrite, pContextKey, pOperation);
					break;
				case STXIOCP_CONTEXTKEY_SOCKET_TYPE_UDPSERVER:
					pThis->WTProcessUdpServer(bResult, dwLastError, dwNumReadWrite, pContextKey, pOperation);
					break;
				case STXIOCP_CONTEXTKEY_SOCKET_TYPE_CLIENT:
					pThis->WTProcessClientSocket(bResult, dwLastError, dwNumReadWrite, pContextKey, pOperation);
					break;
				case STXIOCP_CONTEXTKEY_SOCKET_TYPE_TCP_CONNECTION:
					pThis->WTProcessTcpConnection(bResult, dwLastError, dwNumReadWrite, pContextKey, pOperation);
					break;
				case STXIOCP_CONTEXTKEY_SOCKET_TYPE_MONITORED_DIR:
					pThis->WTProcessDirMonitor(bResult, dwLastError, dwNumReadWrite, pContextKey, pOperation);
					break;
				case STXIOCP_CONTEXTKEY_SOCKET_TYPE_FILE:
					pThis->WTProcessFileRead(bResult, dwLastError, dwNumReadWrite, pContextKey, pOperation);
					break;
				case STXIOCP_CONTEXTKEY_SOCKET_TYPE_HTTP:
					pThis->WTProcessHttpOperation(bResult, dwLastError, dwNumReadWrite, pContextKey, pOperation);
					break;
				default:
					STXTRACELOGE(_T("[r][i]Fatal Error! Unknown Socket Type!"));
				}
			}
			//__except(ProcessException(GetExceptionInformation()))
			{
			}
		}
		//__finally
		{
			LONG nDeque = pThis->DequeueOperation(pOperation);
			//pOperation->MarkProcessed();
			if(nDeque == 0)
			{
				STXTRACELOGFE(_T("DequeueOperation Failed for Operation 0x%X"), pOperation);
			}
			nOpProcessCount++;
			pOperation->Release();
		}

	}//end while

	pThis->OnWorkerThreadUninitializeWrapper(g_pThreadTlsUserData);
	pThis->OnFreeWorkerThreadLocalStorageWrapper(g_pThreadTlsUserData);
	g_pThreadTlsUserData = NULL;
	return 0;
}

void CSTXIOCPServer::WTProcessTcpServer(BOOL bResult, DWORD dwLastError, DWORD dwNumReadWrite, LPSTXIOCPCONTEXTKEY lpContextKey, LPSTXIOCPOPERATION lpOperation)
{
	tr1::shared_ptr<CSTXIOCPTcpServerContext> pServerContext = lpContextKey->pTcpServerContext;
	if(!bResult && dwLastError != 0)		//TCP Listening socket is closed
	{
		if((m_BaseServerInfo.dwStatus & STXSS_PRETERMINATE) || pServerContext->IsClosed())
		{
			delete []lpOperation->pszOutputBuffer;
			closesocket(lpOperation->sockNewAccept);	//close the socket created for accept

			LONG nRef = InterlockedDecrement(&lpContextKey->nAcceptPending);
			if(nRef == 0)
			{
				//STXTRACE(_T("TCP Sub Server unlocked from AcceptEx operations. It's now safe to delete ContextKey."));
				pServerContext->OnDestroy();
				delete lpContextKey;		
			}
		}
		else
		{
			if(lpOperation->nIPVer == STXIOCP_SOCKET_IPV4)
				IssueAccept(lpContextKey, STXIOCP_ACCEPT_IPV4);
			else
				IssueAccept(lpContextKey, STXIOCP_ACCEPT_IPV6);

			delete []lpOperation->pszOutputBuffer;
			closesocket(lpOperation->sockNewAccept);
		}

	}
	else if(dwNumReadWrite == 0 && bResult)		//Ready for Accept a new incoming connection
	{
		InterlockedDecrement(&lpContextKey->nAcceptPending);

		// 1. Allocate IO ContextKey
		// 2. Bind to IOCP
		// 3. Create and Initialize Client Context
		// 4. Fire OnAccepted
		// 5. Issue Read
		// 6. Issue new Accept
		// 7. Clear Address memory allocated in the Pre-Accept


		// 1, 2, 3
		CSTXIOCPServerClientContext *pClientContext;
		LPSTXIOCPCONTEXTKEY pAcceptedSockContextKey = Accept((SOCKET)lpOperation->sockNewAccept, lpContextKey->pTcpServerContext, (LPVOID)lpOperation->pszOutputBuffer, &pClientContext, lpOperation);

		BOOL bOK = TRUE;
		if(pAcceptedSockContextKey)
		{
			// 4
			OnAccepted(pClientContext);

			// 5
			DWORD dwTimeOut = OnGetClientReceiveTimeOutWrapper(pClientContext);
			pClientContext->Lock();
			bOK = IssueClientRead(pClientContext, dwTimeOut);
			pClientContext->Unlock();
		}
		else
		{
			STXTRACELOGE(_T("[r][i]pAcceptedSockContextKey == NULL"));
		}

		// 6
		if(lpOperation->nIPVer == STXIOCP_SOCKET_IPV4)
			IssueAccept(lpContextKey, STXIOCP_ACCEPT_IPV4);
		else
		{
			//STXTRACE(_T("IssueAccept(lpContextKey, STXIOCP_ACCEPT_IPV6)"));
			IssueAccept(lpContextKey, STXIOCP_ACCEPT_IPV6);
		}

		if(!bOK)
		{
			STXTRACELOGE(_T("[r][i]IssueRead failed for client socket in Accept."));

			pClientContext->AddRef();
			delete pAcceptedSockContextKey;
			ReleaseClient(pClientContext);
			pClientContext->Release();
		}

		// 7
		delete []lpOperation->pszOutputBuffer;
	}
	else
	{
		InterlockedDecrement(&lpContextKey->nAcceptPending);

		STXTRACELOGE(_T("[r][i]<Accept>! Unknown Accept! Delete buffer allocated for AcceptEx. Issue a new Accept!"));
		IssueAccept(lpContextKey);
		delete []lpOperation->pszOutputBuffer;
	}
}

void CSTXIOCPServer::WTProcessUdpServer(BOOL bResult, DWORD dwLastError, DWORD dwNumReadWrite, LPSTXIOCPCONTEXTKEY lpContextKey, LPSTXIOCPOPERATION lpOperation)
{
	if(dwNumReadWrite == 0)
	{
		m_Buffers.ReleaseBuffer(lpOperation->pBuffer);

		CSTXServerObjectPtr<CSTXIOCPUdpServerContext> pServerContext = lpContextKey->pUdpServerContext;

		if(pServerContext->GetSocket() == lpOperation->sock)
			pServerContext->CloseSocket();
		else
			pServerContext->CloseSocket6();

		LONG ref = InterlockedDecrement(&lpContextKey->nUdpServerPending);

		if(ref == 0)
		{
			delete lpContextKey;
		}
	}
	else
	{
		if(lpOperation->nOpType == STXIOCP_OPERATION_READ)
		{
			lpOperation->pBuffer->SetDataLength(dwNumReadWrite);

			CSTXIOCPUdpServerContext *pServerContext = lpContextKey->pUdpServerContext;

			int nAddressFamily = (lpOperation->nIPVer == STXIOCP_ACCEPT_IPV6 ? AF_INET6 : AF_INET);
			IssueUdpRead(pServerContext, 0, nAddressFamily);
			OnUdpServerReceivedWrapper(pServerContext, lpOperation->pBuffer, (SOCKADDR*)lpOperation->szUdpAddressBuffer, lpOperation->nUdpAddressLen);

			m_Buffers.ReleaseBuffer(lpOperation->pBuffer);
		}
		else if(lpOperation->nOpType == STXIOCP_OPERATION_WRITE)
		{
			CSTXIOCPUdpServerContext *pServerContext = lpContextKey->pUdpServerContext;
			lpOperation->pBuffer->SetDataLength(dwNumReadWrite);

			OnUdpServerSentWrapper(pServerContext, lpOperation->pBuffer);
			m_Buffers.ReleaseBuffer(lpOperation->pBuffer);
		}			
	}
}

void CSTXIOCPServer::WTProcessClientSocket(BOOL bResult, DWORD dwLastError, DWORD dwNumReadWrite, LPSTXIOCPCONTEXTKEY lpContextKey, LPSTXIOCPOPERATION lpOperation)
{
	//if (lpOperation->nOpType == STXIOCP_OPERATION_WRITE)
	//{
	//	static int i = 0;
	//	i++;
	//	if ((i % 100) == 0)
	//	{
	//		i = 0;
	//		STXTRACEE(_T("[r][i]dwNumReadWrite for client write!, error = %d, bResult = %d, dwNumReadWrite = %d"), dwLastError, bResult, dwNumReadWrite);
	//	}
	//}

	if(lpOperation->nOpType == STXIOCP_OPERATION_DISCONNECT)		//服务器主动断开连接
	{
		CSTXIOCPServerClientContext *pClientContext = lpOperation->pClientContext;
		pClientContext->CloseSocket();
		//pClientContext->Release();
		//STXTRACE(_T("[i]客户端已经断开,由于服务器主动断开连接. No notify"));
	}
	else if(dwNumReadWrite == 0)
	{
		// dwNumRead == 0, bResult == 0 or 1 : Close (Gracefully)


			if(lpOperation->nOpType == STXIOCP_OPERATION_READ)
			{
				InterlockedIncrement(&_numGracefulDisconnect);
				//STXTRACE(_T("客户端已经断开."));
				OnClientDisconnectWrapper(lpOperation->pClientContext);
				lpContextKey->pClientContext->OnDestroy();
				OnPostClientDisconnectWrapper(lpOperation->pClientContext);
				ReleaseClient(lpContextKey->pClientContext);
				delete lpContextKey;
			}
			else if (lpOperation->nOpType == STXIOCP_OPERATION_WRITE)
			{
				lpOperation->pClientContext->LockSend();

				LONG64 nCurrentlyBuffered = lpOperation->pClientContext->DecreaseBufferedSendLength(lpOperation->pBuffer->GetDataLength());
				STXTRACELOGE(_T("[r][i]dwNumReadWrite for client write!, error = %d"), dwLastError);
				delete lpOperation->pBuffer;

				if (nCurrentlyBuffered > 0 && lpOperation->pClientContext->GetSocket() != (SOCKET)INVALID_HANDLE_VALUE && !lpOperation->pClientContext->IsDisconnected())
				{
					CSTXIOCPBuffer *pBuffer = NULL;
					bool bFound = lpOperation->pClientContext->_queuedSendData.try_dequeue(pBuffer);
					size_t nqueued = lpOperation->pClientContext->_queuedSendData.size_approx();
					if (nqueued > 60)
					{
						static int i = 0;
						i++;
						if ((i % 200) == 0)
						{
							STXTRACEE(_T("Client %s has queued %d packages to be sent."), lpOperation->pClientContext->OnGetClientDisplayName(), nqueued);
						}
					}

					if (bFound)
					{
						lpOperation->pClientContext->DecreaseBufferedSendLength(pBuffer->GetDataLength());
						DWORD dwSent = IssueClientWriteForceSend(lpOperation->pClientContext, pBuffer->GetBufferPtr(), pBuffer->GetDataLength(), pBuffer->GetUserData(), 0);
						delete pBuffer;
					}
				}

				lpOperation->pClientContext->UnlockSend();
			}
		
	}
	else
	{
		if(lpOperation->nOpType == STXIOCP_OPERATION_READ)
		{
			DWORD dwTimeOut = OnGetClientReceiveTimeOutWrapper(lpOperation->pClientContext);
			BOOL bContinueRead = TRUE;
			BOOL bReadAppened  = TRUE;
			lpOperation->pBuffer->SetDataLength(dwNumReadWrite);

			bContinueRead = PreClientReceive(lpOperation->pClientContext, lpOperation->pBuffer, lpContextKey->pTcpServerContext);
			bReadAppened = IssueClientRead(lpOperation->pClientContext, dwTimeOut);

			if(!bReadAppened)
			{
				InterlockedIncrement(&_numUnexpectDisconnect);

				//STXTRACELOGE(_T("[i][r]IssueRead 失败!"));
				//由于 IssueRead 失败，将导致后续的断线消息得不到通知。此处必须主动处理断线通知
				OnClientDisconnectWrapper(lpOperation->pClientContext);
				lpContextKey->pClientContext->OnDestroy();
				OnPostClientDisconnectWrapper(lpOperation->pClientContext);
				ReleaseClient(lpContextKey->pClientContext);
				delete lpContextKey;
			}
		}
		else if(lpOperation->nOpType == STXIOCP_OPERATION_WRITE)
		{
			//STXTRACEE(_T("dwNumReadWrite = %d"), dwNumReadWrite);

			lpOperation->pClientContext->LockSend();
			lpOperation->pBuffer->SetDataLength(dwNumReadWrite);
			LONG64 nCurrentlyBuffered = lpOperation->pClientContext->DecreaseBufferedSendLength(dwNumReadWrite);
			OnClientSentWrapper(lpOperation->pClientContext, lpOperation->pBuffer);
			delete lpOperation->pBuffer;

			//LONG64 nCurrentlyBuffered = pClientContext->GetBufferedSendLength();
			if (nCurrentlyBuffered > 0)
			{
				CSTXIOCPBuffer *pBuffer = NULL;
				bool bFound = lpOperation->pClientContext->_queuedSendData.try_dequeue(pBuffer);
				size_t nqueued = lpOperation->pClientContext->_queuedSendData.size_approx();
				if (nqueued > 60)
				{
					static int i = 0;
					i++;
					if ((i % 200) == 0)
					{
						STXTRACEE(_T("Client %s has queued %d packages to be sent."), lpOperation->pClientContext->OnGetClientDisplayName(), nqueued);
					}
				}

				if (bFound)
				{
					lpOperation->pClientContext->DecreaseBufferedSendLength(pBuffer->GetDataLength());
					DWORD dwSent = IssueClientWriteForceSend(lpOperation->pClientContext, pBuffer->GetBufferPtr(), pBuffer->GetDataLength(), pBuffer->GetUserData(), 0);
					delete pBuffer;
				}
			}

			lpOperation->pClientContext->UnlockSend();
			//m_Buffers.ReleaseBuffer(lpOperation->pBuffer);
		}
	}
}

void CSTXIOCPServer::WTProcessTcpConnection(BOOL bResult, DWORD dwLastError, DWORD dwNumReadWrite, LPSTXIOCPCONTEXTKEY lpContextKey, LPSTXIOCPOPERATION lpOperation)
{
	CSTXIOCPTcpConnectionContext *pTcpConnCtx = lpContextKey->pTcpConnectionContext;
	if(dwNumReadWrite == 0)
	{
		if(lpOperation->nOpType == STXIOCP_OPERATION_CONNECT)
		{
			if(bResult)		//Connect successfully
			{
				pTcpConnCtx->MarkConnected();
				OnTcpConnectWrapper(pTcpConnCtx);
				IssueTcpConnectionRead(pTcpConnCtx, 0);
				return;
			}
			else			//Failed to connect
			{
				EnterCriticalSection(&m_csKillAllConnectionsLock);
				PreTcpDisconnect(pTcpConnCtx, dwLastError);
				LeaveCriticalSection(&m_csKillAllConnectionsLock);

				return;
			}
		}
		else if(bResult)	// dwNumRead == 0, bResult == 1 : Close Gracefully
		{
			pTcpConnCtx->MarkDisonnected();
			closesocket(lpContextKey->sock);
			m_Buffers.ReleaseBuffer(lpOperation->pBuffer);

			PreTcpDisconnect(pTcpConnCtx, dwLastError);
		}
		else	//dwNumRead == 0, bResult == 0 : Close
		{
			pTcpConnCtx->MarkDisonnected();
			closesocket(lpContextKey->sock);
			m_Buffers.ReleaseBuffer(lpOperation->pBuffer);
			PreTcpDisconnect(pTcpConnCtx, dwLastError);
		}
		//delete pContextKey;
	}
	else
	{
		if(lpOperation->nOpType == STXIOCP_OPERATION_READ)
		{
			CSTXIOCPTcpConnectionContext *pTcpConnCtx = lpContextKey->pTcpConnectionContext;

			lpOperation->pBuffer->SetDataLength(dwNumReadWrite);
			PreTcpReceive(pTcpConnCtx, lpOperation->pBuffer);

			int nAddressFamily = (lpOperation->nIPVer == STXIOCP_ACCEPT_IPV6 ? AF_INET6 : AF_INET);
			if(!IssueTcpConnectionRead(pTcpConnCtx, 0))
			{
				PreTcpDisconnect(pTcpConnCtx, dwLastError);
			}

			m_Buffers.ReleaseBuffer(lpOperation->pBuffer);
		}
		else if(lpOperation->nOpType == STXIOCP_OPERATION_WRITE)
		{
			lpOperation->pBuffer->SetDataLength(dwNumReadWrite);

			OnTcpSentWrapper(lpContextKey->pTcpConnectionContext, lpOperation->pBuffer);
			m_Buffers.ReleaseBuffer(lpOperation->pBuffer);
		}
	}
}

int CSTXIOCPServer::CreateWorkerThreads()
{
	DWORD dwThreads = OnQueryWorkerThreadCount();

	UINT uThreadID;
	for(DWORD i=0; i < dwThreads; i++)
	{
		HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, WorkThreadProc, this, 0, &uThreadID);
		if(hThread)
		{
			m_arrWorkerThreads.push_back(hThread);
			m_setWorkerThreadId.insert(uThreadID);
		}
	}

	return (int)m_arrWorkerThreads.size();
}

void CSTXIOCPServer::WTProcessDirMonitor(BOOL bResult, DWORD dwLastError, DWORD dwNumReadWrite, LPSTXIOCPCONTEXTKEY lpContextKey, LPSTXIOCPOPERATION lpOperation)
{
	static DWORD_PTR dwOperationUniqueIdBase = 0;

	if(lpOperation->lpCompletionRoutine)
		lpOperation->lpCompletionRoutine(dwLastError, dwNumReadWrite, &lpOperation->ov);

	if(dwNumReadWrite == 0)
	{
		/*
		Upon successful synchronous completion, the lpBuffer parameter is a formatted buffer
		and the number of bytes written to the buffer is available in lpBytesReturned. 
		If the number of bytes transferred is zero, the buffer was either too large for the system to allocate
		or too small to provide detailed information on all the changes that occurred in the directory or subtree.
		In this case, you should compute the changes by enumerating the directory or subtree.
		*/

		if ((m_BaseServerInfo.dwStatus & STXSS_PRETERMINATE) == 0)
		{
			STXTRACELOGE(_T("[r][i]ReadDirectoryChangesW returns lpBytesReturned == 0. Buffer too large or to small. current buffer size is %d bytes."), lpOperation->nMonitorBufferLength);
		}
		else
		{
			EnterCriticalSection(&m_csMonitoredDirSet);
			m_setMonitoredDir.erase(lpContextKey->hMonitoredDir);
			LeaveCriticalSection(&m_csMonitoredDirSet);
			m_setMonitoredDirToRemove.insert(lpContextKey);
			m_mapMonitoredDir.erase(lpContextKey->hMonitoredDir);
			m_mapIDtoMonitoredDir.erase(lpContextKey->pFolderMonitorContext->m_nMonitorID);
			//delete lpContextKey;
			return;
		}
	}
	else
	{
		FILE_NOTIFY_INFORMATION *pChangeRecord = (FILE_NOTIFY_INFORMATION*)lpOperation->pMonitorBuffer;

		PostCustomOperationUsingSprcifiedOperationType(STXIOCP_OPERATION_FILE_CHANGE_NOTIFY, dwOperationUniqueIdBase++, (DWORD_PTR)lpOperation->pMonitorBuffer, (DWORD_PTR)lpContextKey, 10);
		lpOperation->pMonitorBuffer = NULL;		//Prevent this buffer from being deleted. It is used in PostCustomOperationUsingSprcifiedOperationType and will be deleted later

		//while (pChangeRecord)
		//{
		//	OnFolderChanged(lpContextKey->szMonitoredFolder, pChangeRecord);

		//	std::wstring filename(pChangeRecord->FileName, pChangeRecord->FileNameLength / sizeof(TCHAR));
		//	std::wstring fullFilename = GetServerModuleFilePath() + filename;

		//	OnFileChanged(pChangeRecord->Action, filename.c_str(), fullFilename.c_str());

		//	if (pChangeRecord->NextEntryOffset != 0)
		//	{
		//		pChangeRecord = (PFILE_NOTIFY_INFORMATION)((LPBYTE)pChangeRecord + pChangeRecord->NextEntryOffset);
		//		pChangeRecordCopy = (PFILE_NOTIFY_INFORMATION)((LPBYTE)pChangeRecordCopy + pChangeRecordCopy->NextEntryOffset);
		//	}
		//	else
		//	{
		//		pChangeRecord = NULL;
		//	}
		//}
	}

	if (lpContextKey->hMonitoredDir == INVALID_HANDLE_VALUE)
	{
		m_mapMonitoredDir.erase(lpOperation->hMonitoredDir);
		m_mapIDtoMonitoredDir.erase(lpContextKey->pFolderMonitorContext->m_nMonitorID);
		m_setMonitoredDirToRemove.insert(lpContextKey);
		//delete lpContextKey;
		return;
	}

	if (m_BaseServerInfo.dwStatus & STXSS_TERMINATE)		//Server is about to terminate
	{
		m_mapMonitoredDir.erase(lpContextKey->hMonitoredDir);
		m_mapIDtoMonitoredDir.erase(lpContextKey->pFolderMonitorContext->m_nMonitorID);
		m_setMonitoredDirToRemove.insert(lpContextKey);
		//delete lpContextKey;
		return;
	}

	DWORD dwFolderMonitorBufferSize = STXIOCP_FOLDER_MONITOR_BUFFER_SIZE;
	if (m_BaseServerInfo.dwFolderMonitorBufferSize > 1024)
	{
		dwFolderMonitorBufferSize = m_BaseServerInfo.dwFolderMonitorBufferSize;
	}

	// Issue another monitor operation
	NEW_OPERATION(pOperation);

	pOperation->pBuffer = NULL;
	pOperation->nOpType = STXIOCP_OPERATION_MONITOR;
	pOperation->hMonitoredDir = lpOperation->hMonitoredDir;
	pOperation->pMonitorBuffer = new char[dwFolderMonitorBufferSize];
	pOperation->nMonitorBufferLength = dwFolderMonitorBufferSize;
	pOperation->lpCompletionRoutine = lpOperation->lpCompletionRoutine;
	pOperation->sock = INVALID_SOCKET;
	pOperation->dwSubmitTime = GetTickCount();
	pOperation->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_MONITORED_DIR;
	pOperation->dwTimeOut = 0;
	pOperation->dwOperationID = 0;
	pOperation->dwUserData = 0;
	pOperation->dwCompleteType = 0;


	if (lpContextKey->hMonitoredDir == INVALID_HANDLE_VALUE)		//Check again. This check is needed due to parallel execution of different threads
	{
		delete pOperation;
		m_mapMonitoredDir.erase(lpOperation->hMonitoredDir);
		m_mapIDtoMonitoredDir.erase(lpContextKey->pFolderMonitorContext->m_nMonitorID);
		m_setMonitoredDirToRemove.insert(lpContextKey);
		return;
	}

	EnqueueOperation(pOperation);

	BOOL bMonitored = ReadDirectoryChangesW(pOperation->hMonitoredDir, pOperation->pMonitorBuffer, pOperation->nMonitorBufferLength, TRUE,
		lpContextKey->dwMonitorNotifyFilter, NULL, &pOperation->ov, NULL);

	int iLastError = GetLastError();

	if (!bMonitored)
	{
		TCHAR szError[256];
		CSTXLog::GetLastErrorText(szError, 256);

		DequeueOperation(pOperation);

		if (iLastError != ERROR_INVALID_HANDLE)
			CloseHandle(pOperation->hMonitoredDir);

		if ((m_BaseServerInfo.dwStatus & (STXSS_TERMINATE | STXSS_PRETERMINATE)) == 0)
		{
			STXTRACELOGE(_T("[r][g][i]ReadDirectoryChangesW Failed : %s"), szError);
		}

		EnterCriticalSection(&m_csMonitoredDirSet);
		m_setMonitoredDir.erase(pOperation->hMonitoredDir);
		LeaveCriticalSection(&m_csMonitoredDirSet);

		pOperation->Release();		//pOperation->pMonitorBuffer is deleted in destructor

		m_mapMonitoredDir.erase(lpContextKey->hMonitoredDir);
		m_mapIDtoMonitoredDir.erase(lpContextKey->pFolderMonitorContext->m_nMonitorID);
		m_setMonitoredDirToRemove.insert(lpContextKey);
		return;
	}
	
}

void CSTXIOCPServer::WTProcessFileRead(BOOL bResult, DWORD dwLastError, DWORD dwNumReadWrite, LPSTXIOCPCONTEXTKEY lpContextKey, LPSTXIOCPOPERATION lpOperation)
{
	if(lpOperation->lpCompletionRoutine)
		lpOperation->lpCompletionRoutine(dwLastError, dwNumReadWrite, &lpOperation->ov);

	if(dwNumReadWrite == 0)
	{
		if(dwLastError == ERROR_HANDLE_EOF)
		{
			if(lpContextKey->dwMonitorNotifyFilter)
				OnInternalFileRead(lpContextKey->pClientContext, NULL, lpContextKey->dwFileTag, lpContextKey->nBytesRead, lpContextKey->dwCookie, lpContextKey->dwUserData);
			else
				OnFileRead(lpContextKey->pClientContext, NULL, lpContextKey->dwFileTag);

			//STXTRACEE(_T("[%s] End of file, Terminate read."), lpContextKey->szMonitoredFolder, dwNumReadWrite);
		}
		else
		{
			if(lpContextKey->dwMonitorNotifyFilter)
				OnInternalFileRead(lpContextKey->pClientContext, NULL, lpContextKey->dwFileTag, lpContextKey->nBytesRead, lpContextKey->dwCookie, lpContextKey->dwUserData);
			else
				OnFileRead(lpContextKey->pClientContext, NULL, lpContextKey->dwFileTag);

			//STXTRACEE(_T("[%s] Unhandled Error, Terminate read."), lpContextKey->szMonitoredFolder, dwNumReadWrite);
		}

		CloseHandle(lpContextKey->hFile);
		delete lpContextKey;
	}
	else
	{

		lpOperation->pBuffer->SetDataLength(dwNumReadWrite);
		if(lpContextKey->pClientContext)
		{
			if(lpContextKey->dwMonitorNotifyFilter)
				OnInternalFileRead(lpContextKey->pClientContext, lpOperation->pBuffer, lpContextKey->dwFileTag, lpContextKey->nBytesRead, lpContextKey->dwCookie, lpContextKey->dwUserData);
			else
				OnFileRead(lpContextKey->pClientContext, lpOperation->pBuffer, lpContextKey->dwFileTag);
		}

		lpContextKey->nBytesRead += dwNumReadWrite;
		//STXTRACEE(_T("[%s] %I64d bytes read."), lpContextKey->szMonitoredFolder, lpContextKey->nBytesRead);

		m_Buffers.ReleaseBuffer(lpOperation->pBuffer);
		CSTXIOCPBuffer *pNewBuffer = m_Buffers.GetBuffer(20);
		if(pNewBuffer == NULL)
		{
			if(lpContextKey->dwMonitorNotifyFilter)
				OnInternalFileRead(lpContextKey->pClientContext, NULL, lpContextKey->dwFileTag, lpContextKey->nBytesRead, lpContextKey->dwCookie, lpContextKey->dwUserData);
			else
				OnFileRead(lpContextKey->pClientContext, NULL, lpContextKey->dwFileTag);

			CloseHandle(lpContextKey->hFile);
			delete lpContextKey;
			return;
		}

		NEW_OPERATION(pOperation);

		LARGE_INTEGER v;
		v.QuadPart = lpContextKey->nBytesRead;
		pOperation->ov.Offset = v.LowPart;
		pOperation->ov.OffsetHigh = v.HighPart;
		pOperation->pBuffer = pNewBuffer;
		pOperation->nOpType = STXIOCP_OPERATION_READ;
		pOperation->sock = INVALID_SOCKET;
		pOperation->dwSubmitTime = GetTickCount();
		pOperation->dwTimeOut = 0;
		pOperation->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_FILE;
		EnqueueOperation(pOperation);

		BOOL bResult = FALSE;
		DWORD dwNumRead = 0;
		bResult = ReadFile(lpContextKey->hFile, pOperation->pBuffer->GetBufferPtr(), pOperation->pBuffer->GetBufferLength(), &dwNumRead, &pOperation->ov);
		int iError = GetLastError();

		if(bResult)	//Succeed
		{
			// 完成端口工作线程会在稍后获得这个操作已经完成的通知。
			return;
		}
		else
		{
			if(iError == ERROR_IO_PENDING)		//IO操作已经投递成功
			{
				return;
			}

			if(lpContextKey->dwMonitorNotifyFilter)
				OnInternalFileRead(lpContextKey->pClientContext, NULL, lpContextKey->dwFileTag, lpContextKey->nBytesRead, lpContextKey->dwCookie, lpContextKey->dwUserData);
			else
				OnFileRead(lpContextKey->pClientContext, NULL, lpContextKey->dwFileTag);

			m_Buffers.ReleaseBuffer(pOperation->pBuffer);
			pOperation->MarkCanceled();
			DequeueOperation(pOperation);
			pOperation->Release();
			delete lpContextKey;
		}
	}
}

void CSTXIOCPServer::WTProcessHttpOperation(BOOL bResult, DWORD dwLastError, DWORD dwNumReadWrite, LPSTXIOCPCONTEXTKEY lpContextKey, LPSTXIOCPOPERATION lpOperation)
{
	LPSTXIOCPSERVERHTTPCONTEXT pContext = (LPSTXIOCPSERVERHTTPCONTEXT)lpOperation->dwUserData;
	switch(lpOperation->dwOperationID)
	{
	case HTTP_OPERATION_REQUEST:
		{
			// Open the request
			HINTERNET hRequest = HttpOpenRequest(pContext->hConnect, 
				_T("GET"), 
				pContext->szUrl,
				NULL,
				NULL,
				NULL,
				INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE,
				(DWORD_PTR)pContext);  // Request handle's context 
			if (hRequest == NULL)
			{
				if (GetLastError() != ERROR_IO_PENDING)
				{
					STXTRACELOGE(_T("Error: HttpOpenRequest failed."));
					delete pContext;
					return;
				}
			}
			else
			{
				pContext->hRequest = hRequest;
			}
			break;
		}
	case HTTP_OPERATION_REQUEST_SEND:
		{
			if (!HttpSendRequest(pContext->hRequest, 
				NULL, 
				0, 
				NULL,
				0))
			{
				if (GetLastError() != ERROR_IO_PENDING)
				{
					STXTRACELOGE(_T("Error: HttpSendRequest failed."));
					delete pContext;
					return;
				}
			}
			break;
		}
	case HTTP_OPERATION_REQUEST_READ:
		{
			pContext->buff.dwStructSize = sizeof(pContext->buff);
			pContext->buff.dwBufferLength = HTTP_DOWNLOAD_BUFFER_SIZE;

			if (!InternetReadFileEx(pContext->hRequest, &pContext->buff, 0, (DWORD_PTR)pContext))
			{
				if (GetLastError() != ERROR_IO_PENDING)
				{
					STXTRACELOGE(_T("Error: InternetReadFileEx failed."));
					OnUrlDownloadProgressWrapper(pContext, -1);
					OnUrlDownloadCleanupWrapper(pContext);
					delete pContext;
				}
			}
			else
			{
				pContext->bufferContent.WriteBuffer(pContext->buff.lpvBuffer, pContext->buff.dwBufferLength);
				OnUrlDownloadProgressWrapper(pContext, pContext->buff.dwBufferLength);

				if(pContext->buff.dwBufferLength != 0)
				{
					PostHttpRequestOperation(pContext, HTTP_OPERATION_REQUEST_READ);
				}
				else
				{
					PostHttpRequestOperation(pContext, HTTP_OPERATION_REQUEST_FINISHED);
				}
			}
			break;
		}
	case HTTP_OPERATION_REQUEST_FINISHED:
		{
			//OnUrlDownloadProgressWrapper(pContext, 0);
			STXTRACELOGE(_T("URL Download Finished >> %s >> %d bytes"), pContext->szUrl, pContext->bufferContent.GetDataLength());

			OnUrlDownloadCompleteWrapper(pContext);
			OnUrlDownloadCleanupWrapper(pContext);
			delete pContext;
			break;
		}
	}
}

void CSTXIOCPServer::KillAllClient()
{
	// 关闭所有的客户 Socket
	STXTRACELOGE(_T("Killing All Clients..."));

	m_mapClientContext.foreach([&](std::pair<std::wstring, CSTXServerObjectPtr<CSTXIOCPServerClientContext> > item)
		{
			//shutdown(item.second->GetSocketOriginal(), 2);
			//item.second->CloseSocket();
			DisconnectClient(item.second);
	});

}

void CSTXIOCPServer::ClearAllClient()
{
	// 清除所有客户对象
	m_mapClientContext.clear();
}

void CSTXIOCPServer::ClearAllFolderMonitor()
{
	m_setMonitoredDirToRemove.foreach([](LPSTXIOCPCONTEXTKEY pContextKey)
	{
		delete pContextKey;
	});
}

void CSTXIOCPServer::ClearAllServers()
{
	LockServersMap();
	m_mapTcpServers.erase(m_mapTcpServers.begin(), m_mapTcpServers.end());
	m_mapUdpServers.erase(m_mapUdpServers.begin(), m_mapUdpServers.end());
	UnlockServersMap();
}

void CSTXIOCPServer::KillAllConnections()
{
	EnterCriticalSection(&m_csKillAllConnectionsLock);
	STXTRACELOGE(_T("Killing All Connections..."));
	LockConnectionMap();
	map<LONG, CSTXServerObjectPtr<CSTXIOCPTcpConnectionContext> >::iterator it = m_mapConnections.begin();
	for(; it!= m_mapConnections.end(); it++)
	{
		closesocket(it->second->GetSocket());
		it->second->ModifyFlags(0, STXIOCP_SERVER_CONNECTION_FLAG_KEEPCONNECT);
	}
	UnlockConnectionMap();

	LeaveCriticalSection(&m_csKillAllConnectionsLock);
}

BOOL CSTXIOCPServer::CreateSocket(SOCKET *pSocket4, SOCKET *pSocket6, int nSocketType, LPCTSTR lpszPort)
{
	if(pSocket4)	*pSocket4 = INVALID_SOCKET;
	if(pSocket6)	*pSocket6 = INVALID_SOCKET;

	ADDRINFO hints;
	LPADDRINFO pRes = NULL, pPtr = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = nSocketType;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;

	int iResult = 0;
	LPSTR lpszAscii = NULL;
#ifdef UNICODE
	if(lpszPort)
	{
		int cbLen = (int)_tcslen(lpszPort) + 2;
		lpszAscii = (LPSTR)new char[cbLen];
		::WideCharToMultiByte(CP_ACP, 0, lpszPort, -1, lpszAscii, cbLen, NULL, NULL);
		iResult = getaddrinfo(NULL, lpszAscii, &hints, &pRes);
		delete []lpszAscii;
	}
	else
	{
		iResult = getaddrinfo(NULL, "0", &hints, &pRes);
	}
#else
	lpszAscii = (lpszPort == NULL ? _T("0") : lpszPort);
	iResult = getaddrinfo(NULL, lpszAscii, &hints, &pRes);
#endif

	if (iResult != 0)
	{
		if (pRes)
			freeaddrinfo(pRes);
		return FALSE;
	}

	int nBindResult = 0;
	int nBindResult6 = 0;
	pPtr = pRes;
	while(pPtr)
	{
		if(pPtr->ai_family == AF_INET && pSocket4 && *pSocket4 == INVALID_SOCKET)
		{
			*pSocket4 = WSASocket(pPtr->ai_family, pPtr->ai_socktype, 0, 0, 0, WSA_FLAG_OVERLAPPED);
			if(lpszPort)
			{
				nBindResult = ::bind(*pSocket4, pPtr->ai_addr, (int)pPtr->ai_addrlen);
				
				TCHAR szError[256];
				CSTXLog::GetLastErrorText(szError, 256);

				TCHAR szAddress[256] = _T("Unknown IP");
				DWORD dwLen = sizeof(szAddress) / sizeof(TCHAR);
				WSAAddressToString((SOCKADDR*)pPtr->ai_addr, sizeof(SOCKADDR_IN6), NULL, szAddress, &dwLen);

				//char *pIPv4_String = inet_ntoa(((SOCKADDR_IN*)pPtr->ai_addr)->sin_addr);
				if(SOCKET_ERROR == nBindResult)
				{
					STXTRACELOGE(_T("[r][i]Error! In CreateSocket, bind() Failed! Please be sure the port %s at %S is not in use..."), lpszPort, szAddress);
					STXTRACELOGE(_T("[r][i]\t %s"), szError);
				}
				else
				{
					STXTRACELOGE(_T("[g] New socket created and bind to port %s at %S ..."), lpszPort, szAddress);
				}
			}
		}
		else if(pPtr->ai_family == AF_INET6 && pSocket6 && *pSocket6 == INVALID_SOCKET && (m_BaseServerInfo.dwServerFlags & STXSF_IPV6))
		{
			*pSocket6 = WSASocket(pPtr->ai_family, pPtr->ai_socktype, 0, 0, 0, WSA_FLAG_OVERLAPPED);
			if(*pSocket6 == INVALID_SOCKET)
			{
				STXTRACELOGE(_T("[r][i]Error! In CreateSocket, Socket Not Created for IPv6! Please make sure the IPv6 is installed on this computer."));
			}
			else if(lpszPort)
			{
				nBindResult6 = ::bind(*pSocket6, pPtr->ai_addr, (int)pPtr->ai_addrlen);

				TCHAR szError[256];
				CSTXLog::GetLastErrorText(szError, 256);

				TCHAR szAddress[256] = _T("Unknown IP");

				DWORD dwLen = sizeof(szAddress) / sizeof(TCHAR);
				WSAAddressToString((SOCKADDR*)pPtr->ai_addr, sizeof(SOCKADDR_IN6), NULL, szAddress, &dwLen);

				if(SOCKET_ERROR == nBindResult6)
				{
					STXTRACELOGE(_T("[r][i]Error! In CreateSocket, bind() Failed! Please be sure %s is not in use..."), szAddress);
					STXTRACELOGE(_T("[r][i]\t %s"), szError);
				}
				else
				{
					STXTRACELOGE(_T("[g] New socket created and bind to %s ..."), szAddress);
				}
			}
		}
		pPtr = pPtr->ai_next;
	}

	if((m_BaseServerInfo.dwServerFlags & STXSF_IPV6) == 0)
	{
		if (pRes)
			freeaddrinfo(pRes);

		if(nBindResult != 0 ||(pSocket4 && *pSocket4 == INVALID_SOCKET))	//Failed to create the socket
		{
			if(pSocket4 && *pSocket4 != INVALID_SOCKET)	closesocket(*pSocket4);

			return FALSE;
		}
	}
	else
	{
		if(pSocket6 && *pSocket6 == INVALID_SOCKET)
		{
			STXTRACELOGE(_T("[r][i]Error! In CreateSocket, Socket Not Created for IPv6! Please make sure the IPv6 is installed on this computer."));
		}

		if (pRes)
			freeaddrinfo(pRes);

		if(nBindResult != 0 || nBindResult6 != 0 ||
			(pSocket4 && *pSocket4 == INVALID_SOCKET) || (pSocket6 && *pSocket6 == INVALID_SOCKET))	//Failed to create the socket
		{
			if(pSocket4 && *pSocket4 != INVALID_SOCKET)	closesocket(*pSocket4);
			if(pSocket6 && *pSocket6 != INVALID_SOCKET)	closesocket(*pSocket6);
			return FALSE;
		}
	}

	return TRUE;
}

BOOL CSTXIOCPServer::GetSocketExtensionFunctions(SOCKET sock)
{
	if (_lpfnDisconnectEx == NULL)
	{
		GUID GuidDisconnectEx = WSAID_DISCONNECTEX;
		DWORD dwBytes = 0;

		WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidDisconnectEx,
			sizeof(GuidDisconnectEx), &_lpfnDisconnectEx, sizeof(_lpfnDisconnectEx), &dwBytes, NULL, NULL);

		if (_lpfnDisconnectEx == NULL)
		{
			STXTRACELOGE(_T("Error! Can not obtain DisconnectEx function pointer in CSTXIOCPServerClientContext::GetSocketExtensionFunctions() !"));
		}
	}

	if (_lpfnConnectEx == NULL)
	{
		GUID GuidConnectEx = WSAID_CONNECTEX;
		DWORD dwBytes = 0;

		WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidConnectEx,
			sizeof(GuidConnectEx), &_lpfnConnectEx, sizeof(_lpfnConnectEx), &dwBytes, NULL, NULL);

		if (_lpfnConnectEx == NULL)
		{
			STXTRACELOGE(_T("Error! Can not obtain ConnectEx function pointer in CSTXIOCPServerClientContext::GetSocketExtensionFunctions() !"));
		}
	}

	return _lpfnDisconnectEx && _lpfnConnectEx;
}

BOOL CSTXIOCPServer::BeginTcpServer(UINT uPort, DWORD_PTR dwServerParam, LPCTSTR lpszServerParamString, UINT nPostAcceptCount, LONG64 nLimitClientCount)
{
	// 1. 检查在指定的 TCP 端口上是否已经创建了 TCP Server
	// 2. 创建 socket
	// 3. 创建 CSTXIOCPTcpServerContext 对象与 socket 关联
	// 4. 创建 STXIOCPCONTEXTKEY 对象用于完成端口
	// 5. 将 socket 与完成端口关联起来
	// 6. 为 socket 执行绑定(bind)操作
	// 7. 为 socket 执行 listen 操作
	// 8. 创建一个 SERVEROBJECTINFO 对象用于保存此 TCP Server 的信息，并把该对象记录到 TCP Server map 中
	// 9. 为这个 TCP Server 投递若干个 Accept 操作

	//Step 1
	LockServersMap();
	if(m_mapTcpServers.find(uPort) != m_mapTcpServers.end())		//There is already a TCP listener socket listening at the given port
	{
		UnlockServersMap();
		SetLastError(ERROR_ALREADY_EXISTS);
		return FALSE;
	}
	UnlockServersMap();

	//tr1::shared_ptr<STXIOCPCONTEXTKEY> pContextKey(new STXIOCPCONTEXTKEY);
	LPSTXIOCPCONTEXTKEY pContextKey = new STXIOCPCONTEXTKEY;
	memset(pContextKey, 0, sizeof(STXIOCPCONTEXTKEY));
	//Step 2.1
	TCHAR szPort[16];
	_ltot_s(uPort, szPort, 16, 10);

	SOCKET sock, sock6;

	if(!CreateSocket(&sock, &sock6, SOCK_STREAM, szPort))
	{
		STXTRACELOGE(_T("[r][i]Error! In BeginTcpServer, CreateSocket() Failed!..."));
		delete pContextKey;
		return FALSE;
	}

	GetSocketExtensionFunctions(sock);

	pContextKey->sock = sock;
	pContextKey->sock6 = sock6;

	//Step 3
	auto newServerContext = OnCreateServerContext();
	tr1::shared_ptr<CSTXIOCPTcpServerContext> pServerContext(dynamic_cast<CSTXIOCPTcpServerContext*>(newServerContext));
	pServerContext->SetMaximumClientCount(nLimitClientCount);
	pServerContext->SetServerParam(dwServerParam);
	pServerContext->SetServerParamString(lpszServerParamString);
	pServerContext->SetSocket(sock, sock6);
	pServerContext->SetServer(this);
	pServerContext->SetListeningPort(uPort);

	//Step 4
	_tcscpy_s(pContextKey->szDescription, 32, _T("[Accept Socket]"));
	pContextKey->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_TCPSERVER;
	pContextKey->pTcpServerContext = pServerContext;
	pServerContext->SetContextKey(pContextKey);


	//Step 5
	HANDLE hIOCP = NULL;
	hIOCP = ::CreateIoCompletionPort((HANDLE)sock, m_hIOCP, (ULONG_PTR)pContextKey, 0);
	if(hIOCP == NULL)
	{
		STXTRACELOGE(_T("CreateIoCompletionPort for TCP Server socket failed! (IPv4)"));
		delete pContextKey;

		closesocket(sock);
		if(m_BaseServerInfo.dwServerFlags & STXSF_IPV6)
		{
			closesocket(sock6);
		}
		return FALSE;
	}

	if(m_BaseServerInfo.dwServerFlags & STXSF_IPV6)
	{
		hIOCP = ::CreateIoCompletionPort((HANDLE)sock6, m_hIOCP, (ULONG_PTR)pContextKey, 0);
		if(hIOCP == NULL)
		{
			STXTRACELOGE(_T("CreateIoCompletionPort for TCP Server socket failed! (IPv6)"));
			delete pContextKey;
			closesocket(sock);
			if(m_BaseServerInfo.dwServerFlags & STXSF_IPV6)
			{
				closesocket(sock6);
			}
			return FALSE;
		}
	}

	//Step 6 (done at step 2.1)

	//Step 7
	listen(sock, 200);
	
	if(m_BaseServerInfo.dwServerFlags & STXSF_IPV6)
		listen(sock6, 200);

	//Step 8
	LockServersMap();
	m_mapTcpServers[uPort] = pServerContext;
	UnlockServersMap();

	//Step 9 - Issue some Accept operation
	//根据实际需要，在此处投递多个 Accept 以提高连接被接受的可能性。(最大化连接)
	if (nPostAcceptCount < 1 || nPostAcceptCount >= 0x7FFFFFFF)
		nPostAcceptCount = m_BaseServerInfo.dwAcceptPost;

	for(DWORD i=0;i<nPostAcceptCount;i++)
		IssueAccept(pContextKey);

	TCHAR szLimit[64] = {0};
	if (nLimitClientCount > 0)
	{
		_stprintf_s(szLimit, _T(" , Max Clients: %I64d"), nLimitClientCount);
	}

	if (lpszServerParamString)
	{
		STXTRACELOGE(_T("TCP Sub Server <%s> Created. Port: %u [TCP],\tAccept Posted: %d%s"), lpszServerParamString, uPort, nPostAcceptCount, szLimit);
	}
	else
	{
		STXTRACELOGE(_T("TCP Sub Server Created. Port: %u [TCP],\tAccept Posted: %d%s"), uPort, nPostAcceptCount, szLimit);
	}

	OnTcpSubServerInitialized(pServerContext.get());
	

	return TRUE;
}

CSTXServerContextBase* CSTXIOCPServer::BeginUdpServer(UINT uPort, DWORD_PTR dwServerParam, LPCTSTR lpszServerParamString)
{
	// 1. 检查在指定的 UDP 端口上是否已经创建了 UDP Server
	// 2. 创建 socket
	// 3. 创建 CSTXIOCPUdpServerContext 对象与 socket 关联
	// 4. 创建 STXIOCPCONTEXTKEY 对象用于完成端口
	// 5. 将 socket 与完成端口关联起来
	// 6. 为 socket 执行绑定(bind)操作
	// 7. 创建一个 SERVEROBJECTINFO 对象用于保存此 UDP Server 的信息，并把该对象记录到 UDP Server map 中
	// 8. 为这个 UDP Server 投递一个 Read 操作

	//Step 1
	LockServersMap();
	if(m_mapUdpServers.find(uPort) != m_mapUdpServers.end())		//There is already a UDP socket bind on the given port
	{
		UnlockServersMap();
		SetLastError(ERROR_ALREADY_EXISTS);
		return NULL;
	}
	UnlockServersMap();

	//Step 2.1 - Create Socket
	TCHAR szPort[32];
	_ltot_s(uPort, szPort, sizeof(szPort) / sizeof(TCHAR), 10);
	SOCKET sockUDP = INVALID_SOCKET, sockUDP6 = INVALID_SOCKET;
	if(!CreateSocket(&sockUDP, &sockUDP6, SOCK_DGRAM, szPort))
	{
		STXTRACELOGE(_T("[r][i]Error! In BeginUdpServer(), CreateSocket() Failed!..."));
		return NULL;
	}

	//Step 3
	CSTXIOCPUdpServerContext *pServerContext = dynamic_cast<CSTXIOCPUdpServerContext*>(OnCreateUdpServerContext(dwServerParam));
	pServerContext->SetSocket(sockUDP, sockUDP6);
	pServerContext->SetServerParam(dwServerParam);
	pServerContext->SetServerParamString(lpszServerParamString);
	pServerContext->SetServer(this);

	//Step 4
	LPSTXIOCPCONTEXTKEY pContextKey = new STXIOCPCONTEXTKEY;
	memset(pContextKey, 0, sizeof(STXIOCPCONTEXTKEY));
	pContextKey->sock = sockUDP;
	pContextKey->sock6 = sockUDP6;
	pContextKey->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_UDPSERVER;
	pContextKey->pUdpServerContext = pServerContext;
	pServerContext->SetContextKey(pContextKey);

	if (sockUDP != INVALID_SOCKET)
		InterlockedIncrement(&pContextKey->nUdpServerPending);
	if (sockUDP6 != INVALID_SOCKET)
		InterlockedIncrement(&pContextKey->nUdpServerPending);

	//Step 5
	HANDLE hIOCP = NULL;
	hIOCP = ::CreateIoCompletionPort((HANDLE)sockUDP, m_hIOCP, (ULONG_PTR)pContextKey, 0);
	if(hIOCP == NULL)
	{
		STXTRACELOGE(_T("[r][i]Error! In BeginUdpServer(), Failed to bind the socket (IPv4) to the IO Completion Port..."));
		delete pContextKey;
		closesocket(sockUDP);
		return NULL;
	}
	if(m_BaseServerInfo.dwServerFlags & STXSF_IPV6)
	{
		hIOCP = ::CreateIoCompletionPort((HANDLE)sockUDP6, m_hIOCP, (ULONG_PTR)pContextKey, 0);
		if(hIOCP == NULL)
		{
			STXTRACELOGE(_T("[r][i]Error! In BeginUdpServer(), Failed to bind the socket (IPv6) to the IO Completion Port..."));
			delete pContextKey;
			closesocket(sockUDP6);
			return NULL;
		}
	}

	//Step 6 (done at step 2.1)

	//Step 7

	LockServersMap();
	m_mapUdpServers[uPort] = pServerContext;
	pServerContext->SetUdpPort(uPort);
	UnlockServersMap();

	OnUdpSubServerInitialized(pServerContext);

	//Step 8
	IssueUdpRead(pServerContext, 0, AF_INET);
	// No need to pServerContext->AddRef here because pServerContext has Ref = 1 already
	
	if(m_BaseServerInfo.dwServerFlags & STXSF_IPV6)
	{
		IssueUdpRead(pServerContext, 0, AF_INET6);
	}

	STXTRACELOGE(_T("UDP Sub Server Created. Binding at port: %u [UDP]"), uPort);

	return pServerContext;
}

BOOL CSTXIOCPServer::OnAccepted(CSTXIOCPServerClientContext *pClientContext)
{
	return TRUE;
}

BOOL CSTXIOCPServer::CanTerminateWorkThreads()
{
	return m_setOperation.size() == 0;

	//return m_setOperation.size() == 0 && m_mapConnections.size() == 0;
}

BOOL CSTXIOCPServer::Terminate()
{
	DWORD dwThreadId = GetCurrentThreadId();
	if (m_setWorkerThreadId.find(dwThreadId) != m_setWorkerThreadId.end(dwThreadId))
	{
		STXTRACELOGE(_T("[r][i] To Terminate the server from within a worker thread is not allowed!"));
		return FALSE;
	}

	if(m_BaseServerInfo.dwStatus & STXSS_TERMINATE)
		return FALSE;

	//LockServer(_T(__FILE__), __LINE__);

	// Change Server Status as pre-terminate 
	m_BaseServerInfo.dwStatus = STXSS_PRETERMINATE;

	CSTXServerBase::Terminate();

	//Cancel all custom operations
	map<DWORD_PTR, LPSTXIOCPOPERATION>::iterator it;
	m_mapCustomOperation.foreach([&](std::pair<DWORD_PTR, LPSTXIOCPOPERATION> item)
		{
			if(item.second->dwCompleteType == STXIOCP_CUSTOM_OPERATION_QUEUED)
			{
				item.second->dwCompleteType = STXIOCP_CUSTOM_OPERATION_CANCELED;
				PostQueuedCompletionStatus(m_hIOCP, 0, NULL, &item.second->ov);
			}
	});


	STXTRACELOGE(_T("To Shutdown Server..."));
	STXTRACELOGE(_T("Current Client Context: \t%d"), m_mapClientContext.size());
	STXTRACELOGE(_T("Current R/W Buffers: \t%d/%d"), m_Buffers.GetBufferAvailableCount(), m_Buffers.GetBufferTotalCount());
	STXTRACELOGE(_T("Current queued Read: \t%d"), m_queueRead.size());
	STXTRACELOGE(_T("Current uncomplete Operation: \t%d"), m_setOperation.size());

	CloseAllMonitoredFolder();

	// 停止整个服务器模块的过程如下:
	// 1. 将所有的活动 socket 关闭，包括TCP/UDP子服务器socket，客户端连接socket 和 TCP连接socket
	// 2. 在执行了第1步之后会有很多未决的 IO 操作,等待所有这些 IO 操作完成，并将 TimeOut 设置在一个较短的时间
	// 3. 设置一个标记，在所有 IO 操作完成之后的第一个 TimeOut 时，中止工作线程。
	// 4. 由于 StartServer() 一直在等待所有的工作线程中止，当它们确实中止的时候，StartServer() 返回，服务器模块结束工作。

	// Close all server socket, including TCP Listening Socket and UDP socket
	// (Cancel all operations pended on these socket)
	KillAllServers();

	KillAllClient();

	KillAllConnections();

	// Change Server Status for terminate Work-Threads 
	m_BaseServerInfo.dwStatus |= STXSS_TERMINATE;

	// Make IO Time-Out as short as possible
	// (Work-Thread Terminate check is done in IDLE loop)
	m_dwIOWait = 200;

	STXTRACELOGE(_T("WaitForServerUnlock..."));
	WaitForServerUnlock();
	//STXTRACELOGE(_T("ServerUnlocked..."));

	return TRUE;
}

// Close all server socket, including TCP Listening Socket and UDP socket
BOOL CSTXIOCPServer::KillAllServers()
 {
	STXTRACELOGE(_T("Killing All Servers..."));
	LockServersMap();

	// Close all TCP Listening socket
	map<UINT, tr1::shared_ptr<CSTXIOCPTcpServerContext> >::iterator it = m_mapTcpServers.begin();
	for(;it!=m_mapTcpServers.end();it++)
	{
		tr1::shared_ptr<CSTXIOCPTcpServerContext> pServerContext = it->second;
		pServerContext->MarkClosed();
		closesocket(pServerContext->GetSocket());

		if(m_BaseServerInfo.dwServerFlags & STXSF_IPV6)
		{
			closesocket(pServerContext->GetSocket6());
		}
	}

	// Close all UDP sockets
	map<UINT, CSTXServerObjectPtr<CSTXIOCPUdpServerContext> >::iterator it2 = m_mapUdpServers.begin();
	for(;it2!=m_mapUdpServers.end();it2++)
	{
		CSTXIOCPUdpServerContext *pServerContext = it2->second;
		
		//此处不能直接调用 it->second->CloseSocket() , 因为CloseSocket会设置内部变量，会让服务器被误判清空
		closesocket(pServerContext->GetSocket());
		pServerContext->MarkSocketClosed();
		if(m_BaseServerInfo.dwServerFlags & STXSF_IPV6)
		{
			closesocket(it2->second->GetSocket6());
			pServerContext->MarkSocket6Closed();
		}
	}

	UnlockServersMap();

	return TRUE;
}

void CSTXIOCPServer::CloseAllMonitoredFolder()
{
	EnterCriticalSection(&m_csMonitoredDirSet);

	vector<HANDLE> arrHandles;
	copy(m_setMonitoredDir.begin(), m_setMonitoredDir.end(), std::insert_iterator<vector<HANDLE> >(arrHandles, arrHandles.end()));

	vector<HANDLE>::iterator it = arrHandles.begin();
	for(;it != arrHandles.end(); it++)
	{
		auto pContextKey = m_mapMonitoredDir[*it];
		pContextKey->hMonitoredDir = INVALID_HANDLE_VALUE;
		CancelIo(*it);
		CloseHandle(*it);
	}
	LeaveCriticalSection(&m_csMonitoredDirSet);
}

BOOL CSTXIOCPServer::KillTcpServer(UINT uPort)
{
	LockServersMap();

	map<UINT, tr1::shared_ptr<CSTXIOCPTcpServerContext> >::iterator it = m_mapTcpServers.find(uPort);
	if(it == m_mapTcpServers.end())
	{
		UnlockServersMap();
		STXTRACELOGE(_T("No tcp server listening on port %d ... Failed to kill TcpServer !"), uPort);
		return FALSE;
	}

	tr1::shared_ptr<CSTXIOCPTcpServerContext> pServerContext = it->second;

	pServerContext->MarkClosed();

	closesocket(pServerContext->GetSocket());
	if(m_BaseServerInfo.dwServerFlags & STXSF_IPV6)
		closesocket(pServerContext->GetSocket6());

	m_mapTcpServers.erase(it);

	UnlockServersMap();

	OnTcpSubServerDestroyed(pServerContext.get());

	return TRUE;
}

BOOL CSTXIOCPServer::KillUdpServer(UINT uPort)
{
	LockServersMap();
	map<UINT, CSTXServerObjectPtr<CSTXIOCPUdpServerContext> >::iterator it = m_mapUdpServers.find(uPort);
	if(it == m_mapUdpServers.end())
	{
		UnlockServersMap();
		STXTRACELOGE(_T("No UDP server on port %d ... Failed to kill UdpServer!"), uPort);
		return FALSE;
	}

	auto pServerContext = it->second;
	//此处不能直接调用 it->second->CloseSocket() , 因为CloseSocket会设置内部变量，会让服务器被误判清空
	closesocket(it->second->GetSocket());
	if(m_BaseServerInfo.dwServerFlags & STXSF_IPV6)
		closesocket(it->second->GetSocket6());

	m_mapUdpServers.erase(it);

	UnlockServersMap();

	OnUdpSubServerDestroyed(pServerContext);

	return TRUE;
}

int CSTXIOCPServer::GetRunningServerCount()
{
	return (int)(m_mapTcpServers.size() + m_mapUdpServers.size());
}

BOOL CSTXIOCPServer::Initialize(HMODULE hModuleHandle, LPSTXSERVERINIT lpInit)
{
	if(!CSTXServerBase::Initialize(hModuleHandle, lpInit))
		return FALSE;

	return TRUE;
}

void CSTXIOCPServer::StartServer()
{
	ResetEvent(m_hServerUnlockEvent);

	STXTRACELOGE(_T("[g][i]TCP Subserver: \t%d"), m_mapTcpServers.size());
	STXTRACELOGE(_T("[g][i]UDP Subserver: \t%d"), m_mapUdpServers.size());
	STXTRACELOGE(_T("[g][i]Out-going Connection: \t%d"), m_mapConnections.size());

	if(m_BaseServerInfo.dwDefaultOperationTimeout != 0)
	{
		TCHAR szTimeout[MAX_PATH];
		StrFromTimeInterval(szTimeout, MAX_PATH, m_BaseServerInfo.dwDefaultOperationTimeout, 3);
		STXTRACELOGE(_T("Default Operation TimeOut: \t%s"), szTimeout);
	}
	else
		STXTRACELOGE(_T("Default Operation TimeOut: \t[Never]"));

	Run();
}

BOOL CSTXIOCPServer::IssueClientRead(CSTXIOCPServerClientContext *pClientContext, DWORD dwTimeOut)
{
	// 1. 获得一个缓冲区用来执行读操作
	// 2. 准备 WSARecv() 所需要的一些参数
	// 3. 创建一个 STXIOCPOPERATION 对象并将其放入操作集合(set)中
	// 4. 调用 WSARecv 执行读取操作

	if (pClientContext->IsDisconnected())
	{
		return FALSE;
	}

	//Step 1
	CSTXIOCPBuffer *pBuffer = pClientContext->m_pOperationBufferRead;

	if (pBuffer == NULL)
	{
		pBuffer = m_Buffers.GetBuffer(20);
		pClientContext->m_pOperationBufferRead = pBuffer;
	}

	if(pBuffer == NULL)
	{
		STXTRACE(_T("----> Pending Reading... (Not implement)"));
		//PendingRead(sock, nSocketType, dwExtraData, dwExtraData2, dwTimeOut);
		return TRUE;
	}

	//Step 2
	WSABUF buf;
	buf.buf = (char*)pBuffer->GetBufferPtr();
	buf.len = pBuffer->GetBufferLength();

	DWORD dwNumRead = 0;
	DWORD dwFlags = MSG_PARTIAL;	//根据 MSDN, 此参数目前对于任何协议都没有作用

	//Step 3
	NEW_OPERATION(pOperation);

	pOperation->pBuffer = pBuffer;
	pOperation->nOpType = STXIOCP_OPERATION_READ;
	pOperation->sock = pClientContext->GetSocketOriginal();
	pOperation->dwSubmitTime = GetTickCount();
	pOperation->dwTimeOut = dwTimeOut;
	pOperation->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_CLIENT;
	pOperation->pClientContext = pClientContext;
	pOperation->nIPVer = (pClientContext->m_nAddressFamily == AF_INET6 ? STXIOCP_SOCKET_IPV6 : STXIOCP_SOCKET_IPV4);
	EnqueueOperation(pOperation);

	//Step 4
	int iResult = 0;
	iResult = WSARecv(pClientContext->GetSocketOriginal(), &buf, 1, &dwNumRead, &dwFlags, &pOperation->ov, NULL);
	int iWsaError = WSAGetLastError();

	if(iResult == 0 && iWsaError == 0)	//Succeed Immediately
	{
		// 即使该操作立即完成，也不必在此进行处理。
		// 完成端口工作线程会在稍后获得这个操作已经完成的通知。

		return TRUE;
	}
	else
	{
		if(iWsaError == ERROR_IO_PENDING)		//IO操作已经投递成功
		{
			return TRUE;
		}
		else
		{
			TCHAR szError[MAX_PATH];
			CSTXLog::GetErrorText(szError, MAX_PATH, iWsaError);
			STXTRACE(_T("IssueClientRead Error: %s"), szError);
		}
		pOperation->MarkCanceled();
		DequeueOperation(pOperation);
		pOperation->Release();
	}

	return FALSE;
}

BOOL CSTXIOCPServer::IssueTcpConnectionRead(CSTXIOCPTcpConnectionContext *pTcpConnectionContext, DWORD dwTimeOut)
{
	// 1. 获得一个缓冲区用来执行读操作
	// 2. 准备 WSARecv() 所需要的一些参数
	// 3. 创建一个 STXIOCPOPERATION 对象并将其放入操作集合(set)中
	// 4. 调用 WSARecv 执行读取操作

	//Step 1
	CSTXIOCPBuffer *pBuffer = m_Buffers.GetBuffer(20);
	if(pBuffer == NULL)
	{
		STXTRACE(_T("----> Pending Reading... (not implement)"));
		//PendingRead(sock, nSocketType, dwExtraData, dwExtraData2, dwTimeOut);
		return TRUE;
	}

	//Step 2
	WSABUF buf;
	buf.buf = (char*)pBuffer->GetBufferPtr();
	buf.len = pBuffer->GetBufferLength();

	DWORD dwNumRead = 0;
	DWORD dwFlags = MSG_PARTIAL;	//根据 MSDN, 此参数目前对于任何协议都没有作用

	//Step 3
	NEW_OPERATION(pOperation);

	pOperation->pTcpConnectionContext = pTcpConnectionContext;

	pOperation->pBuffer = pBuffer;
	pOperation->nOpType = STXIOCP_OPERATION_READ;
	pOperation->sock = pTcpConnectionContext->GetSocket();
	pOperation->dwSubmitTime = GetTickCount();
	pOperation->dwTimeOut = dwTimeOut;
	pOperation->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_TCP_CONNECTION;
	pOperation->nIPVer = STXIOCP_SOCKET_IPV4;
	EnqueueOperation(pOperation);


	//Step 4
	int iResult = 0;
	iResult = WSARecv(pOperation->sock, &buf, 1, &dwNumRead, &dwFlags, &pOperation->ov, NULL);
	int iWsaError = WSAGetLastError();

	if(iResult == 0 && iWsaError == 0)	//Succeed Immediately
	{
		// 即使该操作立即完成，也不必在此进行处理。
		// 完成端口工作线程会在稍后获得这个操作已经完成的通知。

		return TRUE;
	}
	else
	{
		if(iWsaError == ERROR_IO_PENDING)		//IO操作已经投递成功
		{
			return TRUE;
		}
		else
		{
			m_Buffers.ReleaseBuffer(pBuffer);
		}
		pOperation->MarkCanceled();
		DequeueOperation(pOperation);
		pOperation->Release();
	}

	return FALSE;
}

BOOL CSTXIOCPServer::IssueUdpRead(CSTXIOCPUdpServerContext *pUdpServerContext, DWORD dwTimeOut, int nAddressFamily)
{
	// 1. 获得一个缓冲区用来执行读操作
	// 2. 准备 WSARecv() 所需要的一些参数
	// 3. 创建一个 STXIOCPOPERATION 对象并将其放入操作集合(set)中
	// 4. 调用 WSARecv 执行读取操作

	//Step 1
	CSTXIOCPBuffer *pBuffer = m_Buffers.GetBuffer(20);
	if(pBuffer == NULL)
	{
		STXTRACE(_T("----> Pending Reading... (not implement)"));
		//PendingRead(sock, nSocketType, dwExtraData, dwExtraData2, dwTimeOut);
		return TRUE;
	}

	//Step 2
	WSABUF buf;
	buf.buf = (char*)pBuffer->GetBufferPtr();
	buf.len = pBuffer->GetBufferLength();

	DWORD dwNumRead = 0;
	DWORD dwFlags = MSG_PARTIAL;	//根据 MSDN, 此参数目前对于任何协议都没有作用

	//Step 3
	NEW_OPERATION(pOperation);

	pOperation->pBuffer = pBuffer;
	pOperation->nOpType = STXIOCP_OPERATION_READ;
	if(nAddressFamily == AF_INET)
		pOperation->sock = pUdpServerContext->GetSocket();
	else
		pOperation->sock = pUdpServerContext->GetSocket6();

	pOperation->dwSubmitTime = GetTickCount();
	pOperation->dwTimeOut = dwTimeOut;
	pOperation->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_UDPSERVER;
	pOperation->nIPVer = (nAddressFamily == AF_INET6 ? STXIOCP_SOCKET_IPV6 : STXIOCP_SOCKET_IPV4);
	EnqueueOperation(pOperation);


	//Step 4
	int iResult = 0;

	pOperation->nUdpAddressLen = 256;

	iResult = WSARecvFrom(pOperation->sock, &buf, 1, &dwNumRead, &dwFlags, (SOCKADDR*)pOperation->szUdpAddressBuffer, &pOperation->nUdpAddressLen, &pOperation->ov, NULL);
	int iWsaError = WSAGetLastError();

	if(iResult == 0 && iWsaError == 0)	//Succeed Immediately
	{
		// 即使该操作立即完成，也不必在此进行处理。
		// 完成端口工作线程会在稍后获得这个操作已经完成的通知。

		return TRUE;
	}
	else
	{
		if(iWsaError == ERROR_IO_PENDING)		//IO操作已经投递成功
		{
			return TRUE;
		}
		else
		{
			m_Buffers.ReleaseBuffer(pBuffer);
		}

		pOperation->MarkCanceled();
		DequeueOperation(pOperation);
		pOperation->Release();

	}

	return FALSE;
}

BOOL CSTXIOCPServer::IssueClientDisconnectEx(CSTXIOCPServerClientContext *pClientContext, DWORD dwTimeOut)
{
	if(_lpfnDisconnectEx == NULL)
	{
		STXTRACELOGE(_T("Error! DisconnectEx function is null !"));
		return FALSE;
	}

	NEW_OPERATION(pOperation);

	pOperation->pBuffer = NULL;
	pOperation->nOpType = STXIOCP_OPERATION_DISCONNECT;
	pOperation->sock = pClientContext->GetSocketOriginal();
	pOperation->dwSubmitTime = GetTickCount();
	pOperation->dwTimeOut = 0;
	pOperation->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_CLIENT;
	pOperation->pClientContext = pClientContext;
	pOperation->nIPVer = (pClientContext->m_nAddressFamily == AF_INET6 ? STXIOCP_SOCKET_IPV6 : STXIOCP_SOCKET_IPV4);
	EnqueueOperation(pOperation);

	//Step 4
	int iResult = 0;
	iResult = _lpfnDisconnectEx(pClientContext->GetSocketOriginal(), &pOperation->ov, 0, 0);
	int iWsaError = WSAGetLastError();

	if(iResult == 0 && iWsaError == 0)	//Succeed Immediately
	{
		// 即使该操作立即完成，也不必在此进行处理。
		// 完成端口工作线程会在稍后获得这个操作已经完成的通知。
		pClientContext->MarkDisconnected(TRUE);

		return TRUE;
	}
	else
	{
		if(iWsaError == ERROR_IO_PENDING)		//IO操作已经投递成功
		{
			pClientContext->MarkDisconnected(TRUE);
			return TRUE;
		}
		pOperation->MarkCanceled();
		DequeueOperation(pOperation);
		pOperation->Release();
	}
	return FALSE;
}

void CSTXIOCPServer::PendingRead(SOCKET sock, UINT nSocketType, DWORD_PTR dwExtraData, DWORD_PTR dwExtraData2, DWORD dwTimeOut)
{
	READPENDING readOp = {sock, nSocketType, dwExtraData, dwExtraData2, dwTimeOut};
	EnterCriticalSection(&m_csPending);
	m_queueRead.push(readOp);
	LeaveCriticalSection(&m_csPending);
}


LONG CSTXIOCPServer::IssueClientWrite(CSTXIOCPServerClientContext *pClientContext, LPVOID lpData, DWORD cbDataLen, DWORD_PTR dwBufferUserData, DWORD dwTimeOut)
{
	if (pClientContext->m_socket == (SOCKET)INVALID_HANDLE_VALUE)
	{
		return -1;
	}

	//CSTXIOCPBuffer * pBuffer = m_Buffers.GetBuffer(20);

	//CSTXIOCPBuffer *pBuffer = pClientContext->m_pOperationBufferWrite;

	//if (pBuffer == NULL)
	//{
	//	pBuffer = m_Buffers.GetBuffer(20);
	//	pClientContext->m_pOperationBufferWrite = pBuffer;
	//}
	CSTXIOCPBuffer * pBuffer = new CSTXIOCPBuffer();

	if(pBuffer == NULL)
		return -1;

	pClientContext->LockSend();

	pBuffer->ResetWritePos();
	pBuffer->SetUserData(dwBufferUserData);
	DWORD dwWrite = pBuffer->WriteBuffer(lpData, cbDataLen);

	LONG64 nCurrentlyBuffered = pClientContext->GetBufferedSendLength();
	if (nCurrentlyBuffered > 0)
	{
		if (nCurrentlyBuffered >= 1024 * 1024 * 1)
		{
			//STXTRACEE(_T("[r][g][i]Buffered data is more than 32MB. discard further send request."));
			delete pBuffer;
			pClientContext->UnlockSend();
			return -1;
		}
		else if (nCurrentlyBuffered >= 1024 * 128)
		{
			TCHAR szTemp[MAX_PATH];
			StrFormatByteSize64(nCurrentlyBuffered, szTemp, MAX_PATH);
			//STXTRACEE(_T("Data is buffered and will be sent later. Currently buffered %s."), szTemp);
		}

		pClientContext->_queuedSendData.enqueue(pBuffer);
		pClientContext->IncreaseBufferedSendLength(dwWrite);
		pClientContext->UnlockSend();
		return dwWrite;
	}

	DWORD dwFlags = 0;

	NEW_OPERATION(pOperation);

	pOperation->pBuffer = pBuffer;
	pOperation->nOpType = STXIOCP_OPERATION_WRITE;
	pOperation->sock = pClientContext->GetSocketOriginal();
	pOperation->dwSubmitTime = GetTickCount();
	pOperation->dwTimeOut = dwTimeOut;
	pOperation->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_CLIENT;
	pOperation->pClientContext = pClientContext;

	WSABUF buf;
	buf.buf = (char*)pBuffer->GetBufferPtr();
	buf.len = dwWrite;

	DWORD dwSent = 0;

	//Queued
	EnqueueOperation(pOperation);

	int iResult = WSASend(pOperation->sock, &buf, 1, &dwSent, dwFlags, &pOperation->ov, NULL);
	int iWsaError = WSAGetLastError();

	pClientContext->UnlockSend();

	if(iResult == 0 && iWsaError == 0)	//Succeed Immediately
	{
		// 即使该操作立即完成，也不必在此进行处理。
		// 完成端口工作线程会在稍后获得这个操作已经完成的通知。
		pClientContext->IncreaseBufferedSendLength(dwWrite);

		return dwWrite;
	}
	else
	{
		if(iWsaError == ERROR_IO_PENDING)
		{
			pClientContext->IncreaseBufferedSendLength(dwWrite);
			return 0;
		}
		else
		{
			TCHAR szError[MAX_PATH];
			CSTXLog::GetErrorText(szError, MAX_PATH, iWsaError);
			//STXTRACEE(_T("IssueClientWrite Error: %s"), szError);

			pOperation->MarkCanceled();
		}
		DequeueOperation(pOperation);
		pOperation->Release();
		//m_Buffers.ReleaseBuffer(pBuffer);
		delete pBuffer;
	}
	return -1;
}

LONG CSTXIOCPServer::IssueClientWriteForceSend(CSTXIOCPServerClientContext *pClientContext, LPVOID lpData, DWORD cbDataLen, DWORD_PTR dwBufferUserData, DWORD dwTimeOut)
{
	if (pClientContext->m_socket == (SOCKET)INVALID_HANDLE_VALUE)
	{
		return -1;
	}

	CSTXIOCPBuffer * pBuffer = new CSTXIOCPBuffer();

	if (pBuffer == NULL)
		return -1;

	pBuffer->ResetWritePos();
	pBuffer->SetUserData(dwBufferUserData);
	DWORD dwWrite = pBuffer->WriteBuffer(lpData, cbDataLen);

	DWORD dwFlags = 0;

	NEW_OPERATION(pOperation);

	pOperation->pBuffer = pBuffer;
	pOperation->nOpType = STXIOCP_OPERATION_WRITE;
	pOperation->sock = pClientContext->GetSocketOriginal();
	pOperation->dwSubmitTime = GetTickCount();
	pOperation->dwTimeOut = dwTimeOut;
	pOperation->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_CLIENT;
	pOperation->pClientContext = pClientContext;

	WSABUF buf;
	buf.buf = (char*)pBuffer->GetBufferPtr();
	buf.len = dwWrite;

	DWORD dwSent = 0;

	//Queued
	EnqueueOperation(pOperation);

	int iResult = WSASend(pOperation->sock, &buf, 1, &dwSent, dwFlags, &pOperation->ov, NULL);
	int iWsaError = WSAGetLastError();

	if (iResult == 0 && iWsaError == 0)	//Succeed Immediately
	{
		// 即使该操作立即完成，也不必在此进行处理。
		// 完成端口工作线程会在稍后获得这个操作已经完成的通知。
		pClientContext->IncreaseBufferedSendLength(dwWrite);

		return dwWrite;
	}
	else
	{
		if (iWsaError == ERROR_IO_PENDING)
		{
			pClientContext->IncreaseBufferedSendLength(dwWrite);
			return 0;
		}
		else
		{
			TCHAR szError[MAX_PATH];
			CSTXLog::GetErrorText(szError, MAX_PATH, iWsaError);
			STXTRACEE(_T("IssueClientWrite Error: %s"), szError);

			pOperation->MarkCanceled();
		}
		DequeueOperation(pOperation);
		pOperation->Release();
		delete pBuffer;
	}
	return -1;
}

LONG CSTXIOCPServer::IssueTcpConnectionWrite(CSTXIOCPTcpConnectionContext *pTcpConnectionContext, LPVOID lpData, DWORD cbDataLen, DWORD_PTR dwBufferUserData, DWORD dwTimeOut)
{
	CSTXIOCPBuffer *pBuffer = m_Buffers.GetBuffer(20);

	if(pBuffer == NULL)
		return -1;

	pBuffer->SetUserData(dwBufferUserData);

	DWORD dwFlags = 0;

	NEW_OPERATION(pOperation);

	pOperation->pBuffer = pBuffer;
	pOperation->nOpType = STXIOCP_OPERATION_WRITE;
	pOperation->sock = pTcpConnectionContext->GetSocket();
	pOperation->dwSubmitTime = GetTickCount();
	pOperation->dwTimeOut = dwTimeOut;
	pOperation->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_TCP_CONNECTION;

	DWORD dwWrite = pBuffer->WriteBuffer(lpData, cbDataLen);

	WSABUF buf;
	buf.buf = (char*)pBuffer->GetBufferPtr();
	buf.len = dwWrite;

	DWORD dwSent = 0;

	//Queued
	EnqueueOperation(pOperation);

	int iResult = WSASend(pOperation->sock, &buf, 1, &dwSent, dwFlags, &pOperation->ov, NULL);
	int iWsaError = WSAGetLastError();

	if(iResult == 0 && iWsaError == 0)	//Succeed Immediately
	{
		// 即使该操作立即完成，也不必在此进行处理。
		// 完成端口工作线程会在稍后获得这个操作已经完成的通知。

		return dwWrite;
	}
	else
	{
		if(iWsaError == ERROR_IO_PENDING)
		{
			return 0;
		}
		else
		{
			m_Buffers.ReleaseBuffer(pBuffer);
			pOperation->MarkCanceled();
		}
		DequeueOperation(pOperation);
		pOperation->Release();
	}
	return -1;
}

BOOL CSTXIOCPServer::ReleaseClient(CSTXIOCPServerClientContext *pClientContext)
{
	SOCKET sockOriginal = pClientContext->GetSocketOriginal();
	pClientContext->CloseSocket();

	LPCTSTR lpszClientIp = pClientContext->GetClientIP();

	//m_mapClientContext.lock(lpszClientIp);
	//auto it = m_mapClientContext.find(lpszClientIp);

	//if(it != m_mapClientContext.end(lpszClientIp))
	//{
	//	m_mapClientContext.erase(it);
	//}
	//else
	//{
	//	STXTRACELOGE(_T("[r][i]Error! ClientContext [socket = %d] not found!  [ReleaseClient()] @ STXIOCPServer.cpp Line %d"), sockOriginal, __LINE__);
	//	DebugBreak();
	//}

	//m_mapClientContext.unlock(lpszClientIp);

	m_mapClientContext.findValueAndPerform(lpszClientIp, nullptr, [&](CSTXServerObjectPtr<CSTXIOCPServerClientContext> &pClientContext) {
		m_mapClientContext.erase(lpszClientIp);
	}, [&](std::map<std::wstring, CSTXServerObjectPtr<CSTXIOCPServerClientContext>> &innerMap) {
		STXTRACELOGE(_T("[r][i]Error! ClientContext [socket = %d] not found!  [ReleaseClient()] @ STXIOCPServer.cpp Line %d"), sockOriginal, __LINE__);
		DebugBreak();
	});


	return FALSE;
}

void CSTXIOCPServer::PreTcpReceive(CSTXIOCPTcpConnectionContext *pTcpConnCtx, CSTXIOCPBuffer *pBuffer)
{
	pTcpConnCtx->AppendRecvData(pBuffer->GetBufferPtr(), pBuffer->GetDataLength());

	DWORD dwMessageSize = 0;
	while((dwMessageSize = IsTcpDataReadableWrapper(pTcpConnCtx)) > 0)
	{
		CSTXIOCPBuffer buffer;
		buffer.ReallocateBuffer(dwMessageSize);
		buffer.WriteBuffer(pTcpConnCtx->GetMessageBasePtr(), dwMessageSize);
		OnTcpReceivedWrapper(pTcpConnCtx, &buffer);
		pTcpConnCtx->SkipRecvBuffer(dwMessageSize);
	}
}


BOOL CSTXIOCPServer::PreClientReceive(CSTXIOCPServerClientContext *pClientContext, CSTXIOCPBuffer *pBuffer, tr1::shared_ptr<CSTXIOCPTcpServerContext> pServerContext)
{
	BOOL bResult = TRUE;
	pClientContext->AppendRecvData(pBuffer->GetBufferPtr(), pBuffer->GetDataLength());
	DWORD dwMessageSize = 0;
	pClientContext->Lock();

	while((dwMessageSize = IsClientDataReadableWrapper(pClientContext)) > 0)
	{
		CSTXIOCPBuffer buffer;
		buffer.ReallocateBuffer(dwMessageSize);
		buffer.WriteBuffer(pClientContext->GetMessageBasePtr(), dwMessageSize);
		pClientContext->SkipRecvBuffer(dwMessageSize);

		bResult = OnClientReceivedWrapper(pClientContext, &buffer);
	}

	while((dwMessageSize = IsInternalClientData(pClientContext)) > 0)
	{
		CSTXIOCPBuffer buffer;
		buffer.ReallocateBuffer(dwMessageSize);
		buffer.WriteBuffer(pClientContext->GetMessageBasePtr(), dwMessageSize);
		pClientContext->SkipRecvBuffer(dwMessageSize);
		bResult = OnInternalClientReceived(pClientContext, &buffer);
	}

	pClientContext->Unlock();
	return bResult;
}

DWORD CSTXIOCPServer::IsClientDataReadable(CSTXIOCPServerClientContext *pClientContext)
{
	return pClientContext->GetBufferedMessageLength();
}

DWORD CSTXIOCPServer::IsTcpDataReadable(CSTXIOCPTcpConnectionContext *pTcpConnCtx)
{
	return pTcpConnCtx->GetBufferedMessageLength();
}

BOOL CSTXIOCPServer::OnClientReceived(CSTXIOCPServerClientContext *pClientContext, CSTXIOCPBuffer *pBuffer)
{
	return TRUE;
}

BOOL CSTXIOCPServer::OnClientReceivedWrapper(CSTXIOCPServerClientContext *pClientContext, CSTXIOCPBuffer *pBuffer)
{
	BOOL bResult = FALSE;

	BEGIN_TRY()
	bResult = OnClientReceived(pClientContext, pBuffer);
	END_TRY()

	return bResult;
}

void CSTXIOCPServer::OnClientSentWrapper(CSTXIOCPServerClientContext *pClientContext, CSTXIOCPBuffer *pBuffer)
{
	BEGIN_TRY()
	OnClientSent(pClientContext, pBuffer);
	END_TRY()
}

void CSTXIOCPServer::OnTcpSentWrapper(CSTXIOCPTcpConnectionContext *pTcpConnectionContext, CSTXIOCPBuffer *pBuffer)
{
	BEGIN_TRY()
	OnTcpSent(pTcpConnectionContext, pBuffer);
	END_TRY()
}

void CSTXIOCPServer::OnTcpConnectWrapper(CSTXIOCPTcpConnectionContext *pTcpConnectionContext)
{
	BEGIN_TRY()
	OnTcpConnect(pTcpConnectionContext);
	END_TRY()
}

void CSTXIOCPServer::OnTcpDisconnectedWrapper(CSTXIOCPTcpConnectionContext *pTcpConnectionContext, DWORD dwError)
{
	BEGIN_TRY()
	OnTcpDisconnected(pTcpConnectionContext, dwError);
	END_TRY()
}

void CSTXIOCPServer::OnUdpServerSentWrapper(CSTXIOCPUdpServerContext *pUdpServerContext, CSTXIOCPBuffer *pBuffer)
{
	BEGIN_TRY()
	OnUdpServerSent(pUdpServerContext, pBuffer);
	END_TRY()
}

void CSTXIOCPServer::OnCustomOperationCompleteWrapper(DWORD_PTR dwOperationID, DWORD_PTR dwUserData, DWORD_PTR dwCompleteType)
{
	BEGIN_TRY()
	OnCustomOperationComplete(dwOperationID, dwUserData, dwCompleteType);
	END_TRY()
}

void CSTXIOCPServer::OnUrlDownloadProgressWrapper(LPSTXIOCPSERVERHTTPCONTEXT pContext, int nBytesIncrease)
{
	BEGIN_TRY()
	OnUrlDownloadProgress(pContext, nBytesIncrease);
	END_TRY()
}

void CSTXIOCPServer::OnUrlDownloadCompleteWrapper(LPSTXIOCPSERVERHTTPCONTEXT pContext)
{
	BEGIN_TRY()
	OnUrlDownloadComplete(pContext);
	END_TRY()
}

void CSTXIOCPServer::OnUrlDownloadCleanupWrapper(LPSTXIOCPSERVERHTTPCONTEXT pContext)
{
	BEGIN_TRY()
	OnUrlDownloadCleanup(pContext);
	END_TRY()
}

void CSTXIOCPServer::OnFolderChangedWrapper(CSTXIOCPFolderMonitorContext *pFolderMonitorContext, LPCTSTR lpszFolder, FILE_NOTIFY_INFORMATION *pFileNotify)
{
	BEGIN_TRY()
	OnFolderChanged(pFolderMonitorContext, lpszFolder, pFileNotify);
	END_TRY()
}

void CSTXIOCPServer::OnFileChangedWrapper(CSTXIOCPFolderMonitorContext *pFolderMonitorContext, DWORD dwAction, LPCTSTR lpszFileName, LPCTSTR lpszFileFullPathName)
{
	BEGIN_TRY()
	OnInternalFileChanged(pFolderMonitorContext, dwAction, lpszFileName, lpszFileFullPathName);
	OnFileChanged(pFolderMonitorContext, dwAction, lpszFileName, lpszFileFullPathName);
	END_TRY()
}

DWORD CSTXIOCPServer::IsClientDataReadableWrapper(CSTXIOCPServerClientContext *pClientContext)
{
	DWORD dwLen = 0;
	BEGIN_TRY()
	dwLen = IsClientDataReadable(pClientContext);
	END_TRY()

	return dwLen;
}

DWORD CSTXIOCPServer::IsTcpDataReadableWrapper(CSTXIOCPTcpConnectionContext *pTcpConnCtx)
{
	DWORD dwLen = 0;
	BEGIN_TRY()
	dwLen = IsTcpDataReadable(pTcpConnCtx);
	END_TRY()

	return dwLen;
}

void CSTXIOCPServer::OnWorkerThreadInitializeWrapper(LPVOID pStoragePtr)
{
	BEGIN_TRY()
	OnWorkerThreadInitialize(pStoragePtr);
	END_TRY()
}

void CSTXIOCPServer::OnWorkerThreadUninitializeWrapper(LPVOID pStoragePtr)
{
	BEGIN_TRY()
	OnWorkerThreadUninitialize(pStoragePtr);
	END_TRY()
}

DWORD CSTXIOCPServer::OnGetClientReceiveTimeOutWrapper(CSTXIOCPServerClientContext *pClientContext)
{
	DWORD dwLen = 0;
	BEGIN_TRY()
	dwLen = OnGetClientReceiveTimeOut(pClientContext);
	END_TRY()

	return dwLen;
}

void CSTXIOCPServer::OnClientDisconnectWrapper(CSTXIOCPServerClientContext *pClientContext)
{
	BEGIN_TRY()
	OnClientDisconnect(pClientContext);
	END_TRY()
}

void CSTXIOCPServer::OnPostClientDisconnectWrapper(CSTXIOCPServerClientContext *pClientContext)
{
	BEGIN_TRY()
	OnPostClientDisconnect(pClientContext);
	END_TRY()
}

LPVOID CSTXIOCPServer::OnAllocateWorkerThreadLocalStorageWrapper()
{
	LPVOID pTLS = 0;
	BEGIN_TRY()
	pTLS = OnAllocateWorkerThreadLocalStorage();
	END_TRY()

	return pTLS;
}

void CSTXIOCPServer::OnFreeWorkerThreadLocalStorageWrapper(LPVOID pStoragePtr)
{
	BEGIN_TRY()
	OnFreeWorkerThreadLocalStorage(pStoragePtr);
	END_TRY()
}

void CSTXIOCPServer::OnWorkerThreadPreOperationProcessWrapper(LPVOID pStoragePtr)
{
	BEGIN_TRY()
		OnWorkerThreadPreOperationProcess(pStoragePtr);
	END_TRY()
}

BOOL CSTXIOCPServer::OnInternalClientReceived(CSTXIOCPServerClientContext *pClientContext, CSTXIOCPBuffer *pBuffer)
{
	LPSTXIOCPSERVERMESSAGEHEADER pHeader = (LPSTXIOCPSERVERMESSAGEHEADER)pBuffer->GetBufferPtr();
	switch(pHeader->wOpType)
	{
	case STXIOCP_INTERNAL_OP_UPLOAD:
		return OnClientInternalDataTransfer(pClientContext, pBuffer);
		break;
	case STXIOCP_INTERNAL_OP_DOWNLOAD:
		return OnClientInternalDownload(pClientContext, pBuffer);
		break;
	case STXIOCP_INTERNAL_OP_DOWNLOAD_PASSIVE:
		return OnClientInternalPassiveDownload(pClientContext, pBuffer);
		break;
	}

	return TRUE;
}

BOOL CSTXIOCPServer::OnClientInternalDataTransfer(CSTXIOCPServerClientContext *pClientContext, CSTXIOCPBuffer *pBuffer)
{
	LPSTXIOCPSERVERUPLOADFILE pMsg = (LPSTXIOCPSERVERUPLOADFILE)pBuffer->GetBufferPtr();
	pMsg->wOpCode &= 0x7FFF;
	if(pMsg->wOpCode == STXIOCP_INTERNAL_OPCODE_INITIALIZATION)		//Init
	{
		LPVOID pData = (pMsg + 1);
		STXIOCPSERVERUPLOADFILE ack;
		ack.header.wHeaderSize = sizeof(STXIOCPSERVERMESSAGEHEADER);
		ack.header.wOpType = STXIOCP_INTERNAL_OP_UPLOAD;
		ack.header.dwContentSize = sizeof(STXIOCPSERVERUPLOADFILE) - sizeof(STXIOCPSERVERMESSAGEHEADER);
		ack.header.dwMagic = 0xFFFEFDFC;

		ack.dwCookie = OnClientInternalDataTransferBegin(pClientContext, pMsg->dwUserData, pMsg->dwCookie, pMsg->dwContentSize > 0 ? pData:NULL, pMsg->dwContentSize);

		ack.wOpCode = STXIOCP_INTERNAL_OPCODE_INITIALIZATION | STXIOCP_INTERNAL_OPCODE_ACK;
		ack.dwContentSize = 0;
		ack.dwTransferSize = 0;
		ack.dwUserData = pMsg->dwUserData;

		SendClientData(pClientContext, &ack, sizeof(ack));
	}
	else if(pMsg->wOpCode == STXIOCP_INTERNAL_OPCODE_UPLOAD_BLOCK)		//Upload Data Block
	{
		LPVOID pData = (pMsg + 1);
		OnClientInternalDataTransferDataBlock(pClientContext, pMsg->dwCookie, pMsg->dwUserData, pData, pMsg->dwContentSize);
		
		STXIOCPSERVERUPLOADFILE ack;
		ack.header.wHeaderSize = sizeof(STXIOCPSERVERMESSAGEHEADER);
		ack.header.wOpType = STXIOCP_INTERNAL_OP_UPLOAD;
		ack.header.dwContentSize = sizeof(STXIOCPSERVERUPLOADFILE) - sizeof(STXIOCPSERVERMESSAGEHEADER);
		ack.header.dwMagic = 0xFFFEFDFC;

		ack.dwCookie = pMsg->dwCookie;
		ack.wOpCode = STXIOCP_INTERNAL_OPCODE_UPLOAD_BLOCK | STXIOCP_INTERNAL_OPCODE_ACK;
		ack.dwContentSize = 0;
		ack.dwTransferSize = pMsg->dwContentSize;
		ack.dwUserData = pMsg->dwUserData;

		SendClientData(pClientContext, &ack, sizeof(ack));
	}
	else if(pMsg->wOpCode == STXIOCP_INTERNAL_OPCODE_UPLOAD_FINISH)		//Finish
	{
		LPVOID pData = (pMsg + 1);
		if(!pClientContext->IsUploadTask(pMsg->dwCookie, pMsg->dwUserData))
			return TRUE;

		STXIOCPSERVERUPLOADFILE ack;
		ack.header.wHeaderSize = sizeof(STXIOCPSERVERMESSAGEHEADER);
		ack.header.wOpType = STXIOCP_INTERNAL_OP_UPLOAD;
		ack.header.dwContentSize = sizeof(STXIOCPSERVERUPLOADFILE) - sizeof(STXIOCPSERVERMESSAGEHEADER);
		ack.header.dwMagic = 0xFFFEFDFC;

		ack.dwCookie = pMsg->dwCookie;
		ack.wOpCode = STXIOCP_INTERNAL_OPCODE_UPLOAD_FINISH | STXIOCP_INTERNAL_OPCODE_ACK;
		ack.dwContentSize = 0;
		ack.dwTransferSize = 0;
		ack.dwUserData = pMsg->dwUserData;

		SendClientData(pClientContext, &ack, sizeof(ack));
		OnClientInternalDataTransferPreComplete(pClientContext, pMsg->dwCookie, pMsg->dwUserData, pMsg->dwContentSize > 0 ? pData:NULL, pMsg->dwContentSize);
	}

	return TRUE;
}

BOOL CSTXIOCPServer::OnClientInternalPassiveDownload(CSTXIOCPServerClientContext *pClientContext, CSTXIOCPBuffer *pBuffer)
{
	LPSTXIOCPSERVERPASSIVEDOWNLOADFILE pMsg = (LPSTXIOCPSERVERPASSIVEDOWNLOADFILE)pBuffer->GetBufferPtr();
	pMsg->wOpCode &= 0x7FFF;
	if(pMsg->wOpCode == STXIOCP_INTERNAL_OPCODE_INITIALIZATION)		//Init
	{
		LPVOID pPaddingData = (pMsg + 1);
		STXIOCPSERVERPASSIVEDOWNLOADFILE ack;
		ack.header.wHeaderSize = sizeof(STXIOCPSERVERMESSAGEHEADER);
		ack.header.wOpType = STXIOCP_INTERNAL_OP_DOWNLOAD_PASSIVE;
		ack.header.dwContentSize = sizeof(STXIOCPSERVERPASSIVEDOWNLOADFILE) - sizeof(STXIOCPSERVERMESSAGEHEADER);
		ack.header.dwMagic = 0xFFFEFDFC;

		ack.dwCookie = pMsg->dwCookie;

		BOOL bSucceed = OnClientInternalPassiveDownloadBegin(pClientContext, pMsg->dwCookie, pMsg->dwUserData, pMsg->dwContentSize>0?pPaddingData:NULL, pMsg->dwContentSize);

		ack.wResult = (bSucceed ? 0 : 1);

		ack.wOpCode = STXIOCP_INTERNAL_OPCODE_INITIALIZATION | STXIOCP_INTERNAL_OPCODE_ACK;
		ack.dwContentSize = 0;
		ack.dwOffset = 0;
		ack.dwUserData = pMsg->dwUserData;

		SendClientData(pClientContext, &ack, sizeof(ack));
	}
	return TRUE;
}

BOOL CSTXIOCPServer::OnClientInternalDownload(CSTXIOCPServerClientContext *pClientContext, CSTXIOCPBuffer *pBuffer)
{
	LPSTXIOCPSERVERDOWNLOADFILE pMsg = (LPSTXIOCPSERVERDOWNLOADFILE)pBuffer->GetBufferPtr();
	pMsg->wOpCode &= 0x7FFF;
	if(pMsg->wOpCode == STXIOCP_INTERNAL_OPCODE_INITIALIZATION)		//Init
	{
		LPVOID pPaddingData = (pMsg + 1);
		STXIOCPSERVERDOWNLOADFILE ack;
		ack.header.wHeaderSize = sizeof(STXIOCPSERVERMESSAGEHEADER);
		ack.header.wOpType = STXIOCP_INTERNAL_OP_DOWNLOAD;
		ack.header.dwContentSize = sizeof(STXIOCPSERVERDOWNLOADFILE) - sizeof(STXIOCPSERVERMESSAGEHEADER);
		ack.header.dwMagic = 0xFFFEFDFC;

		ack.dwCookie = pMsg->dwCookie;

		DWORD dwDownloadSize = OnClientInternalDownloadBegin(pClientContext, pMsg->dwCookie, pMsg->dwUserData, pMsg->dwContentSize>0?pPaddingData:NULL, pMsg->dwContentSize);

		ack.wOpCode = STXIOCP_INTERNAL_OPCODE_INITIALIZATION | STXIOCP_INTERNAL_OPCODE_ACK;
		ack.dwContentSize = 0;
		ack.dwOffset = 0;
		ack.dwTransferSize = dwDownloadSize;
		ack.dwUserData = pMsg->dwUserData;

		SendClientData(pClientContext, &ack, sizeof(ack));
	}
	else if(pMsg->wOpCode == STXIOCP_INTERNAL_OPCODE_DOWNLOAD_BLOCK)		//Download Data Block
	{
		char *pszBuffer = new char[16400];
		LPSTXIOCPSERVERDOWNLOADFILE pAck = (LPSTXIOCPSERVERDOWNLOADFILE)pszBuffer;

		DWORD dwDataSize = OnClientInternalDownloadDataBlock(pClientContext, pMsg->dwCookie, pMsg->dwUserData, pMsg->dwOffset, pAck + 1, 8000);

		pAck->header.wHeaderSize = sizeof(STXIOCPSERVERMESSAGEHEADER);
		pAck->header.wOpType = STXIOCP_INTERNAL_OP_DOWNLOAD;
		pAck->header.dwContentSize = sizeof(STXIOCPSERVERDOWNLOADFILE) - sizeof(STXIOCPSERVERMESSAGEHEADER) + dwDataSize;
		pAck->header.dwMagic = 0xFFFEFDFC;

		pAck->dwCookie = pMsg->dwCookie;
		pAck->wOpCode = STXIOCP_INTERNAL_OPCODE_DOWNLOAD_BLOCK | STXIOCP_INTERNAL_OPCODE_ACK;
		pAck->dwContentSize = dwDataSize;
		pAck->dwTransferSize = dwDataSize;
		pAck->dwUserData = pMsg->dwUserData;

		SendClientData(pClientContext, pAck, dwDataSize + sizeof(STXIOCPSERVERDOWNLOADFILE));
		delete []pszBuffer;
	}
/*	else if(pMsg->wOpCode == STXIOCP_INTERNAL_OPCODE_DOWNLOAD_FINISH)		//Finish
	{
		if(!pClientContext->IsUploadTask(pMsg->dwCookie))
			return TRUE;

		STXIOCPSERVERUPLOADFILE ack;
		ack.header.wHeaderSize = sizeof(STXIOCPSERVERMESSAGEHEADER);
		ack.header.wOpType = STXIOCP_INTERNAL_OP_DOWNLOAD;
		ack.header.dwContentSize = sizeof(STXIOCPSERVERUPLOADFILE) - sizeof(STXIOCPSERVERMESSAGEHEADER);
		ack.header.dwMagic = 0xFFFEFDFC;

		ack.dwCookie = pMsg->dwCookie;
		ack.wOpCode = STXIOCP_INTERNAL_OPCODE_DOWNLOAD_FINISH | STXIOCP_INTERNAL_OPCODE_ACK;
		ack.dwContentSize = 0;
		ack.dwTransferSize = 0;

		pClientContext->Send(&ack, sizeof(ack));
		OnClientInternalDataTransferComplete(pClientContext, pMsg->dwCookie);
	}
*/

	return TRUE;
}

DWORD CSTXIOCPServer::OnClientInternalDataTransferBegin(CSTXIOCPServerClientContext *pClientContext, DWORD dwUserDataRemote, DWORD dwCookieDesire, LPVOID pPaddingData, DWORD dwPaddingDataLen)
{
	return pClientContext->PrepareUpload(dwUserDataRemote, dwCookieDesire);
}

void CSTXIOCPServer::OnClientInternalDataTransferDataBlock(CSTXIOCPServerClientContext *pClientContext, DWORD dwCookie, DWORD dwUserDataRemote, LPVOID lpDataBlock, DWORD dwDataBlockLen)
{
	pClientContext->ProcessUpload(dwCookie, dwUserDataRemote, lpDataBlock, dwDataBlockLen);
}

void CSTXIOCPServer::OnClientInternalDataTransferPreComplete(CSTXIOCPServerClientContext *pClientContext, DWORD dwCookie, DWORD dwUserDataRemote, LPVOID pPaddingData, DWORD dwPaddingDataLen)
{
	OnClientInternalDataTransferComplete(pClientContext, dwCookie, dwUserDataRemote, pPaddingData, dwPaddingDataLen);
	pClientContext->CloseUpload(dwCookie, dwUserDataRemote);
}

void CSTXIOCPServer::OnClientInternalDataTransferComplete(CSTXIOCPServerClientContext *pClientContext, DWORD dwCookie, DWORD dwUserDataRemote, LPVOID pPaddingData, DWORD dwPaddingDataLen)
{
	//Do something in derived class
}

DWORD CSTXIOCPServer::OnClientInternalDownloadBegin(CSTXIOCPServerClientContext *pClientContext, DWORD dwCookie, DWORD dwUserDataRemote, LPVOID pPaddingData, DWORD dwPaddingDataLen)
{
	return pClientContext->OnQueryDownloadSize(dwCookie, pPaddingData, dwPaddingDataLen, dwUserDataRemote);
}

DWORD CSTXIOCPServer::OnClientInternalDownloadDataBlock(CSTXIOCPServerClientContext *pClientContext, DWORD dwCookie, DWORD dwUserDataRemote, DWORD dwOffset, __out LPVOID pDataBuffer, DWORD dwDataBufferLen)
{
	return pClientContext->OnQueryDownloadData(dwCookie, dwOffset, pDataBuffer, dwDataBufferLen, dwUserDataRemote);
}

BOOL CSTXIOCPServer::OnClientInternalPassiveDownloadBegin(CSTXIOCPServerClientContext *pClientContext, DWORD dwCookie, DWORD dwUserDataRemote, __out LPVOID pDataBuffer, DWORD dwDataBufferLen)
{
	LARGE_INTEGER v;
	v.LowPart = dwUserDataRemote;
	v.HighPart = dwCookie;
	EnterCriticalSection(&m_csFileDictionary);
	map<__int64, STLSTRING>::iterator it = m_mapFileDictionary.find(v.QuadPart);
	if(it == m_mapFileDictionary.end(v.QuadPart))
	{
		LeaveCriticalSection(&m_csFileDictionary);
		return FALSE;
	}
	BOOL bResult = IssueReadFile(it->second.c_str(), pClientContext, 0, TRUE, dwCookie, dwUserDataRemote);
	LeaveCriticalSection(&m_csFileDictionary);

	return bResult;
/*
	for(int i=0;i<5;i++)
	{

		char szTest[32] = "This is a sample...";
		char szBuf[1024];
		LPSTXIOCPSERVERPASSIVEDOWNLOADFILE pMsg = (LPSTXIOCPSERVERPASSIVEDOWNLOADFILE)szBuf;
		char *pMsgPadding = (char*)(pMsg + 1);
		strcpy_s(pMsgPadding, 32, szTest);

		pMsg->header.wHeaderSize = sizeof(STXIOCPSERVERMESSAGEHEADER);
		pMsg->header.wOpType = STXIOCP_INTERNAL_OP_DOWNLOAD_PASSIVE;
		pMsg->header.dwContentSize = sizeof(STXIOCPSERVERPASSIVEDOWNLOADFILE) - sizeof(STXIOCPSERVERMESSAGEHEADER) + 32;
		pMsg->header.dwMagic = 0xFFFEFDFC;

		pMsg->dwContentSize = 32;
		pMsg->dwCookie = dwCookie;
		pMsg->dwUserData = dwUserDataRemote;
		pMsg->wOpCode = STXIOCP_INTERNAL_OPCODE_DOWNLOAD_BLOCK | STXIOCP_INTERNAL_OPCODE_ACK;
		pMsg->wResult = 0;

		SendClientData(pClientContext, szBuf, sizeof(STXIOCPSERVERPASSIVEDOWNLOADFILE) + pMsg->dwContentSize, 0);
	}

	STXIOCPSERVERPASSIVEDOWNLOADFILE msg;
	msg.header.wHeaderSize = sizeof(STXIOCPSERVERMESSAGEHEADER);
	msg.header.wOpType = STXIOCP_INTERNAL_OP_DOWNLOAD_PASSIVE;
	msg.header.dwContentSize = sizeof(STXIOCPSERVERPASSIVEDOWNLOADFILE) - sizeof(STXIOCPSERVERMESSAGEHEADER) + 32;
	msg.header.dwMagic = 0xFFFEFDFC;

	msg.dwContentSize = 0;
	msg.dwCookie = dwCookie;
	msg.dwUserData = dwUserDataRemote;
	msg.wOpCode = STXIOCP_INTERNAL_OPCODE_DOWNLOAD_FINISH | STXIOCP_INTERNAL_OPCODE_ACK;
	msg.wResult = 0;

	SendClientData(pClientContext, &msg, sizeof(STXIOCPSERVERPASSIVEDOWNLOADFILE), 0);
*/

	return TRUE;
}

void CSTXIOCPServer::OnUdpServerReceived(CSTXIOCPUdpServerContext *pUdpServerContext, CSTXIOCPBuffer *pBuffer, SOCKADDR *pFromAddr, INT nAddrLen)
{
	TCHAR szAddress[MAX_PATH];
	DWORD dwAddressLen = MAX_PATH;
	WSAAddressToString(pFromAddr, nAddrLen, NULL, szAddress, &dwAddressLen);

	STXTRACE(_T("UDP Server [%p:%d] received %d bytes from %s"), pUdpServerContext, pUdpServerContext->GetServerParam(), pBuffer->GetDataLength(), szAddress);
	
	//ShowUserCallStack();
}

void CSTXIOCPServer::OnUdpServerReceivedWrapper(CSTXIOCPUdpServerContext *pUdpServerContext, CSTXIOCPBuffer *pBuffer, SOCKADDR *pFromAddr, INT nAddrLen)
{
	BEGIN_TRY()
	OnUdpServerReceived(pUdpServerContext, pBuffer, pFromAddr, nAddrLen);
	END_TRY()
}

void CSTXIOCPServer::OnClientSent(CSTXIOCPServerClientContext *pClientContext, CSTXIOCPBuffer *pBuffer)
{

}

void CSTXIOCPServer::OnUdpServerSent(CSTXIOCPUdpServerContext *pUdpServerContext, CSTXIOCPBuffer *pBuffer)
{

}

void CSTXIOCPServer::OnClientDisconnect(CSTXIOCPServerClientContext *pClientContext)
{
	pClientContext->m_nDisconnectEventProcessed++;
}

void CSTXIOCPServer::OnPostClientDisconnect(CSTXIOCPServerClientContext *pClientContext)
{

}

void CSTXIOCPServer::OnClientDestroy(CSTXIOCPServerClientContext *pClientContext)
{

}

void CSTXIOCPServer::PreTcpDisconnect(CSTXIOCPTcpConnectionContext *pTcpConnCtx, DWORD dwError)
{
	if((m_BaseServerInfo.dwStatus & STXSS_TERMINATE) == 0 && pTcpConnCtx->GetFlags() & STXIOCP_SERVER_CONNECTION_FLAG_KEEPCONNECT)
	{
		if(dwError == ERROR_CONNECTION_REFUSED || dwError == WSAENETUNREACH || dwError == WSAETIMEDOUT)
		{
			//The socket can be reused
		}
		else
		{
			closesocket(pTcpConnCtx->GetSocket());
			pTcpConnCtx->SetSocket(INVALID_SOCKET);
		}
		PendingTcpConnection(pTcpConnCtx->GetTargetHostName(), pTcpConnCtx->GetTargetHostPort(), pTcpConnCtx->GetServerParam(), NULL /*Not used when pTcpConnCtx is not null*/, pTcpConnCtx->GetAddressFamily(), TRUE, pTcpConnCtx);
		return;
	}

	STXTRACELOGE(_T("TCP connection [%d] disconnected. -> %s:%d"), pTcpConnCtx->GetConnectionID(), pTcpConnCtx->GetTargetHostName(), pTcpConnCtx->GetTargetHostPort());
	OnTcpDisconnectedWrapper(pTcpConnCtx, dwError);

	LockConnectionMap();
	map<LONG, CSTXServerObjectPtr<CSTXIOCPTcpConnectionContext> >::iterator it = m_mapConnections.find(pTcpConnCtx->GetConnectionID());
	if(it != m_mapConnections.end())
	{
		if(!it->second->IsConnected())
		{
			delete it->second->GetContextKey();
		}
		it->second->Release();
		m_mapConnections.erase(it);
	}
	UnlockConnectionMap();

}


void CSTXIOCPServer::OnTcpDisconnected(CSTXIOCPTcpConnectionContext *pTcpConnCtx, DWORD dwError)
{
	STXTRACELOG(_T("[r][g][i]CSTXIOCPServer::OnTcpDisconnected : Warning! The forward Connection [%d] has been disconnected!"), pTcpConnCtx->GetConnectionID());
}

void CSTXIOCPServer::OnTcpReceived(CSTXIOCPTcpConnectionContext *pTcpConnCtx, CSTXIOCPBuffer *pBuffer)
{
	STXTRACELOG(_T("[r][g][i]CSTXIOCPServer::OnTcpReceived : [Connection:%d] Received %d bytes."), pTcpConnCtx->GetConnectionID(), pBuffer->GetDataLength());
}

void CSTXIOCPServer::OnTcpReceivedWrapper(CSTXIOCPTcpConnectionContext *pTcpConnCtx, CSTXIOCPBuffer *pBuffer)
{
	BEGIN_TRY()
	OnTcpReceived(pTcpConnCtx, pBuffer);
	END_TRY()
}

void CSTXIOCPServer::OnTcpSent(CSTXIOCPTcpConnectionContext *pTcpConnectionContext, CSTXIOCPBuffer *pBuffer)
{

}

void CSTXIOCPServer::OnTcpConnect(CSTXIOCPTcpConnectionContext *pTcpConnectionContext)
{

}

void CSTXIOCPServer::OnCustomOperationComplete(DWORD_PTR dwOperationID, DWORD_PTR dwUserData, DWORD_PTR dwCompleteType)
{

}

void CSTXIOCPServer::OnInternalCustomOperationComplete(UINT nOperationType, DWORD_PTR dwOperationID, DWORD_PTR dwUserData, DWORD_PTR dwUserData2, DWORD_PTR dwCompleteType)
{
	if (nOperationType == STXIOCP_OPERATION_FILE_CHANGE_NOTIFY)
	{
		LPSTXIOCPCONTEXTKEY lpContextKey = (LPSTXIOCPCONTEXTKEY)dwUserData2;
		PFILE_NOTIFY_INFORMATION pChangeRecord = (PFILE_NOTIFY_INFORMATION)dwUserData;

		while (pChangeRecord)
		{
			OnFolderChangedWrapper(lpContextKey->pFolderMonitorContext, lpContextKey->szMonitoredFolder, pChangeRecord);

			std::wstring filename(pChangeRecord->FileName, pChangeRecord->FileNameLength / sizeof(TCHAR));
			std::wstring fullFilename = GetServerModuleFilePath() + filename;

			LPCTSTR lpszExt = PathFindExtension(filename.c_str());
			if (lpszExt && lpContextKey->pFolderMonitorContext->IsIgnoreFileExtension(lpszExt))
			{
				//Ignore files with specific extensions
			}
			else
			{
				OnFileChangedWrapper(lpContextKey->pFolderMonitorContext, pChangeRecord->Action, filename.c_str(), fullFilename.c_str());
			}

			if (pChangeRecord->NextEntryOffset != 0)
			{
				pChangeRecord = (PFILE_NOTIFY_INFORMATION)((LPBYTE)pChangeRecord + pChangeRecord->NextEntryOffset);
			}
			else
			{
				pChangeRecord = NULL;
			}
		}

		delete[]((char*)dwUserData);
	}
}

BOOL CSTXIOCPServer::OnClientContextOperationTimeout(CSTXIOCPServerClientContext *pClientContext)
{
	return TRUE;
}

void CSTXIOCPServer::OnFolderChanged(CSTXIOCPFolderMonitorContext *pFolderMonitorContext, LPCTSTR lpszFolder, FILE_NOTIFY_INFORMATION *pFileNotify)
{

}

void CSTXIOCPServer::OnInternalFileChanged(CSTXIOCPFolderMonitorContext *pFolderMonitorContext, DWORD dwAction, LPCTSTR lpszFileName, LPCTSTR lpszFileFullPathName)
{
	LPCTSTR lpszRelativePathName = lpszFileFullPathName + _tcslen(GetServerModuleFilePath());
	static LPCTSTR actionText[10] = { 0, _T("Added"), _T("Removed"), _T("Modified"), _T("Renaming"), _T("Renamed"), 0, 0, _T("Refreshed"), 0 };

	//STXTRACEE(_T("OnFileChanged %s: %s"), actionText[dwAction], lpszRelativePathName);

	FILETIME ftLastModify;
	if (!GetFileLastModifyTime(lpszFileFullPathName, &ftLastModify))
	{
		if (dwAction == FILE_ACTION_REMOVED)
		{
			//STXTRACELOGE(_T("File %s: %s"), actionText[dwAction], lpszRelativePathName);
			pFolderMonitorContext->_fileLastModifyTime.erase(lpszRelativePathName);
			pFolderMonitorContext->_fileLastChangeType.erase(lpszRelativePathName);
			OnFileChangedFiltered(pFolderMonitorContext, dwAction, lpszRelativePathName, lpszFileFullPathName);
		}
		else if (dwAction == FILE_ACTION_RENAMED_OLD_NAME)
		{
			pFolderMonitorContext->_fileLastModifyTime.erase(lpszRelativePathName);
			pFolderMonitorContext->_fileLastChangeType.erase(lpszRelativePathName);
			OnFileChangedFiltered(pFolderMonitorContext, dwAction, lpszRelativePathName, lpszFileFullPathName);
		}
		return;
	}

	BOOL bTrigger = FALSE;

	pFolderMonitorContext->_fileLastChangeType.lock(lpszRelativePathName);
	auto itChangeType = pFolderMonitorContext->_fileLastChangeType.find(lpszRelativePathName);
	BOOL bFound = (itChangeType != pFolderMonitorContext->_fileLastChangeType.end(lpszRelativePathName));
	if (!bFound || itChangeType->second != dwAction)
	{
		if (dwAction == FILE_ACTION_ADDED && bFound && itChangeType->second != FILE_ACTION_MODIFIED)
		{
			bTrigger = TRUE;
			//STXTRACELOGE(_T("File %s: %s"), actionText[dwAction], lpszRelativePathName);
		}
		pFolderMonitorContext->_fileLastChangeType[lpszRelativePathName] = dwAction;
		if (!bFound)
		{
			bTrigger = TRUE;
		}
		pFolderMonitorContext->_fileLastChangeType.unlock(lpszRelativePathName);

		if (bTrigger)
		{
			pFolderMonitorContext->_fileLastModifyTime.insertValue(lpszRelativePathName, ftLastModify);
			OnFileChangedFiltered(pFolderMonitorContext, dwAction, lpszRelativePathName, lpszFileFullPathName);
			return;
		}
	}
	else
	{
		pFolderMonitorContext->_fileLastChangeType.unlock(lpszRelativePathName);
	}

	DWORD dwActionNotify = dwAction;

	//else
	{
		pFolderMonitorContext->_fileLastModifyTime.lock(lpszRelativePathName);
		auto it = pFolderMonitorContext->_fileLastModifyTime.find(lpszRelativePathName);
		if (it == pFolderMonitorContext->_fileLastModifyTime.end(lpszRelativePathName) || CompareFileTime(&it->second, &ftLastModify) != 0)
		{
			//STXTRACELOGE(_T("File %s: %s"), actionText[dwAction], lpszRelativePathName);
			pFolderMonitorContext->_fileLastModifyTime[lpszRelativePathName] = ftLastModify;
			if (it != pFolderMonitorContext->_fileLastModifyTime.end(lpszRelativePathName))
			{
				bTrigger = TRUE;
			}
		}
		pFolderMonitorContext->_fileLastModifyTime.unlock(lpszRelativePathName);
		if (dwAction == FILE_ACTION_RENAMED_NEW_NAME)
		{
			bTrigger = TRUE;
		}
		else if (dwAction == FILE_ACTION_RENAMED_OLD_NAME)
		{
			bTrigger = TRUE;
			pFolderMonitorContext->_fileLastModifyTime.erase(lpszRelativePathName);
		}

		if (bTrigger)
		{
			pFolderMonitorContext->_fileLastChangeType.insertValue(lpszRelativePathName, dwAction);
			OnFileChangedFiltered(pFolderMonitorContext, dwActionNotify, lpszRelativePathName, lpszFileFullPathName);
		}
	}
}

void CSTXIOCPServer::OnFileChanged(CSTXIOCPFolderMonitorContext *pFolderMonitorContext, DWORD dwAction, LPCTSTR lpszFileName, LPCTSTR lpszFileFullPathName)
{
	
}

void CSTXIOCPServer::OnFileChangedFiltered(CSTXIOCPFolderMonitorContext *pFolderMonitorContext, DWORD dwAction, LPCTSTR lpszFileName, LPCTSTR lpszFileFullPathName)
{
	
}

void CSTXIOCPServer::OnWorkerThreadInitialize(LPVOID pStoragePtr)
{

}

void CSTXIOCPServer::OnWorkerThreadUninitialize(LPVOID pStoragePtr)
{

}

LPVOID CSTXIOCPServer::OnAllocateWorkerThreadLocalStorage()
{
	return NULL;
}

void CSTXIOCPServer::OnFreeWorkerThreadLocalStorage(LPVOID pStoragePtr)
{

}

void CSTXIOCPServer::OnWorkerThreadPreOperationProcess(LPVOID pStoragePtr)
{

}

DWORD CSTXIOCPServer::OnGetClientReceiveTimeOut(CSTXIOCPServerClientContext *pClientContext)
{
	return pClientContext->m_dwOperationTimeout;
}

LPCTSTR CSTXIOCPServer::OnGetUserDefinedExceptionName(DWORD dwExceptionCode)
{
	return NULL;
}

DWORD CSTXIOCPServer::OnParseUserDefinedExceptionArgument(DWORD dwExceptionCode, DWORD nArguments, ULONG_PTR *pArgumentArray, LPTSTR lpszBuffer, UINT cchBufferSize)
{
	return 0;
}

void CSTXIOCPServer::LockServersMap()
{
	EnterCriticalSection(&m_csServersMap);
}

void CSTXIOCPServer::UnlockServersMap()
{
	LeaveCriticalSection(&m_csServersMap);
}

CSTXIOCPServerClientContext *CSTXIOCPServer::OnCreateClientContext(tr1::shared_ptr<CSTXIOCPTcpServerContext> pServerContext)
{
	return new CSTXIOCPServerClientContext();
}

CSTXIOCPTcpConnectionContext* CSTXIOCPServer::OnCreateTcpConnectionContext()
{
	return new CSTXIOCPTcpConnectionContext();
}

CSTXServerContextBase* CSTXIOCPServer::OnCreateServerContext()
{
	CSTXServerContextBase *pServerContext = new CSTXIOCPServerContext();
	return pServerContext;
}

DWORD CSTXIOCPServer::IsInternalClientData(CSTXIOCPServerClientContext *pClientContext)
{
	DWORD dwMsgDataLen = pClientContext->GetBufferedMessageLength();

	if(dwMsgDataLen < sizeof(WORD))
		return 0;

	WORD wHeaderSize = *((WORD*)pClientContext->GetMessageBasePtr());

	if(wHeaderSize != sizeof(STXIOCPSERVERMESSAGEHEADER))
		return 0;

	if(dwMsgDataLen < wHeaderSize)
		return 0;

	LPSTXIOCPSERVERMESSAGEHEADER pHeader = (LPSTXIOCPSERVERMESSAGEHEADER)pClientContext->GetMessageBasePtr();
	if(pHeader->dwMagic != 0xFFFEFDFC)
		return 0;

	DWORD dwMsgTotalSize = pHeader->dwContentSize + sizeof(STXIOCPSERVERMESSAGEHEADER);

	if(dwMsgDataLen >= dwMsgTotalSize)
		return dwMsgTotalSize;

	return 0;
}

LONG CSTXIOCPServer::EnqueueOperation(LPSTXIOCPOPERATION lpOperation)
{
	//m_setOperation.insert(lpOperation);

	if(lpOperation->dwTimeOut > 0)
	{
		lpOperation->dwOriginalTimeOut = lpOperation->dwTimeOut;
		lpOperation->AddRef();
		lpOperation->AddRef();
		LONGLONG nAltId = -1;
		HANDLE hTimer = AddTimerObject(lpOperation, lpOperation->dwTimeOut, &nAltId);
		lpOperation->hTimerHandle = hTimer;
		lpOperation->nTimerAltId = nAltId;

	}
	else
	{
		lpOperation->AddRef();
	}
	return 1;
}

LONG CSTXIOCPServer::DequeueOperation(LPSTXIOCPOPERATION lpOperation)
{
	//m_setOperation.erase(lpOperation);

	if(lpOperation->dwOriginalTimeOut > 0)
	{
		//STXTRACE(_T("DequeueOperation\t%p"), lpOperation);
		if (lpOperation->hTimerHandle)
		{
			lpOperation->Lock();
			if (lpOperation->hTimerHandle)
			{
				DeleteTimerObject(lpOperation->hTimerHandle, lpOperation->nTimerAltId);
				lpOperation->hTimerHandle = NULL;
			}
			lpOperation->Unlock();
		}
		lpOperation->Release();
		lpOperation->Release();
	}
	else
	{
		lpOperation->Release();
	}

	return 1;
}

int CSTXIOCPServer::SockaddrFromHost(LPCTSTR lpszHostAddress, UINT uPort, SOCKADDR *pAddr, int nAddressFamily)
{
	int nResult = -1;
	if(nAddressFamily == AF_INET6)
	{
		int nLen = sizeof(SOCKADDR_IN6);
		nResult = WSAStringToAddress((LPTSTR)lpszHostAddress, AF_INET6, NULL, (SOCKADDR*)pAddr, &nLen);
		if (nResult == 0)
		{
			((SOCKADDR_IN6*)pAddr)->sin6_port = htons(uPort);
		}
	}
	else if (nAddressFamily == AF_INET)
	{
		int nLen = sizeof(SOCKADDR_IN);
		nResult = WSAStringToAddress((LPTSTR)lpszHostAddress, AF_INET, NULL, (SOCKADDR*)pAddr, &nLen);
		if (nResult == 0)
		{
			((SOCKADDR_IN*)pAddr)->sin_port = htons(uPort);
		}
	}
	if (nResult != 0)
	{
		int iWsaError = WSAGetLastError();
		TCHAR szError[MAX_PATH];
		CSTXLog::GetErrorText(szError, MAX_PATH, iWsaError);
		STXTRACELOGE(_T("WSAStringToAddress failed! Address = %s:%d,  Error = %d,%s"), lpszHostAddress, uPort, iWsaError, szError);
	}
	return nResult;
}

BOOL CSTXIOCPServer::KeepAlive(SOCKET sock, DWORD dwFirstTime, DWORD dwInterval)
{
	//////////////////////////////////////////////////////////////////////////
	// 下面的代码用于保持长时间连接
	//定义结构及宏
	struct TCP_KEEPALIVE_STRUCT
	{
		u_long  onoff;
		u_long  keepalivetime;
		u_long  keepaliveinterval;
	};

#define SIO_KEEPALIVE_VALS _WSAIOW(IOC_VENDOR , 4)

	//KeepAlive实现
	TCP_KEEPALIVE_STRUCT inKeepAlive = {0}; //输入参数
	unsigned long ulInLen = sizeof(TCP_KEEPALIVE_STRUCT); 

	TCP_KEEPALIVE_STRUCT outKeepAlive = {0}; //输出参数
	unsigned long ulOutLen = sizeof(TCP_KEEPALIVE_STRUCT); 

	unsigned long ulBytesReturn = 0; 

	//设置socket的keep alive
	inKeepAlive.onoff = 1; 
	inKeepAlive.keepaliveinterval = dwInterval;		//两次KeepAlive探测间的时间间隔
	inKeepAlive.keepalivetime = dwFirstTime;		//开始首次KeepAlive探测前的TCP空闭时间

	if (WSAIoctl((unsigned int)sock, SIO_KEEPALIVE_VALS,
		(LPVOID)&inKeepAlive, ulInLen,
		(LPVOID)&outKeepAlive, ulOutLen,
		&ulBytesReturn, NULL, NULL) == SOCKET_ERROR) 
	{
		STXLOGE(_T("SIO_KEEPALIVE_VALS failed!"));
		return FALSE;
	}

	return TRUE;
}


LONG CSTXIOCPServer::CreateTcpConnection(LPCTSTR lpszTargetHostAddress, UINT uTargetPort, DWORD_PTR dwConnectionParam, LPCTSTR lpszServerParamString, int nAddressFamily, BOOL bKeepConnect)
{
	SOCKADDR *pAddr = NULL;
	int nAddrLen = sizeof(SOCKADDR_IN);
	if(nAddressFamily == AF_INET)
		pAddr = (SOCKADDR*)new SOCKADDR_IN;
	else
	{
		pAddr = (SOCKADDR*)new SOCKADDR_IN6;
		nAddrLen = sizeof(SOCKADDR_IN6);
	}
		
	int nAddressResult = SockaddrFromHost(lpszTargetHostAddress, uTargetPort, pAddr, nAddressFamily);
	if (nAddressResult != 0)
	{
		return -1;
	}

	//Create Socket
	SOCKET sock = WSASocket(nAddressFamily, SOCK_STREAM, IPPROTO_TCP, 0, 0, WSA_FLAG_OVERLAPPED);
	if(sock == INVALID_SOCKET)
	{
		delete pAddr;
		return -1;
	}

	if(connect(sock, (SOCKADDR*)pAddr, nAddrLen) == 0)
	{
		//Succeed
		CSTXIOCPTcpConnectionContext *pTcpConnCtx = OnCreateTcpConnectionContext();

		pTcpConnCtx->SetAddressFamily(nAddressFamily);

		LockConnectionMap();
		pTcpConnCtx->SetConnectionID(GetNextTcpConnectionID());
		pTcpConnCtx->SetSocket(sock);
		pTcpConnCtx->SetTargetHoatName(lpszTargetHostAddress);
		pTcpConnCtx->SetTargetHoatPort(uTargetPort);
		pTcpConnCtx->SetServerParamString(lpszServerParamString);
		pTcpConnCtx->SetServerParam(dwConnectionParam);
		pTcpConnCtx->MarkConnected();


		if(bKeepConnect)
			pTcpConnCtx->ModifyFlags(STXIOCP_SERVER_CONNECTION_FLAG_KEEPCONNECT, 0);

		m_mapConnections[pTcpConnCtx->GetConnectionID()] = pTcpConnCtx;
		UnlockConnectionMap();

		LPSTXIOCPCONTEXTKEY pNewContextKey = new STXIOCPCONTEXTKEY;
		memset(pNewContextKey, 0, sizeof(STXIOCPCONTEXTKEY));
		_tcscpy_s(pNewContextKey->szDescription, 32, _T("[Connection Socket]"));
		pNewContextKey->sock = sock;
		pNewContextKey->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_TCP_CONNECTION;
		pNewContextKey->pClientContext = NULL;
		pNewContextKey->pTcpConnectionContext = pTcpConnCtx;

		pTcpConnCtx->SetContextKey(pNewContextKey);

		KeepAlive(sock, 120000, 120000);

		HANDLE hIOCP = ::CreateIoCompletionPort((HANDLE)pNewContextKey->sock, m_hIOCP, (ULONG_PTR)pNewContextKey, 0);

		//IssueRead(sock, STXIOCP_CONTEXTKEY_SOCKET_TYPE_TCP_CONNECTION, (DWORD_PTR)pConnectionInfo, (DWORD_PTR)pTcpConnCtx, 0, nAddressFamily);

		IssueTcpConnectionRead(pTcpConnCtx, 0);

		delete pAddr;
		return pTcpConnCtx->GetConnectionID();
	}

	//else if(connect())
	int iWsaError = WSAGetLastError();

	if (iWsaError == ERROR_IO_PENDING)		//IO操作已经投递成功
	{
		return -1;
	}
	else
	{
		TCHAR szError[MAX_PATH];
		CSTXLog::GetErrorText(szError, MAX_PATH, iWsaError);
		STXTRACELOG(_T("TCP Connecting failed!...... Socket = 0x%x, Target=%s:%d, Error = %d,%s"), sock, lpszTargetHostAddress, uTargetPort, iWsaError, szError);
		closesocket(sock);
		delete pAddr;
	}
	return -1;
}

LONG CSTXIOCPServer::PendingTcpConnection(LPCTSTR lpszTargetHostAddress, UINT uTargetPort, DWORD_PTR dwConnectionParam, LPCTSTR lpszServerParamString, int nAddressFamily, BOOL bKeepConnect, CSTXIOCPTcpConnectionContext *pKeepTcpConnCtx)
{
	// PendingTcpConnection 的实现基于 ConnectEx

	// 1. 从指定的目标地址和端口生成一个 SOCKADDR_IN 结构
	// 2. 创建 Socket
	// 3. 绑定到目标地址
	// 4. 取得 ConnectEx 函数指针
	// 5. 创建一个 CSTXIOCPTcpConnectionContext 对象与 socket 关联
	// 6. 创建一个 STXIOCPSERVERCONNECTIONINFO 对象用来保存此连接的更多信息
	// 7. 创建一个 STXIOCPCONTEXTKEY 用于完成端口
	// 8. 将此连接记录到连接map中
	// 9. 将 socket 与完成端口关联起来
	// 10. 为即将发出的连接操作创建一个 STXIOCPOPERATION 对象并记录到操作列表中
	// 11. 设置 socket 的 Keep-Alive 属性
	// 12. 发出连接请求

	// Step 1
	SOCKADDR *pAddr = NULL;
	int nAddrLen = sizeof(SOCKADDR_IN);
	if(nAddressFamily == AF_INET)
		pAddr = (SOCKADDR*)new SOCKADDR_IN;
	else
	{
		pAddr = (SOCKADDR*)new SOCKADDR_IN6;
		nAddrLen = sizeof(SOCKADDR_IN6);
	}

	SockaddrFromHost(lpszTargetHostAddress, uTargetPort, (SOCKADDR*)pAddr);

	SOCKET sock = INVALID_SOCKET;
	int iBound = SOCKET_ERROR;
	BOOL bNeedAttachIOCP = FALSE;
	if(pKeepTcpConnCtx == NULL || pKeepTcpConnCtx->GetSocket() == INVALID_SOCKET)
	{
		//Step 2 - Create Socket
		sock = WSASocket(nAddressFamily, SOCK_STREAM, IPPROTO_TCP, 0, 0, WSA_FLAG_OVERLAPPED);
		if(sock == INVALID_SOCKET)
		{
			delete pAddr;
			return -1;
		}

		//Step 3 - Bind
		if(nAddressFamily == AF_INET6)
		{
			SOCKADDR_IN6 sockAddrBind;
			memset(&sockAddrBind, 0, sizeof(sockAddrBind));
			sockAddrBind.sin6_family = AF_INET6;
			sockAddrBind.sin6_addr = in6addr_any;
			iBound = ::bind(sock, (SOCKADDR*)&sockAddrBind, sizeof(sockAddrBind));
		}
		else
		{
			SOCKADDR_IN sockAddrBind;
			memset(&sockAddrBind, 0, sizeof(sockAddrBind));
			sockAddrBind.sin_family = AF_INET;
			sockAddrBind.sin_port = 0;
			iBound = ::bind(sock, (SOCKADDR*)&sockAddrBind, sizeof(sockAddrBind));
		}

		if(pKeepTcpConnCtx)
		{
			pKeepTcpConnCtx->SetSocket(sock);
			bNeedAttachIOCP = TRUE;
		}
	}
	else
	{
		sock = pKeepTcpConnCtx->GetSocket();
	}

	//Step 4 - Obtain ConnectEx function pointer
	GetSocketExtensionFunctions(sock);

	if(_lpfnConnectEx == NULL)
	{
		STXLOGE(_T("[r][i]Error! ConnectEx function is null !"));
		closesocket(sock);
		delete pAddr;
		return -1;
	}

	CSTXIOCPTcpConnectionContext *pTcpConnCtx = pKeepTcpConnCtx;

	if(pTcpConnCtx == NULL)
	{
		//Step 5 - Create and initialize a new CSTXIOCPTcpConnectionContext object
		CSTXIOCPTcpConnectionContext *pNewTcpConnCtx = OnCreateTcpConnectionContext();
		pTcpConnCtx = pNewTcpConnCtx;
		pTcpConnCtx->SetConnectionID(GetNextTcpConnectionID());
		pTcpConnCtx->SetSocket(sock);
		pTcpConnCtx->SetAddressFamily(nAddressFamily);
		pTcpConnCtx->SetTargetHoatName(lpszTargetHostAddress);
		pTcpConnCtx->SetTargetHoatPort(uTargetPort);
		pTcpConnCtx->SetServerParamString(lpszServerParamString);
		pTcpConnCtx->SetServerParam(dwConnectionParam);
	}

	LONG nConnectionID = pTcpConnCtx->GetConnectionID();

	if(pKeepTcpConnCtx == NULL)
	{
		//Step 6

		if(bKeepConnect)
			pTcpConnCtx->ModifyFlags(STXIOCP_SERVER_CONNECTION_FLAG_KEEPCONNECT, 0);

		//Step 7
		LPSTXIOCPCONTEXTKEY pContextKey = new STXIOCPCONTEXTKEY;
		memset(pContextKey, 0, sizeof(STXIOCPCONTEXTKEY));
		_tcscpy_s(pContextKey->szDescription, 32, _T("[Connection Socket]"));
		pContextKey->sock = sock;
		pContextKey->pTcpConnectionContext = pTcpConnCtx;
		pContextKey->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_TCP_CONNECTION;

		//Step 8
		LockConnectionMap();
		m_mapConnections[nConnectionID] = pTcpConnCtx;
		UnlockConnectionMap();

		pTcpConnCtx->SetContextKey(pContextKey);

		//Step 9 - Attach to IOCP
		HANDLE hIOCP = ::CreateIoCompletionPort((HANDLE)sock, m_hIOCP, (ULONG_PTR)pContextKey, 0);

		if(hIOCP == NULL)
		{
			LockConnectionMap();
			m_mapConnections.erase(nConnectionID);
			UnlockConnectionMap();

			delete pContextKey;
			delete pAddr;

			return -1;
		}

	}
	else	//pKeepTcpConnCtx is not NULL
	{
		LPSTXIOCPCONTEXTKEY pContextKey = pKeepTcpConnCtx->GetContextKey();
		pContextKey->sock = sock;
		if(bNeedAttachIOCP)
		{
			//Attach to IOCP
			HANDLE hIOCP = ::CreateIoCompletionPort((HANDLE)sock, m_hIOCP, (ULONG_PTR)pContextKey, 0);
		}
	}


	//Step 10
	NEW_OPERATION(pOperation);

	pOperation->nOpType = STXIOCP_OPERATION_CONNECT;
	pOperation->sock = sock;
	pOperation->dwSubmitTime = GetTickCount();
	pOperation->dwTimeOut = 0;													//对外 TCP 连接的 ConnectEx 操作不需要由程序进行超时检查.
	pOperation->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_TCP_CONNECTION;
	EnqueueOperation(pOperation);

	//Step 11 - Keep Alive
	KeepAlive(sock, 120000, 120000);

	//Step 12
	DWORD dwSent = 0;
	if(_lpfnConnectEx(sock, (SOCKADDR*)pAddr, nAddrLen, NULL, NULL, NULL, &pOperation->ov))
	{
		// 即使该操作立即完成，也不必在此进行处理。
		// 完成端口工作线程会在稍后获得这个操作已经完成的通知。
		delete pAddr;
		return nConnectionID;
	}
	else
	{
		delete pAddr;
		int iResult = WSAGetLastError();
		if(iResult == ERROR_IO_PENDING)
		{
			return nConnectionID;
		}
		else
		{
			closesocket(sock);
			pOperation->MarkCanceled();
			DequeueOperation(pOperation);
			pOperation->Release();
		}
	}
	return -1;
}

LONG CSTXIOCPServer::GetNextTcpConnectionID()
{
	return s_nTcpConnectionIDBase++;
}

CSTXIOCPTcpConnectionContext *CSTXIOCPServer::GetTcpConnectionContext(LONG nConnectionID)
{
	CSTXServerObjectPtr<CSTXIOCPTcpConnectionContext> pTcpConnCtx;
	LockConnectionMap();
	map<LONG, CSTXServerObjectPtr<CSTXIOCPTcpConnectionContext> >::iterator it = m_mapConnections.find(nConnectionID);
	if(it == m_mapConnections.end())
	{
		UnlockConnectionMap();
		return NULL;
	}
	pTcpConnCtx = it->second;
	UnlockConnectionMap();
	return pTcpConnCtx;
}

LONG CSTXIOCPServer::GetTcpConnectionCount()
{
	return (LONG)m_mapConnections.size();
}

BOOL CSTXIOCPServer::SendTcpData(LONG nConnectionID, LPVOID lpData, DWORD cbDataLen, DWORD_PTR dwUserData)
{
	CSTXIOCPTcpConnectionContext *pTcpConnCtx = GetTcpConnectionContext(nConnectionID);
	if(pTcpConnCtx == NULL)
		return FALSE;

	if(!pTcpConnCtx->IsConnected())
	{
		return FALSE;
	}

	IssueTcpConnectionWrite(pTcpConnCtx, lpData, cbDataLen, dwUserData, 0);

	return TRUE;
}

BOOL CSTXIOCPServer::PostCustomOperation(DWORD_PTR dwOperationID, DWORD_PTR dwUserData, DWORD dwTimeOut)
{
	map<DWORD_PTR, LPSTXIOCPOPERATION>::iterator it;
	m_mapCustomOperation.lock(dwOperationID);
	it = m_mapCustomOperation.find(dwOperationID);
	if(it != m_mapCustomOperation.end(dwOperationID))
	{
		m_mapCustomOperation.unlock(dwOperationID);
		STXTRACE(_T("[r][i]There is an operation with the same ID is being processed. Failed to post custom operation."));
		SetLastError(ERROR_ALREADY_EXISTS);
		return FALSE;
	}
	m_mapCustomOperation.unlock(dwOperationID);

	NEW_OPERATION(pOperation);

	pOperation->pBuffer = NULL;
	pOperation->nOpType = STXIOCP_OPERATION_CUSTOM;
	pOperation->sock = INVALID_SOCKET;
	pOperation->dwSubmitTime = GetTickCount();
	pOperation->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_FAKE;
	pOperation->dwTimeOut = dwTimeOut;
	pOperation->dwOperationID = dwOperationID;
	pOperation->dwUserData = dwUserData;
	pOperation->dwCompleteType = STXIOCP_CUSTOM_OPERATION_QUEUED;

	EnqueueOperation(pOperation);
	return TRUE;
}

BOOL CSTXIOCPServer::PostCustomOperationUsingSprcifiedOperationType(UINT nOperationType, DWORD_PTR dwOperationID, DWORD_PTR dwUserData, DWORD_PTR dwUserData2, DWORD dwTimeOut)
{
	map<DWORD_PTR, LPSTXIOCPOPERATION>::iterator it;
	m_mapInternalCustomOperation.lock(dwOperationID);
	it = m_mapInternalCustomOperation.find(dwOperationID);
	if(it != m_mapInternalCustomOperation.end(dwOperationID))
	{
		m_mapInternalCustomOperation.unlock(dwOperationID);
		STXTRACE(_T("[r][i]There is an operation with the same ID is being processed. Failed to post custom operation."));
		SetLastError(ERROR_ALREADY_EXISTS);
		return FALSE;
	}
	m_mapInternalCustomOperation.unlock(dwOperationID);

	NEW_OPERATION(pOperation);

	pOperation->pszSrcFile = _T(__FILE__);
	pOperation->nLine = __LINE__;

	pOperation->pBuffer = NULL;
	pOperation->nOpType = nOperationType;
	pOperation->sock = INVALID_SOCKET;
	pOperation->dwSubmitTime = GetTickCount();
	pOperation->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_FAKE;
	pOperation->dwTimeOut = dwTimeOut;
	pOperation->dwOperationID = dwOperationID;
	pOperation->dwUserData = dwUserData;
	pOperation->dwUserData2 = dwUserData2;
	pOperation->dwCompleteType = STXIOCP_CUSTOM_OPERATION_QUEUED;

	EnqueueOperation(pOperation);
	return TRUE;
}

BOOL CSTXIOCPServer::PostHttpRequestOperation( LPSTXIOCPSERVERHTTPCONTEXT pContext, DWORD dwOperationType )
{
	NEW_OPERATION(pOperation);

	pOperation->pszSrcFile = _T(__FILE__);
	pOperation->nLine = __LINE__;

	pOperation->pBuffer = NULL;
	pOperation->nOpType = STXIOCP_OPERATION_CUSTOM;
	pOperation->sock = INVALID_SOCKET;
	pOperation->dwSubmitTime = GetTickCount();
	pOperation->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_HTTP;
	pOperation->dwTimeOut = 0;
	pOperation->dwOperationID = dwOperationType;
	pOperation->dwUserData = (DWORD_PTR)pContext;
	pOperation->dwCompleteType = STXIOCP_CUSTOM_OPERATION_QUEUED;

	EnqueueOperation(pOperation);

	PostQueuedCompletionStatus(m_hIOCP, 0, (DWORD_PTR)&m_httpContextKey, &pOperation->ov);
	return TRUE;
}


void __stdcall CSTXIOCPServer::HttpDownloadCallback(HINTERNET hInternet,
						DWORD_PTR dwContext,
						DWORD dwInternetStatus,
						LPVOID lpStatusInfo,
						DWORD dwStatusInfoLen)
{

	LPSTXIOCPSERVERHTTPCONTEXT pContext = (LPSTXIOCPSERVERHTTPCONTEXT)dwContext;
	if(pContext == 0)
		return;

	switch(pContext->dwRequestType)
	{
	case HTTP_CONTEXT_CONNECT: // Connection handle
		if (dwInternetStatus == INTERNET_STATUS_HANDLE_CREATED)
		{
			INTERNET_ASYNC_RESULT *pRes = (INTERNET_ASYNC_RESULT *)lpStatusInfo;
			pContext->hConnect = (HINTERNET)pRes->dwResult;
			pContext->dwRequestType = HTTP_CONTEXT_REQUEST;

			pContext->pServer->PostHttpRequestOperation(pContext, HTTP_OPERATION_REQUEST);
		}
		break;
	case HTTP_CONTEXT_REQUEST: // Request handle
		{
			switch(dwInternetStatus)
			{
			case INTERNET_STATUS_HANDLE_CREATED:
				{
					INTERNET_ASYNC_RESULT *pRes = (INTERNET_ASYNC_RESULT *)lpStatusInfo;
					pContext->hRequest = (HINTERNET)pRes->dwResult;

					pContext->pServer->PostHttpRequestOperation(pContext, HTTP_OPERATION_REQUEST_SEND);
				}
				break;
			case INTERNET_STATUS_REQUEST_SENT:
				{
					//DWORD *lpBytesSent = (DWORD*)lpStatusInfo;
				}
				break;
			case INTERNET_STATUS_REQUEST_COMPLETE:
				{
					INTERNET_ASYNC_RESULT *pAsyncRes = (INTERNET_ASYNC_RESULT *)lpStatusInfo;
					if(pAsyncRes)
					{
						if(pAsyncRes->dwError != 0)
						{
							STXTRACELOGE(_T("Error: Code = %d"), pAsyncRes->dwError);
							pContext->pServer->OnUrlDownloadProgressWrapper(pContext, -1);
							pContext->pServer->OnUrlDownloadCleanupWrapper(pContext);
							delete pContext;
							return;
						}
					}

					DWORD statCodeLen = sizeof(DWORD);
					DWORD statCode;

					//Query Response Code
					if(!HttpQueryInfo(pContext->hRequest,
						HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
						&statCode,
						&statCodeLen,
						NULL))
					{
						STXTRACELOGE(_T("Error: HttpQueryInfo failed."));
					}

					if(statCodeLen == sizeof(DWORD))
					{
						pContext->dwHttpResponseCode = statCode;
						if(statCode != 200)
						{
							STXTRACELOGE(_T("Error: HTTP Response Code = %d"), statCode);
							pContext->pServer->OnUrlDownloadProgressWrapper(pContext, -1);
							pContext->pServer->OnUrlDownloadCleanupWrapper(pContext);
							delete pContext;
							return;
						}
						else
						{
							STXTRACELOGE(_T("Info: 200 OK : %s"), pContext->szUrl);
						}
					}


					//pContext->pServer->PostHttpRequestOperation(pContext, HTTP_OPERATION_REQUEST_SEND);

					FillMemory(&pContext->buff, sizeof(pContext->buff), 0);
					pContext->buff.dwStructSize = sizeof(pContext->buff);
					pContext->buff.lpvBuffer = new char[HTTP_DOWNLOAD_BUFFER_SIZE + 1];
					pContext->buff.dwBufferLength = HTTP_DOWNLOAD_BUFFER_SIZE;

					pContext->dwRequestType = HTTP_CONTEXT_DOWNLOAD;

					if (!InternetReadFileEx(pContext->hRequest,
						&pContext->buff,
						0, (DWORD_PTR)pContext))
					{
						if (GetLastError() != ERROR_IO_PENDING)
						{
							STXTRACELOGE(_T("Error: InternetReadFileEx failed."));
							pContext->pServer->OnUrlDownloadProgressWrapper(pContext, -1);
							pContext->pServer->OnUrlDownloadCleanupWrapper(pContext);
							delete pContext;
						}
					}
					else
					{
						if(pContext->buff.dwBufferLength != 0)
						{
							pContext->bufferContent.WriteBuffer(pContext->buff.lpvBuffer, pContext->buff.dwBufferLength);
							pContext->pServer->OnUrlDownloadProgressWrapper(pContext, pContext->buff.dwBufferLength);
							pContext->pServer->PostHttpRequestOperation(pContext, HTTP_OPERATION_REQUEST_READ);
						}
					}
				}
				break;
			case INTERNET_STATUS_RECEIVING_RESPONSE:
				break;
			case INTERNET_STATUS_RESPONSE_RECEIVED:
				break;
			}	
		}
		break;
	case HTTP_CONTEXT_DOWNLOAD: // Download handle
		{
			if (dwInternetStatus == INTERNET_STATUS_REQUEST_COMPLETE)
			{
				pContext->bufferContent.WriteBuffer(pContext->buff.lpvBuffer, pContext->buff.dwBufferLength);
				pContext->pServer->OnUrlDownloadProgressWrapper(pContext, pContext->buff.dwBufferLength);

				if(pContext->buff.dwBufferLength > 0)
				{
					pContext->pServer->PostHttpRequestOperation(pContext, HTTP_OPERATION_REQUEST_READ);
				}
				else
				{
					pContext->pServer->PostHttpRequestOperation(pContext, HTTP_OPERATION_REQUEST_FINISHED);
				}
			}
			break;
		}
	}
}

BOOL CSTXIOCPServer::AddFolderMonitorIgnoreFileExtension(LONGLONG nFolderMonitorID, LPCTSTR lpszExt)
{
	auto it = m_mapIDtoMonitoredDir.find(nFolderMonitorID);
	if (it == m_mapIDtoMonitoredDir.end(nFolderMonitorID))
	{
		return FALSE;
	}

	it->second->pFolderMonitorContext->AddIgnoreFileExtension(lpszExt);

	return TRUE;
}

BOOL CSTXIOCPServer::RemoveFolderMonitorIgnoreFileExtension(LONGLONG nFolderMonitorID, LPCTSTR lpszExt)
{
	auto it = m_mapIDtoMonitoredDir.find(nFolderMonitorID);
	if (it == m_mapIDtoMonitoredDir.end(nFolderMonitorID))
	{
		return FALSE;
	}

	it->second->pFolderMonitorContext->RemoveIgnoreFileExtension(lpszExt);

	return TRUE;
}

void CSTXIOCPServer::RaiseCustomException(DWORD dwExceptionCode, DWORD nArguments, ULONG_PTR *pArguments)
{
	if ((dwExceptionCode & (1 << 29)) == 0)
	{
		STXTRACELOGE(_T("RaiseCustomException error: the exception code 0x%X is not a user defined exception. [Must contain 0x20000000 bit]"), dwExceptionCode);
		return;
	}

	if (nArguments > EXCEPTION_MAXIMUM_PARAMETERS - 1)
	{
		STXTRACELOGE(_T("RaiseCustomException error: too many arguments for exception 0x%X. %d arguments allowed at maximum."), dwExceptionCode, EXCEPTION_MAXIMUM_PARAMETERS - 1);
		return;
	}

	ULONG_PTR pArgumentsCopy[EXCEPTION_MAXIMUM_PARAMETERS];
	pArgumentsCopy[0] = (ULONG_PTR)this;
	for (DWORD i = 0; i < nArguments; i++)
	{
		pArgumentsCopy[i + 1] = pArguments[i];
	}
	RaiseException(dwExceptionCode, 0, nArguments + 1, pArgumentsCopy);
}

BOOL CSTXIOCPServer::BeginDownloadURL(LPCTSTR lpszServer, WORD wPort, LPCTSTR lpszURL, DWORD_PTR dwUserData, LPCTSTR lpszUserDataString)
{
	HINTERNET hInstance = InternetOpen(_T("asynchttp"), 
		INTERNET_OPEN_TYPE_PRECONFIG,
		NULL,
		NULL,
		INTERNET_FLAG_ASYNC); // ASYNC Flag

	if (hInstance == NULL)
	{
		STXTRACELOGE(_T("Error: InternetOpen failed!"));
		return FALSE;
	}

	// Setup callback function
	if (InternetSetStatusCallback(hInstance,
		(INTERNET_STATUS_CALLBACK)&CSTXIOCPServer::HttpDownloadCallback) == INTERNET_INVALID_STATUS_CALLBACK)
	{
		InternetCloseHandle(hInstance);
		STXTRACELOGE(_T("Error: InternetSetStatusCallback failed."));
		return FALSE;
	}

	STXIOCPSERVERHTTPCONTEXT *pContext = new STXIOCPSERVERHTTPCONTEXT();
	pContext->pServer = this;
	pContext->hInternet = hInstance;
	pContext->hConnect = 0;
	pContext->hRequest = 0;
	pContext->dwRequestType = HTTP_CONTEXT_CONNECT;
	pContext->dwUserData = dwUserData;
	_tcscpy_s(pContext->szUrl, lpszURL);
	pContext->stringParam = lpszUserDataString ? lpszUserDataString : _T("");

	// First call that will actually complete asynchronously even
	// though there is no network traffic
	HINTERNET hConnect = InternetConnect(hInstance, 
		lpszServer, 
		wPort,
		NULL,
		NULL,
		INTERNET_SERVICE_HTTP,
		0,
		(DWORD_PTR)pContext); // Connection handle's Context

	if (hConnect == NULL)
	{
		if (GetLastError() != ERROR_IO_PENDING)
		{
			STXTRACELOGE(_T("Error: InternetConnect failed : %s"), lpszServer);
			return FALSE;
		}
	}

	return TRUE;
}


LONGLONG CSTXIOCPServer::MonitorFolder(LPCTSTR lpszFolder, DWORD dwNotifyFilter, LPOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
	static LONGLONG monitorIDBase = 0;

	HANDLE hDir = CreateFile(lpszFolder,
		FILE_LIST_DIRECTORY, 
		FILE_SHARE_READ | FILE_SHARE_WRITE ,//| FILE_SHARE_DELETE, <-- removing FILE_SHARE_DELETE prevents the user or someone else from renaming or deleting the watched directory. This is a good thing to prevent.
		NULL, //security attributes
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | //<- the required priviliges for this flag are: SE_BACKUP_NAME and SE_RESTORE_NAME.  CPrivilegeEnabler takes care of that.
		FILE_FLAG_OVERLAPPED, //OVERLAPPED!
		NULL);

	if( hDir == INVALID_HANDLE_VALUE )
	{
		DWORD dwError = GetLastError();
		STXTRACELOGE(_T("[r][g][i]Couldn't open directory for monitoring. Err=%d  %s"), dwError, lpszFolder);
		::SetLastError(dwError);//who knows if TRACE will cause GetLastError() to return success...probably won't, but set it manually just for fun.
		return -1;
	}

	LPSTXIOCPCONTEXTKEY pNewContextKey = new STXIOCPCONTEXTKEY;

	if (pNewContextKey == NULL)
	{
		STXTRACELOGE(_T("[r][i]Failed to allocate LPSTXIOCPCONTEXTKEY for MonitorFolder"));
		CloseHandle(hDir);
		return -1;
	}

	memset(pNewContextKey, 0, sizeof(STXIOCPCONTEXTKEY));
	_tcscpy_s(pNewContextKey->szDescription, 32, _T("[Monitored Dir]"));
	pNewContextKey->hMonitoredDir = hDir;
	pNewContextKey->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_MONITORED_DIR;
	_tcscpy_s(pNewContextKey->szMonitoredFolder, MAX_PATH, lpszFolder);
	pNewContextKey->dwMonitorNotifyFilter = dwNotifyFilter;

	m_mapMonitoredDir.insertValue(hDir, pNewContextKey);
	HANDLE hIOCP = ::CreateIoCompletionPort((HANDLE)hDir, m_hIOCP, (ULONG_PTR)pNewContextKey, 0);
	if(hIOCP == NULL)	//Failed to bind to IOCP
	{
		STXTRACELOGE(_T("[r][i]Failed to bind the Client Socket to IOCP"));
		delete pNewContextKey;
		CloseHandle(hDir);
		m_mapMonitoredDir.erase(hDir);
		return -1;
	}

	CSTXIOCPFolderMonitorContext *pContext = new CSTXIOCPFolderMonitorContext();
	if (pContext == NULL)
	{
		STXTRACELOGE(_T("[r][i]Failed to allocate CSTXIOCPFolderMonitorContext"));
		delete pNewContextKey;
		CloseHandle(hDir);
		m_mapMonitoredDir.erase(hDir);
		return -1;
	}

	LONGLONG newMonitorID = monitorIDBase++;

	pContext->m_nMonitorID = newMonitorID;
	m_mapIDtoMonitoredDir.insertValue(newMonitorID, pNewContextKey);

	pNewContextKey->pFolderMonitorContext = pContext;
	pContext->Release();

	NEW_OPERATION(pOperation);

	pOperation->pszSrcFile = _T(__FILE__);
	pOperation->nLine = __LINE__;

	DWORD dwFolderMonitorBufferSize = STXIOCP_FOLDER_MONITOR_BUFFER_SIZE;
	if (m_BaseServerInfo.dwFolderMonitorBufferSize > 1024)
	{
		dwFolderMonitorBufferSize = m_BaseServerInfo.dwFolderMonitorBufferSize;
	}

	pOperation->pBuffer = NULL;
	pOperation->nOpType = STXIOCP_OPERATION_MONITOR;
	pOperation->hMonitoredDir = hDir;
	pOperation->pMonitorBuffer = new char[dwFolderMonitorBufferSize];
	pOperation->nMonitorBufferLength = dwFolderMonitorBufferSize;
	pOperation->lpCompletionRoutine = lpCompletionRoutine;
	pOperation->sock = INVALID_SOCKET;
	pOperation->dwSubmitTime = GetTickCount();
	pOperation->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_MONITORED_DIR;
	pOperation->dwTimeOut = 0;
	pOperation->dwOperationID = 0;
	pOperation->dwUserData = 0;
	pOperation->dwCompleteType = 0;

	EnqueueOperation(pOperation);

	BOOL bMonitored = ReadDirectoryChangesW(hDir, pOperation->pMonitorBuffer, pOperation->nMonitorBufferLength, TRUE,
		dwNotifyFilter, NULL, &pOperation->ov, NULL);

	if(!bMonitored)
	{
		TCHAR szError[256];
		CSTXLog::GetLastErrorText(szError, 256);

		DequeueOperation(pOperation);
		pOperation->Release();	//pOperation->pMonitorBuffer is deleted in destructor
		CloseHandle(hDir);
		STXTRACELOGE(_T("[r][g][i]ReadDirectoryChangesW Failed @ CSTXIOCPServer::MonitorFolder : %s"), szError);
		m_mapMonitoredDir.erase(hDir);
		return -1;
	}

	EnterCriticalSection(&m_csMonitoredDirSet);
	m_setMonitoredDir.insert(hDir);
	LeaveCriticalSection(&m_csMonitoredDirSet);


	ULONGLONG tick = GetTickCount64();
	size_t nBaseFolderLength = _tcslen(lpszFolder);
	std::wstring fileFilter = lpszFolder;
	fileFilter += _T("*.*");
	EnumFiles(fileFilter.c_str(), [=](LPCTSTR lpszFile, DWORD_PTR)
	{
		FILETIME fTime;
		memset(&fTime, 0, sizeof(fTime));
		GetFileLastModifyTime(lpszFile, &fTime);

		pContext->_fileLastModifyTime.insertValue(lpszFile + nBaseFolderLength, fTime);
		return TRUE;
	}, 0);

	STXTRACELOGE(3, _T("%d file Modify/Write time loaded for %s. Time Elapsed: %I64dms"), pContext->_fileLastModifyTime.size(), fileFilter.c_str(), GetTickCount64() - tick);


	return newMonitorID;
}

BOOL CSTXIOCPServer::GetFileLastModifyTime(LPCTSTR lpszFile, LPFILETIME lpFileTime)
{
	BOOL bResult = FALSE;
	HANDLE hFile = CreateFile(lpszFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile != INVALID_HANDLE_VALUE)
	{
		if (GetFileTime(hFile, NULL, NULL, lpFileTime))
		{
			bResult = TRUE;
		}
		CloseHandle(hFile);
	}
	return bResult;
}

void CSTXIOCPServer::EnumFiles(LPCTSTR lpszFilter, const std::function<BOOL(LPCTSTR, DWORD_PTR)>& pfnEnumFunc, DWORD_PTR dwUserData)
{
	if (pfnEnumFunc == NULL)
		return;

	TCHAR szStartPath[MAX_PATH];
	size_t nLen = 0;
	LPCTSTR pszLastFlash = _tcsrchr(lpszFilter, _T('\\'));
	LPCTSTR pszFilterOnly = _T("*");
	if (pszLastFlash != NULL)
	{
		pszFilterOnly = pszLastFlash + 1;
		_tcsncpy_s(szStartPath, MAX_PATH, lpszFilter, pszFilterOnly - lpszFilter);
		nLen = _tcslen(szStartPath);
	}
	else
	{
		_tcscpy_s(szStartPath, MAX_PATH, lpszFilter);
		nLen = _tcslen(szStartPath);
		if (nLen > 0 && szStartPath[nLen - 1] != _T('\\'))
		{
			szStartPath[nLen] = _T('\\');
			nLen++;
		}
	}

	TCHAR szTempPathName[MAX_PATH];
	_tcscpy_s(szTempPathName, MAX_PATH, szStartPath);
	LPTSTR pszTempPathNameFileName = szTempPathName + nLen;


	WIN32_FIND_DATA FindFileData;
	HANDLE hFind = INVALID_HANDLE_VALUE;

	TCHAR szStartFilter[MAX_PATH];
	_tcscpy_s(szStartFilter, MAX_PATH, szStartPath);
	_tcscat_s(szStartFilter, MAX_PATH, pszFilterOnly);
	TCHAR szSubFilter[MAX_PATH];

	// Find the first file in the directory.
	hFind = FindFirstFile(szStartFilter, &FindFileData);

	if (hFind == INVALID_HANDLE_VALUE)
	{
		_tprintf(TEXT("Invalid file handle. Error is %u.\n"), GetLastError());
		return;
	}
	else
	{
		if (FindFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			if (_tcscmp(FindFileData.cFileName, _T(".")) != 0 && _tcscmp(FindFileData.cFileName, _T("..")))
			{
				_tcscpy_s(szSubFilter, MAX_PATH, szStartPath);
				_tcscat_s(szSubFilter, MAX_PATH, FindFileData.cFileName);
				_tcscat_s(szSubFilter, MAX_PATH, _T("\\"));
				_tcscat_s(szSubFilter, MAX_PATH, pszFilterOnly);
				EnumFiles(szSubFilter, pfnEnumFunc, dwUserData);
			}
		}
		else
		{
			_tcscpy_s(pszTempPathNameFileName, MAX_PATH - nLen, FindFileData.cFileName);
			if (!pfnEnumFunc(szTempPathName, dwUserData))
			{
				FindClose(hFind);
				return;
			}
		}

		// List all the other files in the directory.
		while (FindNextFile(hFind, &FindFileData) != 0/* && !g_bTerminateEnum*/)
		{
			if (FindFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				if (_tcscmp(FindFileData.cFileName, _T(".")) != 0 && _tcscmp(FindFileData.cFileName, _T("..")))
				{
					_tcscpy_s(szSubFilter, MAX_PATH, szStartPath);
					_tcscat_s(szSubFilter, MAX_PATH, FindFileData.cFileName);
					_tcscat_s(szSubFilter, MAX_PATH, _T("\\"));
					_tcscat_s(szSubFilter, MAX_PATH, pszFilterOnly);
					EnumFiles(szSubFilter, pfnEnumFunc, dwUserData);
				}
			}
			else
			{
				_tcscpy_s(pszTempPathNameFileName, MAX_PATH - nLen, FindFileData.cFileName);
				if (!pfnEnumFunc(szTempPathName, dwUserData))
				{
					FindClose(hFind);
					return;
				}
			}
		}
		FindClose(hFind);
	}
}

BOOL CSTXIOCPServer::DoCustomOperation(DWORD_PTR dwOperationID, DWORD dwCompleteType)
{
	map<DWORD_PTR, LPSTXIOCPOPERATION>::iterator it;
	m_mapCustomOperation.lock(dwOperationID);
	it = m_mapCustomOperation.find(dwOperationID);
	if(it == m_mapCustomOperation.end(dwOperationID))
	{
		m_mapCustomOperation.unlock(dwOperationID);
		return FALSE;
	}
	if(it->second->dwCompleteType != STXIOCP_CUSTOM_OPERATION_QUEUED)
	{
		m_mapCustomOperation.unlock(dwOperationID);
		return FALSE;
	}
	it->second->dwCompleteType = dwCompleteType;
	PostQueuedCompletionStatus(m_hIOCP, 0, NULL, &it->second->ov);
	m_mapCustomOperation.unlock(dwOperationID);

	return FALSE;
}

LONG CSTXIOCPServer::SendUdpData(UINT uUdpSocketPort, LPVOID lpData, DWORD cbDataLen, LPCTSTR lpszTargetHostAddress, UINT uTargetPort, int nAddressFamily)
{
	if(((m_BaseServerInfo.dwServerFlags & STXSF_IPV6) == 0) && nAddressFamily == AF_INET6)
	{
		SetLastError(ERROR_NOT_SUPPORTED);
		return -1;
	}

	LockServersMap();
	map<UINT, CSTXServerObjectPtr<CSTXIOCPUdpServerContext> >::iterator it = m_mapUdpServers.find(uUdpSocketPort);
	if(it == m_mapUdpServers.end())
	{
		UnlockServersMap();
		return -1;
	}

	SOCKET sock = INVALID_SOCKET;
	if(nAddressFamily == AF_INET6)
		sock = it->second->GetSocket6();
	else
		sock = it->second->GetSocket();

	UnlockServersMap();
	
	CSTXIOCPBuffer *pBuffer = m_Buffers.GetBuffer(20);

	if(pBuffer == NULL)
		return -1;

	DWORD dwFlags = 0;

	NEW_OPERATION(pOperation);

	pOperation->pszSrcFile = _T(__FILE__);
	pOperation->nLine = __LINE__;

	pOperation->pBuffer = pBuffer;
	pOperation->nOpType = STXIOCP_OPERATION_WRITE;
	pOperation->sock = sock;
	pOperation->dwSubmitTime = GetTickCount();
	pOperation->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_UDPSERVER;
	pOperation->dwTimeOut = 0;													// UDP Server 的写操作不需要进行超时检查
	pOperation->nIPVer = (nAddressFamily == AF_INET6 ? STXIOCP_SOCKET_IPV6 : STXIOCP_SOCKET_IPV4);

	DWORD dwWrite = pBuffer->WriteBuffer(lpData, cbDataLen);

	WSABUF buf;
	buf.buf = (char*)pBuffer->GetBufferPtr();
	buf.len = dwWrite;

	DWORD dwSent = 0;

	SOCKADDR *pAddr = NULL;
	int nAddrLen = sizeof(SOCKADDR_IN);
	if(nAddressFamily == AF_INET)
		pAddr = (SOCKADDR*)new SOCKADDR_IN;
	else
	{
		pAddr = (SOCKADDR*)new SOCKADDR_IN6;
		nAddrLen = sizeof(SOCKADDR_IN6);
	}

	SockaddrFromHost(lpszTargetHostAddress, uTargetPort, pAddr, nAddressFamily);

	//Queued
	EnqueueOperation(pOperation);

	int iResult = WSASendTo(sock, &buf, 1, &dwSent, dwFlags, (SOCKADDR*)pAddr, nAddrLen, &pOperation->ov, NULL);
	int iWsaError = WSAGetLastError();

	delete pAddr;

	if(iResult == 0 && iWsaError == 0)	//Succeed Immediately
	{
		// 即使该操作立即完成，也不必在此进行处理。
		// 完成端口工作线程会在稍后获得这个操作已经完成的通知。
		return dwSent;
	}
	else
	{
		if(iWsaError == ERROR_IO_PENDING)
		{
			return TRUE;
		}

		closesocket(sock);
		pOperation->MarkCanceled();
		m_Buffers.ReleaseBuffer(pBuffer);
		
		DequeueOperation(pOperation);
		pOperation->Release();
	}
	return FALSE;
}

LPSTXIOCPCONTEXTKEY CSTXIOCPServer::Accept(SOCKET sockClient, tr1::shared_ptr<CSTXIOCPTcpServerContext> pServerContext, LPVOID lpAddrBuffer, CSTXIOCPServerClientContext **ppContextContext, LPSTXIOCPOPERATION pOperation)
{
	if (pServerContext->IsReachedMaximumClient())
	{
		STXTRACELOGE(_T("[r][i]Tcp Server <%s> has reached maximum client count %I64d. rejecting incoming client now!"), pServerContext->GetServerParamString().c_str(), pServerContext->GetClientCountLimit());
		closesocket(sockClient);
		return NULL;
	}

	pServerContext->IncreaseClientCount();

	// 1. 创建一个 STXIOCPCONTEXTKEY 为完成端口使用
	// 2. 引发 OnCreateClientContext 事件以创建一个 CSTXIOCPServerClientContext 对象与此客户端连接相对应
	// 3. 将 socket 与完成端口关联起来
	// 4. 读出客户 socket 的地址信息并保存到 CSTXIOCPServerClientContext 中
	// 5. 引发 CSTXIOCPServerClientContext::OnInitialize 事件
	// 6. 将此连接的信息记录到所有客户连接的map中
	// 7. 引发 OnNewClientContext 事件
	// 8. 将 CSTXIOCPServerClientContext 写入输出参数

	//Step 1 - Allocate ContextKey
	LPSTXIOCPCONTEXTKEY pNewContextKey = new STXIOCPCONTEXTKEY;
	memset(pNewContextKey, 0, sizeof(STXIOCPCONTEXTKEY));
	_tcscpy_s(pNewContextKey->szDescription, 32, _T("[Client Socket]"));
	pNewContextKey->sock = sockClient;
	pNewContextKey->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_CLIENT;
	pNewContextKey->pTcpServerContext = pServerContext;

	//Step 2 - Create a ClientContext for the socket
	CSTXIOCPServerClientContext *pClientContext = OnCreateClientContext(pServerContext);
	if(pClientContext == NULL)
	{
		delete pNewContextKey;
		if(ppContextContext)
			*ppContextContext = NULL;
		STXTRACELOGE(_T("[r][i]Failed to Create Client-Context!   OnCreateClientContext Failed!"));
		pServerContext->DecreaseClientCount();
		return NULL;
	}
	pClientContext->m_dwOperationTimeout = m_BaseServerInfo.dwDefaultOperationTimeout;
	pClientContext->m_pServer = this;
	pClientContext->m_socket = sockClient;
	pClientContext->m_socketCopy = sockClient;
	pClientContext->m_pServerContext = pServerContext;
	pNewContextKey->pClientContext = pClientContext;
	pClientContext->Release();	//

	//Step 4 - Get the address of the socket, save to the client context
	if(pOperation->nIPVer == STXIOCP_SOCKET_IPV6)
	{
		SOCKADDR_IN6 *pLocalAddr, *pRemoteAddr;
		int nLocalAddrLen, nRemoteAddrLen;
		GetAcceptExSockaddrs((LPVOID)lpAddrBuffer, 0, sizeof(SOCKADDR_IN6) + 16, sizeof(SOCKADDR_IN6) + 16
			,(SOCKADDR**)&pLocalAddr, &nLocalAddrLen, (SOCKADDR**)&pRemoteAddr, &nRemoteAddrLen); 

		memcpy(&pClientContext->m_addrLocal6, pLocalAddr, sizeof(SOCKADDR_IN6));
		memcpy(&pClientContext->m_addrRemote6, pRemoteAddr, sizeof(SOCKADDR_IN6));
		pClientContext->SetAddressFamily(AF_INET6);
	}
	else
	{
		SOCKADDR_IN *pLocalAddr, *pRemoteAddr;
		int nLocalAddrLen, nRemoteAddrLen;
		GetAcceptExSockaddrs((LPVOID)lpAddrBuffer, 0, sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16
			,(SOCKADDR**)&pLocalAddr, &nLocalAddrLen, (SOCKADDR**)&pRemoteAddr, &nRemoteAddrLen); 

		memcpy(&pClientContext->m_addrLocal, pLocalAddr, sizeof(SOCKADDR_IN));
		memcpy(&pClientContext->m_addrRemote, pRemoteAddr, sizeof(SOCKADDR_IN));
		pClientContext->SetAddressFamily(AF_INET);
	}

	//Step 5
	pClientContext->OnInitialize();

	LPCTSTR lpszClientIp = pClientContext->GetClientIP();
	//Step 6 - Add to ClientContextMap
	m_mapClientContext.lock(lpszClientIp);
	if(m_mapClientContext.find(lpszClientIp) == m_mapClientContext.end(lpszClientIp))
		m_mapClientContext[lpszClientIp] = pClientContext;
	else
	{
		STXTRACELOGE(_T("The socket is already assigned to a ClientContext : %s"), lpszClientIp);
	}

	//Step 7
	//OnClientContextInitialized(pClientContext);

	//Step 8
	if(ppContextContext)
		*ppContextContext = pClientContext;

	//Step 3 - Bind to IOCP
	HANDLE hIOCP = ::CreateIoCompletionPort((HANDLE)pNewContextKey->sock, m_hIOCP, (ULONG_PTR)pNewContextKey, 0);
	if(hIOCP == NULL)	//Failed to bind to IOCP
	{
		pClientContext->AddRef();
		m_mapClientContext.erase(lpszClientIp);

		m_mapClientContext.unlock(lpszClientIp);

		if(ppContextContext)
			*ppContextContext = NULL;

		pClientContext->Release();
		STXTRACELOGE(_T("[r][i]Failed to bind the Client Socket to IOCP"));
		//pServerContext->DecreaseClientCount();		//done in pClientContext->Release();
		return NULL;
	}

	m_mapClientContext.unlock(lpszClientIp);
	return pNewContextKey;
}

BOOL CSTXIOCPServer::IssueAccept(LPSTXIOCPCONTEXTKEY lpListenSockContextKey, DWORD dwAcceptFlags, SOCKET sockReuse4, SOCKET sockReuse6)
{
	SOCKET sock4 = INVALID_SOCKET, sock6 = INVALID_SOCKET;
	SOCKET *pSock4 = NULL, *pSock6 = NULL;
	if(dwAcceptFlags & STXIOCP_ACCEPT_IPV4)
		pSock4 = &sock4;
	if((m_BaseServerInfo.dwServerFlags & STXSF_IPV6) && (dwAcceptFlags & STXIOCP_ACCEPT_IPV6))
		pSock6 = &sock6;

	if(!CreateSocket(pSock4, pSock6, SOCK_STREAM))
	{
		STXTRACELOGE(_T("[r][i]Failed to Create a socket to be Accepted!"));
		return FALSE;
	}

	tr1::shared_ptr<CSTXIOCPTcpServerContext> pServerContext = lpListenSockContextKey->pTcpServerContext;
	char *pszOutputBuffer = NULL;
	DWORD dwByteReceived = NULL;

	// IPv4
	if(pSock4)
	{
		pszOutputBuffer = new char[m_BaseServerInfo.dwAcceptBufferSize];
		dwByteReceived = m_BaseServerInfo.dwAcceptBufferSize;
		NEW_OPERATION(pOperation);

		pOperation->pszSrcFile = _T(__FILE__);
		pOperation->nLine = __LINE__;

		pOperation->sockNewAccept = sock4;
		pOperation->pszOutputBuffer = pszOutputBuffer;

		pOperation->nOpType = STXIOCP_OPERATION_ACCEPT;
		pOperation->sock = lpListenSockContextKey->sock;
		pOperation->dwSubmitTime = GetTickCount();
		pOperation->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_TCPSERVER;
		pOperation->dwTimeOut = 0;													//TCP Server 的 Accept 操作不需要进行超时检查
		pOperation->nIPVer = STXIOCP_SOCKET_IPV4;

		//Queued
		EnqueueOperation(pOperation);

		//Issue an Accept operation

		BOOL bAccepted = ::AcceptEx(lpListenSockContextKey->sock, sock4, pszOutputBuffer, 0, sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, &dwByteReceived, &pOperation->ov);
		if(WSAGetLastError() != ERROR_IO_PENDING && !bAccepted)
		{
			delete []pszOutputBuffer;
			if(pSock4)
				closesocket(sock4);
			if(pSock6)
				closesocket(sock6);
			pOperation->MarkCanceled();
			DequeueOperation(pOperation);
			pOperation->Release();

			//IssueAccept(lpListenSockContextKey, dwAcceptFlags, sockReuse4, sockReuse6);

			return FALSE;
		}
		InterlockedIncrement(&lpListenSockContextKey->nAcceptPending);
	}

	// IPv6
	if((m_BaseServerInfo.dwServerFlags & STXSF_IPV6) && pSock6)
	{
		pszOutputBuffer = new char[m_BaseServerInfo.dwAcceptBufferSize];
		dwByteReceived = m_BaseServerInfo.dwAcceptBufferSize;

		NEW_OPERATION(pOperation6);

		pOperation6->pszSrcFile = _T(__FILE__);
		pOperation6->nLine = __LINE__;

		pOperation6->sockNewAccept = sock6;
		pOperation6->pszOutputBuffer = pszOutputBuffer;

		pOperation6->nOpType = STXIOCP_OPERATION_ACCEPT;
		pOperation6->sock = lpListenSockContextKey->sock;
		pOperation6->dwSubmitTime = GetTickCount();
		pOperation6->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_TCPSERVER;
		pOperation6->dwTimeOut = 0;													//TCP Server 的 Accept 操作不需要进行超时检查

		pOperation6->nIPVer = STXIOCP_SOCKET_IPV6;

		EnqueueOperation(pOperation6);

		//Issue an Accept operation
		::AcceptEx(lpListenSockContextKey->sock6, sock6, pszOutputBuffer, 0, sizeof(SOCKADDR_IN6) + 16, sizeof(SOCKADDR_IN6) + 16, &dwByteReceived, &pOperation6->ov);
		if(WSAGetLastError() != ERROR_IO_PENDING && WSAGetLastError() != NOERROR)
		{
			TCHAR szError[256];
			CSTXLog::GetLastErrorText(szError, 256);
			STXTRACELOGE(_T("[r][i]AcceptEx (IPv6) Failed! %s"), szError);
			delete []pszOutputBuffer;
			if(pSock4)
				closesocket(sock4);
			if(pSock6)
				closesocket(sock6);
			pOperation6->MarkCanceled();
			DequeueOperation(pOperation6);
			pOperation6->Release();

			return FALSE;
		}
		InterlockedIncrement(&lpListenSockContextKey->nAcceptPending);
	}
	return TRUE;
}

BOOL CSTXIOCPServer::SetBufferConfig(LONG nBufferSize /* = STXIOCPSERVER_BUFFER_SIZE */, LONG nInitBufferCount /* = STXIOCPSERVER_BUFFER_COUNT */, LONG nMaxBufferCount /* = STXIOCPSERVER_BUFFER_MAXCOUNT */)
{
	m_BaseServerInfo.dwBufferSize = nBufferSize;
	m_BaseServerInfo.dwBufferInitialCount = nInitBufferCount;
	m_BaseServerInfo.dwBufferMaxCount = nMaxBufferCount;
	return TRUE;
}

void CSTXIOCPServer::LockConnectionMap()
{
	EnterCriticalSection(&m_csConnections);
}

void CSTXIOCPServer::UnlockConnectionMap()
{
	LeaveCriticalSection(&m_csConnections);
}

void CSTXIOCPServer::DisconnectClient( CSTXIOCPServerClientContext *pClientContext )
{
	if(pClientContext->IsDisconnected())
		return;

	if(!IssueClientDisconnectEx(pClientContext, 0))
	{
		STXTRACELOGE(_T("[r][i]IssueClientDisconnectEx failed!"));
	}
}

LPVOID CSTXIOCPServer::GetThreadUserStorage()
{
	return g_pThreadTlsUserData;
}

LONGLONG CSTXIOCPServer::GetTimeSpanSeconds(SYSTEMTIME &tmNow, SYSTEMTIME & tmPrev)
{
	union FileTimeUnion
	{
		FILETIME fileTime;
		ULARGE_INTEGER ul;
	};

	LONGLONG        llSecond = 0;
	FileTimeUnion   ftNow;
	FileTimeUnion   ftPrev;

	SystemTimeToFileTime(&tmNow, &ftNow.fileTime);
	SystemTimeToFileTime(&tmPrev, &ftPrev.fileTime);

	llSecond = (ftNow.ul.QuadPart - ftPrev.ul.QuadPart);
	llSecond /= 10000; ///< ms counter
	llSecond /= 1000; ///< second counter

	return llSecond;
}

BOOL CSTXIOCPServer::DisconnectClientByTimeout(CSTXIOCPServerClientContext *pClientContext)
{
	if (pClientContext->IsDisconnected())
		return FALSE;

	SYSTEMTIME tmNow;
	GetLocalTime(&tmNow);
	LONGLONG nTimeDiffSec = GetTimeSpanSeconds(tmNow, pClientContext->_lastDataReceiveTime);
	DWORD dwTimeout = OnGetClientReceiveTimeOut(pClientContext);
	if (nTimeDiffSec * 1000 < dwTimeout)
	{
		STXTRACELOGE(_T("[r][g]Tcp Client [sock:%d, ip:%s] timeout timer triggered but not a real timeout! %I64d seconds elapsed since last recv time : %.4d-%.2d-%.2d %.2d:%.2d:%.2d.%.3d"), pClientContext->GetSocket(), pClientContext->GetClientIP()
			, nTimeDiffSec, pClientContext->_lastDataReceiveTime.wYear, pClientContext->_lastDataReceiveTime.wMonth, pClientContext->_lastDataReceiveTime.wDay
			, pClientContext->_lastDataReceiveTime.wHour, pClientContext->_lastDataReceiveTime.wMinute, pClientContext->_lastDataReceiveTime.wSecond, pClientContext->_lastDataReceiveTime.wMilliseconds);
		return FALSE;
	}

	pClientContext->_timeOutDisconnected = TRUE;
	if (pClientContext->_lastDataReceiveTime.wYear == 1800)		//1800 means no package received because wYear is initialized 1800 at ClientContext constructor
	{
		STXTRACELOGE(_T("[r][g]Tcp Client [sock:%d, ip:%s] operation timeout! received no package after %d seconds!"), pClientContext->GetSocket(), pClientContext->GetClientIP()
			, dwTimeout / 1000);
	}
	else
	{
		STXTRACELOGE(_T("[r][g]Tcp Client [sock:%d, ip:%s] operation timeout! %I64d seconds elapsed since last recv time : %.4d-%.2d-%.2d %.2d:%.2d:%.2d.%.3d"), pClientContext->GetSocket(), pClientContext->GetClientIP()
			, nTimeDiffSec, pClientContext->_lastDataReceiveTime.wYear, pClientContext->_lastDataReceiveTime.wMonth, pClientContext->_lastDataReceiveTime.wDay
			, pClientContext->_lastDataReceiveTime.wHour, pClientContext->_lastDataReceiveTime.wMinute, pClientContext->_lastDataReceiveTime.wSecond, pClientContext->_lastDataReceiveTime.wMilliseconds);
	}

	if (!IssueClientDisconnectEx(pClientContext, 0))
	{
		STXTRACELOGE(_T("[r][i]IssueClientDisconnectEx failed!"));
	}

	return TRUE;
}

void CSTXIOCPServer::WaitForServerUnlock()
{
	::WaitForSingleObject(m_hServerUnlockEvent, INFINITE);
}

LONG CSTXIOCPServer::SendClientData( CSTXIOCPServerClientContext *pClientContext, LPVOID lpData, DWORD cbDataLen, DWORD_PTR dwUserData /*= 0*/ )
{
	LONG nSent = IssueClientWrite(pClientContext, lpData, cbDataLen, dwUserData, m_BaseServerInfo.dwDefaultOperationTimeout);
	if(nSent < 0)
	{
		//STXTRACE(_T("[i][r][g]CSTXIOCPServerClientContext::Send  Failed."));
		return 0;
	}
	return nSent;
}

void CSTXIOCPServer::InternalTerminate()
{
	if(m_BaseServerInfo.dwStatus & STXSS_TERMINATE)
		return;

	//LockServer(_T(__FILE__), __LINE__);

	// Change Server Status as pre-terminate 
	m_BaseServerInfo.dwStatus = STXSS_PRETERMINATE;

	CSTXServerBase::Terminate();

	//Cancel all custom operations

	STXTRACELOGE(_T("To Shutdown Server..."));
	STXTRACELOGE(_T("Current Client Context: \t%d"), m_mapClientContext.size());
	STXTRACELOGE(_T("Current R/W Buffers: \t%d/%d  (Available/Maximum)"), m_Buffers.GetBufferAvailableCount(), m_Buffers.GetBufferTotalCount());
	STXTRACELOGE(_T("Current queued Read: \t%d"), m_queueRead.size());
	STXTRACELOGE(_T("Current uncomplete Operation: \t%d"), m_setOperation.size());
	if (m_setOperation.size())
	{
		m_setOperation.foreach([&](LPSTXIOCPOPERATION pOperation)
		{
			STXTRACELOGE(_T("\tOperation created at %s(%d)"), pOperation->pszSrcFile, pOperation->nLine);

		});
	}
	m_setOperation.clear();

	// 停止整个服务器模块的过程如下:
	// 1. 将所有的活动 socket 关闭，包括TCP/UDP子服务器socket，客户端连接socket 和 TCP连接socket
	// 2. 在执行了第1步之后会有很多未决的 IO 操作,等待所有这些 IO 操作完成，并将 TimeOut 设置在一个较短的时间
	// 3. 设置一个标记，在所有 IO 操作完成之后的第一个 TimeOut 时，中止工作线程。
	// 4. 由于 StartServer() 一直在等待所有的工作线程中止，当它们确实中止的时候，StartServer() 返回，服务器模块结束工作。

	CloseAllMonitoredFolder();

	// Close all server socket, including TCP Listening Socket and UDP socket
	// (Cancel all operations pended on these socket)
	KillAllServers();
	KillAllClient();
	KillAllConnections();

	// Change Server Status for terminate Work-Threads 
	m_BaseServerInfo.dwStatus |= STXSS_TERMINATE;

	// Make IO Time-Out as short as possible
	// (Work-Thread Terminate check is done in IDLE loop)
	m_dwIOWait = 200;

	HANDLE *arrhWorkerThreads = new HANDLE[m_arrWorkerThreads.size()];
	int iPos = 0;
	vector<HANDLE>::iterator itThreads = m_arrWorkerThreads.begin();
	for(;itThreads!=m_arrWorkerThreads.end();itThreads++)
	{
		HANDLE hThread = (HANDLE)(*itThreads);
		arrhWorkerThreads[iPos++] = hThread;
	}

	//	::SetEvent(m_ServerInfo.hTerminateSignal);
	::WaitForMultipleObjects((DWORD)m_arrWorkerThreads.size(), arrhWorkerThreads, TRUE, INFINITE);
	delete []arrhWorkerThreads;
	m_setWorkerThreadId.clear();
}

BOOL CSTXIOCPServer::IssueReadFile( LPCTSTR lpszFile, CSTXIOCPServerClientContext *pClientContext, DWORD_PTR dwTag)
{
	return IssueReadFile(lpszFile, pClientContext, dwTag, FALSE, 0, 0);
}

BOOL CSTXIOCPServer::IssueReadFile( LPCTSTR lpszFile, CSTXIOCPServerClientContext *pClientContext, DWORD_PTR dwTag, BOOL bInternalUse, DWORD dwCookie, DWORD dwUserDataRemote)
{
	//Step 1
	CSTXIOCPBuffer *pBuffer = m_Buffers.GetBuffer(20);
	if(pBuffer == NULL)
	{
		STXTRACE(_T("----> Pending Reading... (Not implement)"));
		//PendingRead(sock, nSocketType, dwExtraData, dwExtraData2, dwTimeOut);
		return TRUE;
	}

	//Step 2
	HANDLE hFile = CreateFile(lpszFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	if(hFile == INVALID_HANDLE_VALUE)
	{
		m_Buffers.ReleaseBuffer(pBuffer);
		STXTRACELOGE(_T("[r][g][i]Failed to open file to read: %s"), lpszFile);
		return FALSE;
	}

	//Step 3

	//Step 4
	LPSTXIOCPCONTEXTKEY pContextKey = new STXIOCPCONTEXTKEY;
	memset(pContextKey, 0, sizeof(STXIOCPCONTEXTKEY));
	pContextKey->sock = INVALID_SOCKET;
	pContextKey->sock6 = INVALID_SOCKET;
	pContextKey->hFile = hFile;
	pContextKey->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_FILE;
	pContextKey->pClientContext = pClientContext;
	pContextKey->dwFileTag = dwTag;
	pContextKey->nBytesRead = 0;
	pContextKey->dwMonitorNotifyFilter = (bInternalUse ? 1 : 0);
	pContextKey->dwCookie = dwCookie;
	pContextKey->dwUserData = dwUserDataRemote;
	_tcscpy_s(pContextKey->szMonitoredFolder, sizeof(pContextKey->szMonitoredFolder) / sizeof(TCHAR), lpszFile);


	//Step 5
	HANDLE hIOCP = NULL;
	hIOCP = ::CreateIoCompletionPort(hFile, m_hIOCP, (ULONG_PTR)pContextKey, 0);
	if(hIOCP == NULL)
	{
		STXTRACELOGE(_T("[r][i]Error! In IssueReadFile(), Failed to bind file handle to the IO Completion Port..."));
		delete pContextKey;
		m_Buffers.ReleaseBuffer(pBuffer);
		CloseHandle(hFile);
		return FALSE;
	}

	//Step 6
	NEW_OPERATION(pOperation);

	LARGE_INTEGER v;
	v.QuadPart = 0;
	pOperation->ov.Offset = v.LowPart;
	pOperation->ov.OffsetHigh = v.HighPart;
	pOperation->pBuffer = pBuffer;
	pOperation->nOpType = STXIOCP_OPERATION_READ;
	pOperation->sock = INVALID_SOCKET;
	pOperation->dwSubmitTime = GetTickCount();
	pOperation->dwTimeOut = 0;
	pOperation->nSocketType = STXIOCP_CONTEXTKEY_SOCKET_TYPE_FILE;
	EnqueueOperation(pOperation);


	//Step 7
	BOOL bResult = FALSE;
	DWORD dwNumRead = 0;
	bResult = ReadFile(hFile, pBuffer->GetBufferPtr(), pBuffer->GetBufferLength(),&dwNumRead, &pOperation->ov);
	int iError = GetLastError();

	if(bResult)	//Succeed
	{
		// 完成端口工作线程会在稍后获得这个操作已经完成的通知。
		return TRUE;
	}
	else
	{
		if(iError == ERROR_IO_PENDING)		//IO操作已经投递成功
		{
			return TRUE;
		}
		else
		{
			m_Buffers.ReleaseBuffer(pBuffer);
		}
		CloseHandle(hFile);
		pOperation->MarkCanceled();
		DequeueOperation(pOperation);
		pOperation->Release();
	}

	return FALSE;
}

void CSTXIOCPServer::OnFileRead( CSTXIOCPServerClientContext *pClientContext, CSTXIOCPBuffer *pBuffer, DWORD_PTR dwTag )
{

}

void CSTXIOCPServer::OnUrlDownloadProgress(LPSTXIOCPSERVERHTTPCONTEXT pContext, int nBytesIncrease)
{
	STXTRACELOGE(_T(">> Downloading... %d"), nBytesIncrease);
}

void CSTXIOCPServer::OnUrlDownloadComplete(LPSTXIOCPSERVERHTTPCONTEXT pContext)
{

}

void CSTXIOCPServer::OnUrlDownloadCleanup(LPSTXIOCPSERVERHTTPCONTEXT pContext)
{

}

DWORD CSTXIOCPServer::OnQueryWorkerThreadCount()
{
	SYSTEM_INFO sysinfo;
	GetSystemInfo(&sysinfo);
	DWORD dwThreads = sysinfo.dwNumberOfProcessors * 2 + 2;		// NumberOfCpuCore * 2 + 2
	return dwThreads;
}

void CSTXIOCPServer::OnTcpSubServerInitialized(CSTXIOCPTcpServerContext *pServerContext)
{

}

void CSTXIOCPServer::OnTcpSubServerDestroyed(CSTXIOCPTcpServerContext *pServerContext)
{

}

void CSTXIOCPServer::OnUdpSubServerInitialized(CSTXIOCPUdpServerContext *pServerContext)
{

}

void CSTXIOCPServer::OnUdpSubServerDestroyed(CSTXIOCPUdpServerContext *pServerContext)
{

}

void CSTXIOCPServer::OnInternalFileRead( CSTXIOCPServerClientContext *pClientContext, CSTXIOCPBuffer *pBuffer, DWORD_PTR dwTag , __int64 dwOffset, DWORD dwCookie, DWORD dwUserDataRemote)
{
	if(pBuffer)
	{
		DWORD dwDataLen = pBuffer->GetDataLength();
		char *pszBuf = new char[pBuffer->GetDataLength() + 256];
		LPSTXIOCPSERVERPASSIVEDOWNLOADFILE pMsg = (LPSTXIOCPSERVERPASSIVEDOWNLOADFILE)pszBuf;
		char *pMsgPadding = (char*)(pMsg + 1);
		memcpy(pMsgPadding, pBuffer->GetBufferPtr(), dwDataLen);

		pMsg->header.wHeaderSize = sizeof(STXIOCPSERVERMESSAGEHEADER);
		pMsg->header.wOpType = STXIOCP_INTERNAL_OP_DOWNLOAD_PASSIVE;
		pMsg->header.dwContentSize = sizeof(STXIOCPSERVERPASSIVEDOWNLOADFILE) - sizeof(STXIOCPSERVERMESSAGEHEADER) + dwDataLen;
		pMsg->header.dwMagic = 0xFFFEFDFC;

		pMsg->dwOffset = dwOffset;
		pMsg->dwContentSize = dwDataLen;
		pMsg->dwCookie = dwCookie;
		pMsg->dwUserData = dwUserDataRemote;
		pMsg->wOpCode = STXIOCP_INTERNAL_OPCODE_DOWNLOAD_BLOCK | STXIOCP_INTERNAL_OPCODE_ACK;
		pMsg->wResult = 0;

		SendClientData(pClientContext, pszBuf, sizeof(STXIOCPSERVERPASSIVEDOWNLOADFILE) + pMsg->dwContentSize, 0);
		delete []pszBuf;
	}
	else
	{
		STXIOCPSERVERPASSIVEDOWNLOADFILE msg;
		msg.header.wHeaderSize = sizeof(STXIOCPSERVERMESSAGEHEADER);
		msg.header.wOpType = STXIOCP_INTERNAL_OP_DOWNLOAD_PASSIVE;
		msg.header.dwContentSize = sizeof(STXIOCPSERVERPASSIVEDOWNLOADFILE) - sizeof(STXIOCPSERVERMESSAGEHEADER);
		msg.header.dwMagic = 0xFFFEFDFC;

		msg.dwContentSize = 0;
		msg.dwCookie = dwCookie;
		msg.dwUserData = dwUserDataRemote;
		msg.wOpCode = STXIOCP_INTERNAL_OPCODE_DOWNLOAD_FINISH | STXIOCP_INTERNAL_OPCODE_ACK;
		msg.wResult = 0;

		SendClientData(pClientContext, &msg, sizeof(STXIOCPSERVERPASSIVEDOWNLOADFILE), 0);
	}
}

BOOL CSTXIOCPServer::AddFileReference( DWORD dwCookie, DWORD dwUserData, LPCTSTR lpszFile )
{
	BOOL bResult = FALSE;

	LARGE_INTEGER v;
	v.LowPart = dwUserData;
	v.HighPart = dwCookie;
	EnterCriticalSection(&m_csFileDictionary);
	map<__int64, STLSTRING>::iterator it = m_mapFileDictionary.find(v.QuadPart);
	if(it != m_mapFileDictionary.end(v.QuadPart))
	{
		//File Reference Exists. Error
		LeaveCriticalSection(&m_csFileDictionary);
		return FALSE;
	}
	if(PathFileExists(lpszFile))
	{
		m_mapFileDictionary.insertValue(v.QuadPart, lpszFile);
		bResult = TRUE;
	}
	LeaveCriticalSection(&m_csFileDictionary);
	return bResult;
}

BOOL CSTXIOCPServer::RemoveFileReference( DWORD dwCookie, DWORD dwUserData )
{
	LARGE_INTEGER v;
	v.LowPart = dwUserData;
	v.HighPart = dwCookie;
	EnterCriticalSection(&m_csFileDictionary);
	map<__int64, STLSTRING>::iterator it = m_mapFileDictionary.find(v.QuadPart);
	if(it != m_mapFileDictionary.end(v.QuadPart))
	{
		m_mapFileDictionary.erase(it);
		LeaveCriticalSection(&m_csFileDictionary);
		return TRUE;
	}
	LeaveCriticalSection(&m_csFileDictionary);
	return FALSE;
}

void CSTXIOCPServer::OnTimerTimeout( LPVOID lpTimerParam )
{
	LPSTXIOCPOPERATION pOperation = (LPSTXIOCPOPERATION)lpTimerParam;
	//STXTRACE(_T("OnTimerTimeout\t%p"), pOperation);

	// 超时，分两种情况，分别处理网络Operation和自定义Operation
pOperation->Lock();
if (pOperation->nOpType < STXIOCP_OPERATION_CUSTOM)
{
	//处理网络 Operation.

	if (pOperation->nSocketType == STXIOCP_CONTEXTKEY_SOCKET_TYPE_CLIENT)
	{
		if (OnClientContextOperationTimeout(pOperation->pClientContext))
		{
			pOperation->MarkCanceled();
			DisconnectClientByTimeout(pOperation->pClientContext);
			pOperation->dwTimeOut = 0;
		}
	}
	else
	{
		pOperation->MarkCanceled();
		closesocket(pOperation->sock);
		pOperation->dwTimeOut = 0;
	}
}
else
{
	if (pOperation->dwCompleteType == STXIOCP_CUSTOM_OPERATION_QUEUED)
	{
		pOperation->dwCompleteType = STXIOCP_CUSTOM_OPERATION_TIMEOUT;
		PostQueuedCompletionStatus(m_hIOCP, 0, NULL, &pOperation->ov);
		pOperation->dwTimeOut = 0;
	}
}

if (pOperation->hTimerHandle)
{
	DeleteTimerObject(pOperation->hTimerHandle, pOperation->nTimerAltId);
	pOperation->hTimerHandle = NULL;
	//pOperation->Release();
}
pOperation->Unlock();
}

int CSTXIOCPServer::ProcessException(LPEXCEPTION_POINTERS pExp)
{
	CONTEXT c;
	if (pExp != NULL && pExp->ExceptionRecord != NULL)
	{
		memcpy(&c, pExp->ContextRecord, sizeof(CONTEXT));
		//c.ContextFlags = CONTEXT_FULL;

		const DWORD nExceptionBufferLength = 4096;
		TCHAR szExceptionText[nExceptionBufferLength];
		DWORD dwExceptionCode = pExp->ExceptionRecord->ExceptionCode;
		switch (dwExceptionCode)
		{
		case EXCEPTION_ACCESS_VIOLATION:
			_stprintf_s(szExceptionText, _T("Access violation %s location 0x%p"), (pExp->ExceptionRecord->ExceptionInformation[0] ? _T("writing to") : _T("reading from")), (void*)pExp->ExceptionRecord->ExceptionInformation[1]);
			break;
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
			_stprintf_s(szExceptionText, _T("Accessed array out of bounds")); break;
			break;
		case EXCEPTION_BREAKPOINT:
			_stprintf_s(szExceptionText, _T("Hit breakpoint")); break;
		case EXCEPTION_DATATYPE_MISALIGNMENT:
			_stprintf_s(szExceptionText, _T("Data misaligned")); break;
		case EXCEPTION_FLT_DENORMAL_OPERAND:
			_stprintf_s(szExceptionText, _T("FPU denormal operand")); break;
		case EXCEPTION_FLT_DIVIDE_BY_ZERO:
			_stprintf_s(szExceptionText, _T("FPU divide by zero")); break;
		case EXCEPTION_FLT_INEXACT_RESULT:
			_stprintf_s(szExceptionText, _T("FPU inexact result")); break;
		case EXCEPTION_FLT_INVALID_OPERATION:
			_stprintf_s(szExceptionText, _T("FPU invalid operation")); break;
		case EXCEPTION_FLT_OVERFLOW:
			_stprintf_s(szExceptionText, _T("FPU overflow")); break;
		case EXCEPTION_FLT_STACK_CHECK:
			_stprintf_s(szExceptionText, _T("FPU stack fault")); break;
		case EXCEPTION_FLT_UNDERFLOW:
			_stprintf_s(szExceptionText, _T("FPU underflow")); break;
		case EXCEPTION_GUARD_PAGE:
			_stprintf_s(szExceptionText, _T("Attempt to access guard page")); break;
		case EXCEPTION_ILLEGAL_INSTRUCTION:
			_stprintf_s(szExceptionText, _T("Attempt to execeute illegal instruction")); break;
		case EXCEPTION_IN_PAGE_ERROR:
			_stprintf_s(szExceptionText, _T("Page fault")); break;
		case EXCEPTION_INT_DIVIDE_BY_ZERO:
			_stprintf_s(szExceptionText, _T("Integer divide by zero")); break;
		case EXCEPTION_INT_OVERFLOW:
			_stprintf_s(szExceptionText, _T("Integer overflow")); break;
		case EXCEPTION_INVALID_DISPOSITION:
			_stprintf_s(szExceptionText, _T("Badly handled exception")); break;
		case EXCEPTION_NONCONTINUABLE_EXCEPTION:
			_stprintf_s(szExceptionText, _T("Exception during exception handling")); break;
		case EXCEPTION_PRIV_INSTRUCTION:
			_stprintf_s(szExceptionText, _T("Attempt to execute a privileged instruction")); break;
		case EXCEPTION_SINGLE_STEP:
			_stprintf_s(szExceptionText, _T("Processor in single step mode")); break;
		case EXCEPTION_STACK_OVERFLOW:
			_stprintf_s(szExceptionText, _T("Stack overflow")); break;
		default:
		{
			if (dwExceptionCode & (1 << 29))	//User define dexception code
			{
				if (pExp->ExceptionRecord->NumberParameters >= 1)
				{
					LPCTSTR lpszArguments = _T("");
					ULONG_PTR *pArgs = pExp->ExceptionRecord->ExceptionInformation;
					CSTXIOCPServer *pServer = (CSTXIOCPServer*)pArgs[0];
					LPCTSTR lpszUDExceptionName = pServer->OnGetUserDefinedExceptionName(dwExceptionCode);
					DWORD dwArgumentBufferLen = pServer->OnParseUserDefinedExceptionArgument(dwExceptionCode, pExp->ExceptionRecord->NumberParameters - 1, pArgs + 1, NULL, 0);
					BOOL bArgumentBufferUsed = FALSE;
					if (dwArgumentBufferLen && dwArgumentBufferLen <= nExceptionBufferLength - 96)
					{
						bArgumentBufferUsed = TRUE;
						LPTSTR pArgumentBuffer = new TCHAR[dwArgumentBufferLen + 1];
						pServer->OnParseUserDefinedExceptionArgument(dwExceptionCode, pExp->ExceptionRecord->NumberParameters - 1, pArgs + 1, pArgumentBuffer, dwArgumentBufferLen);
						lpszArguments = pArgumentBuffer;
					}
					else
					{
						if (dwArgumentBufferLen > nExceptionBufferLength - 96)
						{
							lpszArguments = _T("## Exception arguments text is too large! ##");
						}
					}

					if (lpszUDExceptionName)
					{
						if (bArgumentBufferUsed)
						{
							_stprintf_s(szExceptionText, _T("[User Defined Exception 0x%X] %s\r\n%s"), dwExceptionCode, lpszUDExceptionName, lpszArguments);
						}
						else
						{
							_stprintf_s(szExceptionText, _T("[User Defined Exception 0x%X] %s"), dwExceptionCode, lpszUDExceptionName);
						}
					}
					else
					{
						if (bArgumentBufferUsed)
						{
							_stprintf_s(szExceptionText, _T("[User Defined Exception 0x%X]\r\n%s"), dwExceptionCode, lpszArguments);
						}
						else
						{
							_stprintf_s(szExceptionText, _T("[User Defined Exception 0x%X]"), dwExceptionCode);
						}
					}

					if (bArgumentBufferUsed)
					{
						delete[]lpszArguments;
					}
				}
			}
			else
			{
				_stprintf_s(szExceptionText, _T("Unknown exception code"));
			}
		}

		g_LogGlobal.Lock();
		STXTRACELOGE(1, _T("[r][i]**Exception** - %s"), szExceptionText);
		ShowExceptionCallStack(&c, NULL, 1);
		g_LogGlobal.Unlock();
		}
	}
	else
	{
		g_LogGlobal.Lock();
		STXTRACELOGE(1, _T("[r][i]Unknown exception!"));
		ShowExceptionCallStack(NULL, NULL, 1);
		g_LogGlobal.Unlock();
	}

	return EXCEPTION_EXECUTE_HANDLER;
}


//////////////////////////////////////////////////////////////////////////
// CSTXIOCPServerClientContext

CSTXIOCPServerClientContext::CSTXIOCPServerClientContext()
{
	m_dwOperationTimeout = 0xFFFFFFFF;
	m_szClientIP[0] = _T('\0');

	m_cbBufferSize = 16384;
	m_pRecvBuffer = (char*)malloc(m_cbBufferSize);
	m_cbWriteOffset = 0;
	m_nAddressFamily = AF_INET;
	m_bDisconnected = FALSE;

	m_nDisconnectEventProcessed = 0;
	m_dwUploadCookieBase = 1;
	m_pOperationBufferRead = NULL;
	m_pOperationBufferWrite = NULL;
	m_nBufferedSendDataLength = 0;

	_timeOutDisconnected = FALSE;
	memset(&_lastDataReceiveTime, 0, sizeof(_lastDataReceiveTime));
//	GetLocalTime(&_lastDataReceiveTime);
	_lastDataReceiveTime.wYear = 1800;		//a very long time ago
}

CSTXIOCPServerClientContext::~CSTXIOCPServerClientContext()
{
	if (_timeOutDisconnected)
	{
		STXTRACEE(_T("[g] CSTXIOCPServerClientContext destroyed by a timeout disconnection!"));
	}
	if(m_socket != INVALID_SOCKET)
	{
		STXTRACELOGE(_T("[r][g][i]Warning !! m_socket not empty at CSTXIOCPServerClientContext destructor!"));
		closesocket(m_socket);
	}

	map<__int64, CSTXIOCPBuffer*>::iterator it = m_mapUploadBuffers.begin();
	for(;it!=m_mapUploadBuffers.end();it++)
	{
		delete it->second;
	}
	m_mapUploadBuffers.clear();

	free(m_pRecvBuffer);

	if(m_nDisconnectEventProcessed == 0)
		STXTRACELOGE(_T("[r][g][i]Warning !! CSTXIOCPServerClientContext delete without processing Disconnect Event!! Did you forget to call OnClientDisconnect method of base class ?"));
	else if(m_nDisconnectEventProcessed > 1)
		STXTRACELOGE(_T("[r][i]Error !! CSTXIOCPServerClientContext delete with more than 1 (%d) Disconnect Event processed!!"), m_nDisconnectEventProcessed);

	if (m_pOperationBufferRead)
	{
		m_pServer->m_Buffers.ReleaseBuffer(m_pOperationBufferRead);
		m_pOperationBufferRead = NULL;
	}
	if (m_pOperationBufferWrite)
	{
		m_pServer->m_Buffers.ReleaseBuffer(m_pOperationBufferWrite);
		m_pOperationBufferWrite = NULL;
	}

	CSTXIOCPBuffer *pBuffer = NULL;
	bool bFound = false;
	while (bFound = _queuedSendData.try_dequeue(pBuffer))
	{
		delete pBuffer;
	}

	m_pServerContext->DecreaseClientCount();
}

void CSTXIOCPServerClientContext::AppendRecvData(LPVOID lpData, DWORD cbDataLen)
{
	UpdateLastDataReceiveTime();

	//由于每次处理收到的消息包完成之后才会再次调用IssueClientRead。此处不存在多线程同时进入此函数的情况
	LONG cbOldBufferSize = m_cbBufferSize;
	while((LONG)cbDataLen + m_cbWriteOffset > m_cbBufferSize)
		m_cbBufferSize *= 2;

	if(m_cbBufferSize > cbOldBufferSize)
	{
		m_pRecvBuffer = (char*)realloc(m_pRecvBuffer, m_cbBufferSize);
	}

	memcpy(m_pRecvBuffer + m_cbWriteOffset, lpData, cbDataLen);
	m_cbWriteOffset += cbDataLen;
}

void CSTXIOCPServerClientContext::SkipRecvBuffer(LONG cbSkip)
{
	//由于每次处理收到的消息包完成之后才会再次调用IssueClientRead。此处不存在多线程同时进入此函数的情况
	memmove(m_pRecvBuffer, m_pRecvBuffer + cbSkip, m_cbWriteOffset - cbSkip);
	m_cbWriteOffset -= cbSkip;
}

BOOL CSTXIOCPServerClientContext::OnInitialize()
{
	if(m_nAddressFamily == AF_INET6)
	{
		DWORD dwLen = sizeof(m_szClientIP) / sizeof(TCHAR);
		return (WSAAddressToString((SOCKADDR*)&m_addrRemote6, sizeof(m_addrRemote6), NULL, m_szClientIP, &dwLen) == 0);
	}
	else
	{
		DWORD dwLen = sizeof(m_szClientIP) / sizeof(CHAR);
		return (WSAAddressToString((SOCKADDR*)&m_addrRemote, sizeof(m_addrRemote), NULL, m_szClientIP, &dwLen) == 0);
	}
		
	return TRUE;
}

void CSTXIOCPServerClientContext::OnDestroy()
{

}

DWORD CSTXIOCPServerClientContext::OnQueryDownloadSize(DWORD dwCookie, LPVOID pPaddingData, DWORD dwPaddingDataLen, DWORD dwUserDataRemote)
{
	return GetUploadTaskSize(dwCookie, dwUserDataRemote);
}

DWORD CSTXIOCPServerClientContext::OnQueryDownloadData(DWORD dwCookie, DWORD dwOffset, LPVOID lpData, DWORD cbQueryBufferLen, DWORD dwUserDataRemote)
{
	return ProcessDownload(dwCookie, dwUserDataRemote, lpData, dwOffset, cbQueryBufferLen);
}

LONG CSTXIOCPServerClientContext::AddRef()
{
	return __super::AddRef();
}

LONG CSTXIOCPServerClientContext::Release()
{
	return __super::Release();
}

LPCTSTR CSTXIOCPServerClientContext::GetClientIP() const
{
	return m_szClientIP;
}

BOOL CSTXIOCPServerClientContext::IsSameIP(CSTXIOCPServerClientContext *pClientContext)
{
	return (_tcsicmp(pClientContext->m_szClientIP, m_szClientIP) == 0);
}

SOCKET CSTXIOCPServerClientContext::GetSocket()
{
	return m_socket;
}

SOCKET CSTXIOCPServerClientContext::GetSocketOriginal()
{
	return m_socketCopy;
}

void CSTXIOCPServerClientContext::SetOperationTimeout(DWORD dwTimeout)
{
	m_dwOperationTimeout = dwTimeout;
}

void CSTXIOCPServerClientContext::CloseSocket()
{
	if(m_socket == INVALID_SOCKET)
	{
		//STXTRACE(_T("[r][g][i]Warning! Client socket [socket = %d] has been closed previously!  [ReleaseClient()]"), m_socketCopy);
	}
	else
	{
		closesocket(m_socket);
		m_socket = INVALID_SOCKET;
	}
}

void CSTXIOCPServerClientContext::SetAddressFamily(int nAddressFamily)
{
	m_nAddressFamily = nAddressFamily;
}

int CSTXIOCPServerClientContext::GetAddressFamily()
{
	return m_nAddressFamily;
}

BOOL CSTXIOCPServerClientContext::IsDisconnected()
{
	return m_bDisconnected;
	//return m_socket == INVALID_SOCKET;
}

BOOL CSTXIOCPServerClientContext::MarkDisconnected(BOOL bDisconnected)
{
	BOOL bOldValue = m_bDisconnected;
	m_bDisconnected = bDisconnected;
	return bOldValue;
}

void CSTXIOCPServerClientContext::Lock()
{
	_mtxLock.lock();
}

void CSTXIOCPServerClientContext::Unlock()
{
	_mtxLock.unlock();
}

void CSTXIOCPServerClientContext::LockSend()
{
	_mtxLockSend.lock();
}

void CSTXIOCPServerClientContext::UnlockSend()
{
	_mtxLockSend.unlock();
}

void CSTXIOCPServerClientContext::UpdateLastDataReceiveTime()
{
	GetLocalTime(&_lastDataReceiveTime);
}

LPVOID CSTXIOCPServerClientContext::GetMessageBasePtr()
{
	return m_pRecvBuffer;
}

DWORD CSTXIOCPServerClientContext::GetBufferedMessageLength()
{
	return m_cbWriteOffset;
}

DWORD CSTXIOCPServerClientContext::PrepareUpload(DWORD dwUserDataRemote, DWORD dwCookieDesire)
{
	if(dwCookieDesire == 0)
	{
		LARGE_INTEGER v;
		v.LowPart = dwUserDataRemote;
		v.HighPart = m_dwUploadCookieBase;
		m_mapUploadBuffers[v.QuadPart] = new CSTXIOCPBuffer();
		DWORD dwCookie = m_dwUploadCookieBase;
		m_dwUploadCookieBase++;
		return dwCookie;
	}
	else
	{
		LARGE_INTEGER v;
		v.LowPart = dwUserDataRemote;
		v.HighPart = dwCookieDesire;

		map<__int64, CSTXIOCPBuffer*>::iterator it = m_mapUploadBuffers.find(v.QuadPart);
		if(it != m_mapUploadBuffers.end())
		{
			delete it->second;
			m_mapUploadBuffers.erase(it);
		}
		m_mapUploadBuffers[v.QuadPart] = new CSTXIOCPBuffer();

		return dwCookieDesire;
	}
}

DWORD CSTXIOCPServerClientContext::ProcessUpload(DWORD dwCookie, DWORD dwUserDataRemote, LPVOID lpData, DWORD cbDataLen)
{
	LARGE_INTEGER v;
	v.LowPart = dwUserDataRemote;
	v.HighPart = dwCookie;
	map<__int64, CSTXIOCPBuffer*>::iterator it = m_mapUploadBuffers.find(v.QuadPart);
	if(it == m_mapUploadBuffers.end())
		return 0;

	return it->second->WriteBuffer(lpData, cbDataLen);
}

DWORD CSTXIOCPServerClientContext::ProcessDownload(DWORD dwCookie, DWORD dwUserDataRemote, LPVOID lpData, DWORD dwOffset, DWORD cbQueryDataLen)
{
	LARGE_INTEGER v;
	v.LowPart = dwUserDataRemote;
	v.HighPart = dwCookie;
	map<__int64, CSTXIOCPBuffer*>::iterator it = m_mapUploadBuffers.find(v.QuadPart);
	if(it == m_mapUploadBuffers.end())
		return 0;

	char *pszBase = (char*)it->second->GetBufferPtr();
	DWORD dwDataLen = it->second->GetDataLength();

	if(dwDataLen <= dwOffset)
		return 0;

	DWORD dwQueryLen = min(dwDataLen - dwOffset, cbQueryDataLen);
	memcpy(lpData, pszBase + dwOffset, dwQueryLen);
	return dwQueryLen;
}

BOOL CSTXIOCPServerClientContext::CloseUpload(DWORD dwCookie, DWORD dwUserDataRemote)
{
	LARGE_INTEGER v;
	v.LowPart = dwUserDataRemote;
	v.HighPart = dwCookie;

	map<__int64, CSTXIOCPBuffer*>::iterator it = m_mapUploadBuffers.find(v.QuadPart);
	if(it == m_mapUploadBuffers.end())
		return FALSE;

	delete it->second;
	m_mapUploadBuffers.erase(it);
	return TRUE;
}

LPCTSTR CSTXIOCPServerClientContext::OnGetClientDisplayName()
{
	return GetClientIP();
}

BOOL CSTXIOCPServerClientContext::IsUploadTask(DWORD dwCookie, DWORD dwUserDataRemote)
{
	LARGE_INTEGER v;
	v.LowPart = dwUserDataRemote;
	v.HighPart = dwCookie;

	map<__int64, CSTXIOCPBuffer*>::iterator it = m_mapUploadBuffers.find(v.QuadPart);
	if(it == m_mapUploadBuffers.end())
		return FALSE;

	return TRUE;
}

DWORD CSTXIOCPServerClientContext::GetUploadTaskSize(DWORD dwCookie, DWORD dwUserDataRemote)
{
	LARGE_INTEGER v;
	v.LowPart = dwUserDataRemote;
	v.HighPart = dwCookie;

	map<__int64, CSTXIOCPBuffer*>::iterator it = m_mapUploadBuffers.find(v.QuadPart);
	if(it == m_mapUploadBuffers.end())
		return 0;

	return it->second->GetDataLength();
}

CSTXIOCPBuffer* CSTXIOCPServerClientContext::GetUploadTaskBuffer(DWORD dwCookie, DWORD dwUserDataRemote)
{
	LARGE_INTEGER v;
	v.LowPart = dwUserDataRemote;
	v.HighPart = dwCookie;

	map<__int64, CSTXIOCPBuffer*>::iterator it = m_mapUploadBuffers.find(v.QuadPart);
	if(it == m_mapUploadBuffers.end())
		return NULL;

	return it->second;
}

tr1::shared_ptr<CSTXIOCPTcpServerContext> CSTXIOCPServerClientContext::GetServerContext()
{
	return m_pServerContext;
}

LONG64 CSTXIOCPServerClientContext::DecreaseBufferedSendLength(LONG64 nLength)
{
	return InterlockedAdd64(&m_nBufferedSendDataLength, -nLength);
}

LONG64 CSTXIOCPServerClientContext::IncreaseBufferedSendLength(LONG64 nLength)
{
	return InterlockedAdd64(&m_nBufferedSendDataLength, nLength);
}

LONG64 CSTXIOCPServerClientContext::GetBufferedSendLength()
{
	return m_nBufferedSendDataLength;
}

//////////////////////////////////////////////////////////////////////////
// CSTXIOCPServerContext

CSTXIOCPServerContext::CSTXIOCPServerContext()
{
	m_dwServerParam = 0;
	m_socket = INVALID_SOCKET;
	m_socket6 = INVALID_SOCKET;
	m_pServer = NULL;
	m_pContextKey = NULL;
}

CSTXIOCPServerContext::~CSTXIOCPServerContext()
{
}

void CSTXIOCPServerContext::SetSocket(SOCKET sock, SOCKET sock6)
{
	m_bSocketClosed = false;
	m_bSocket6Closed = false;

	m_socket = sock;
	m_socket6 = sock6;
}

SOCKET CSTXIOCPServerContext::GetSocket()
{
	return m_socket;
}

SOCKET CSTXIOCPServerContext::GetSocket6()
{
	return m_socket6;
}

void CSTXIOCPServerContext::SetServer(CSTXIOCPServer *pServer)
{
	m_pServer = pServer;
}

void CSTXIOCPServerContext::SetContextKey(LPSTXIOCPCONTEXTKEY pContextKey)
{
	m_pContextKey = pContextKey;
}

BOOL CSTXIOCPServerContext::IsSocketClear()
{
	return m_socket == INVALID_SOCKET && m_socket6 == INVALID_SOCKET;
}

void CSTXIOCPServerContext::CloseSocket()
{
	if (!m_bSocketClosed)
	{
		closesocket(m_socket);
		m_bSocketClosed = true;
	}
	m_socket = INVALID_SOCKET;
}

void CSTXIOCPServerContext::CloseSocket6()
{
	if (!m_bSocket6Closed)
	{
		closesocket(m_socket6);
		m_bSocket6Closed = true;
	}
	m_socket6 = INVALID_SOCKET;
}

void CSTXIOCPServerContext::MarkSocketClosed()
{
	m_bSocketClosed = true;
}

void CSTXIOCPServerContext::MarkSocket6Closed()
{
	m_bSocket6Closed = true;
}

//////////////////////////////////////////////////////////////////////////
// CSTXIOCPFolderMonitorContext

CSTXIOCPFolderMonitorContext::CSTXIOCPFolderMonitorContext()
{
	m_nMonitorID = -1;
}

CSTXIOCPFolderMonitorContext::~CSTXIOCPFolderMonitorContext()
{

}

void CSTXIOCPFolderMonitorContext::AddIgnoreFileExtension(LPCTSTR lpszExt)
{
	_ignoreFileExtensions.insert(lpszExt);
}

void CSTXIOCPFolderMonitorContext::RemoveIgnoreFileExtension(LPCTSTR lpszExt)
{
	_ignoreFileExtensions.erase(lpszExt);
}

BOOL CSTXIOCPFolderMonitorContext::IsIgnoreFileExtension(LPCTSTR lpszExt)
{
	return _ignoreFileExtensions.find(lpszExt) != _ignoreFileExtensions.end(lpszExt);
}


//////////////////////////////////////////////////////////////////////////
// CSTXIOCPTcpServerContext

CSTXIOCPTcpServerContext::CSTXIOCPTcpServerContext()
{
	m_bClosed = FALSE;
	_currentClientCount = 0;
	_maximumClientCount = 1;
	m_nListeningPort = 0;
}

CSTXIOCPTcpServerContext::~CSTXIOCPTcpServerContext()
{

}

void CSTXIOCPTcpServerContext::OnDestroy()
{
	LARGE_INTEGER t;
	t.QuadPart = GetRunTime();
	TCHAR szRunTime[MAX_PATH];
	StrFromTimeInterval(szRunTime, MAX_PATH, t.LowPart, 3);

	STXTRACELOGE(_T("[g]TCP Server <%s> Destroy! Run for %s."), GetServerParamString().c_str(), szRunTime);
}

void CSTXIOCPTcpServerContext::MarkClosed(BOOL bClosed)
{
	m_bClosed = bClosed;
}

BOOL CSTXIOCPTcpServerContext::IsClosed()
{
	return m_bClosed;
}

LONG64 CSTXIOCPTcpServerContext::IncreaseClientCount()
{
	return InterlockedIncrement64(&_currentClientCount);
}

LONG64 CSTXIOCPTcpServerContext::DecreaseClientCount()
{
	return InterlockedDecrement64(&_currentClientCount);
}

LONG64 CSTXIOCPTcpServerContext::GetCurrentClientCount()
{
	return _currentClientCount;
}

LONG64 CSTXIOCPTcpServerContext::GetClientCountLimit()
{
	return _maximumClientCount;
}

BOOL CSTXIOCPTcpServerContext::IsReachedMaximumClient()
{
	if (_maximumClientCount <= 0)
	{
		return FALSE;
	}
	return _currentClientCount >= _maximumClientCount;
}

void CSTXIOCPTcpServerContext::SetListeningPort(UINT nPort)
{
	m_nListeningPort = nPort;
}

UINT CSTXIOCPTcpServerContext::GetListeningPort()
{
	return m_nListeningPort;
}

void CSTXIOCPTcpServerContext::SetMaximumClientCount(LONG64 nMaxClientCount)
{
	_maximumClientCount = nMaxClientCount;
}


//////////////////////////////////////////////////////////////////////////
// CSTXIOCPUdpServerContext

CSTXIOCPUdpServerContext::CSTXIOCPUdpServerContext(DWORD_PTR dwServeParam) : CSTXUdpServerContextBase(dwServeParam)
{
	m_nUdpPort = 0;
}

CSTXIOCPUdpServerContext::~CSTXIOCPUdpServerContext()
{
	STXTRACE(_T("[g]CSTXIOCPUdpServerContext destroyed!"));
}

void CSTXIOCPUdpServerContext::SetUdpPort(UINT nPort)
{
	m_nUdpPort = nPort;
}

UINT CSTXIOCPUdpServerContext::GetUdpPort()
{
	return m_nUdpPort;
}

void CSTXIOCPUdpServerContext::SetSocket(SOCKET sock, SOCKET sock6)
{
	m_bSocketClosed = false;
	m_bSocket6Closed = false;

	m_socket = sock;
	m_socket6 = sock6;
}

SOCKET CSTXIOCPUdpServerContext::GetSocket()
{
	return m_socket;
}

SOCKET CSTXIOCPUdpServerContext::GetSocket6()
{
	return m_socket6;
}

void CSTXIOCPUdpServerContext::MarkSocketClosed()
{
	m_bSocketClosed = true;
}

void CSTXIOCPUdpServerContext::MarkSocket6Closed()
{
	m_bSocket6Closed = true;
}

void CSTXIOCPUdpServerContext::SetServer(CSTXIOCPServer *pServer)
{
	m_pServer = pServer;
}

void CSTXIOCPUdpServerContext::SetContextKey(LPSTXIOCPCONTEXTKEY pContextKey)
{
	m_pContextKey = pContextKey;
}

BOOL CSTXIOCPUdpServerContext::IsSocketClear()
{
	return m_socket == INVALID_SOCKET && m_socket6 == INVALID_SOCKET;
}

void CSTXIOCPUdpServerContext::CloseSocket()
{
	if (!m_bSocketClosed)
	{
		closesocket(m_socket);
		m_bSocketClosed = true;
	}
	m_socket = INVALID_SOCKET;
}

void CSTXIOCPUdpServerContext::CloseSocket6()
{
	if (!m_bSocket6Closed)
	{
		closesocket(m_socket6);
		m_bSocket6Closed = true;
	}
	m_socket6 = INVALID_SOCKET;
}

LONG CSTXIOCPUdpServerContext::Send(LPVOID lpData, DWORD dwDataLen, LPCTSTR lpszTargetHost, UINT nTargetPort)
{
	return m_pServer->SendUdpData(m_nUdpPort, lpData, dwDataLen, lpszTargetHost, nTargetPort);
}


//////////////////////////////////////////////////////////////////////////
// CSTXIOCPTcpConnectionContext

CSTXIOCPTcpConnectionContext::CSTXIOCPTcpConnectionContext()
{
	m_bConnected = FALSE;
	m_socket = INVALID_SOCKET;
	m_nConnectionID = -1;
	m_pContextKey = NULL;
	m_pServer = NULL;
	m_nAddressFamily = AF_INET;
	m_dwConnectionFlags = 0;
	memset(m_szTargetHost, 0, sizeof(m_szTargetHost));
	m_uTargetPort = 0;

	m_cbBufferSize = 8192;
	m_pRecvBuffer = (char*)malloc(m_cbBufferSize);
	m_cbWriteOffset = 0;

}

CSTXIOCPTcpConnectionContext::~CSTXIOCPTcpConnectionContext()
{
	STXTRACE(_T("[g]CSTXIOCPTcpConnectionContext destroyed!"));

// 	if(m_pContextKey)
// 	{
// 		delete m_pContextKey;
// 		m_pContextKey = NULL;
// 	}
	free(m_pRecvBuffer);
}

void CSTXIOCPTcpConnectionContext::Disconnect(BOOL bForceNoKeep)
{
	if(bForceNoKeep)
	{
		ModifyFlags(0, STXIOCP_SERVER_CONNECTION_FLAG_KEEPCONNECT);
	}

	closesocket(m_socket);
}

void CSTXIOCPTcpConnectionContext::SetSocket(SOCKET sock)
{
	m_socket = sock;
}

void CSTXIOCPTcpConnectionContext::SetConnectionID(LONG nID)
{
	m_nConnectionID = nID;
}

LONG CSTXIOCPTcpConnectionContext::GetConnectionID()
{
	return m_nConnectionID;
}

SOCKET CSTXIOCPTcpConnectionContext::GetSocket()
{
	return m_socket;
}

void CSTXIOCPTcpConnectionContext::SetContextKey(LPSTXIOCPCONTEXTKEY pContextKey)
{
	m_pContextKey = pContextKey;
}

void CSTXIOCPTcpConnectionContext::SetServer(CSTXIOCPServer *pServer)
{
	m_pServer = pServer;
}

LPSTXIOCPCONTEXTKEY CSTXIOCPTcpConnectionContext::GetContextKey()
{
	return m_pContextKey;
}

BOOL CSTXIOCPTcpConnectionContext::IsConnected()
{
	return m_bConnected;
}

int CSTXIOCPTcpConnectionContext::GetAddressFamily()
{
	return m_nAddressFamily;
}

void CSTXIOCPTcpConnectionContext::SetAddressFamily(int nAddressFamily)
{
	m_nAddressFamily = nAddressFamily;
}

void CSTXIOCPTcpConnectionContext::AppendRecvData(LPVOID lpData, DWORD cbDataLen)
{
	LONG cbOldBufferSize = m_cbBufferSize;

	//C1 :Expand buffer if needed.
	while((LONG)cbDataLen + m_cbWriteOffset > m_cbBufferSize)
		m_cbBufferSize *= 2;

	if(m_cbBufferSize > cbOldBufferSize)
	{
		m_pRecvBuffer = (char*)realloc(m_pRecvBuffer, m_cbBufferSize);
	}
	//C1 End

	memcpy(m_pRecvBuffer + m_cbWriteOffset, lpData, cbDataLen);
	m_cbWriteOffset += cbDataLen;
}

void CSTXIOCPTcpConnectionContext::SkipRecvBuffer(LONG cbSkip)
{
	memmove(m_pRecvBuffer, m_pRecvBuffer + cbSkip, m_cbWriteOffset - cbSkip);
	m_cbWriteOffset -= cbSkip;
}

LPVOID CSTXIOCPTcpConnectionContext::GetMessageBasePtr()
{
	return m_pRecvBuffer;
}

DWORD CSTXIOCPTcpConnectionContext::GetBufferedMessageLength()
{
	return m_cbWriteOffset;
}

DWORD CSTXIOCPTcpConnectionContext::ModifyFlags( DWORD dwAdd, DWORD dwRemove )
{
	if(dwAdd & dwRemove)
	{
		STXTRACELOG(_T("[r][i]Warning: Don't add and remove the same flags at the same time! in CSTXIOCPTcpConnectionContext::ModifyFlags"));
	}

	m_dwConnectionFlags |= dwAdd;
	m_dwConnectionFlags &= (~dwRemove);

	return m_dwConnectionFlags;
}

void CSTXIOCPTcpConnectionContext::MarkConnected()
{
	m_bConnected = TRUE;
}

void CSTXIOCPTcpConnectionContext::MarkDisonnected()
{
	m_bConnected = FALSE;
}

DWORD CSTXIOCPTcpConnectionContext::GetFlags() const
{
	return m_dwConnectionFlags;
}

LPCTSTR CSTXIOCPTcpConnectionContext::GetTargetHostName()
{
	return m_szTargetHost;
}

UINT CSTXIOCPTcpConnectionContext::GetTargetHostPort()
{
	return m_uTargetPort;
}

void CSTXIOCPTcpConnectionContext::SetTargetHoatName( LPCTSTR lpszHostName )
{
	_tcscpy_s(m_szTargetHost, lpszHostName);
}

void CSTXIOCPTcpConnectionContext::SetTargetHoatPort( UINT uPort )
{
	m_uTargetPort = uPort;
}
