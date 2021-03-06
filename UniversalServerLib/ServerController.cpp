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

#include "stdafx.h"
#include "ServerController.h"
#include <Shlwapi.h>
#include "UniversalIOCPServer.h"



CServerController::CServerController(std::string name)
{
	_pServer = NULL;
	if (_server == NULL)
	{
		_server = new CUniversalIOCPServer();
		if (_server == NULL)
		{
			STXTRACELOGFE(_T("[r][i]CServerController::CServerController: not enough memory."));
		}
		else
		{
			memcpy(&_serverInitializationInfo, &_server->_serverInitializationInfo, sizeof(_serverInitializationInfo));
		}
	}
}


CServerController::~CServerController()
{
	if (_server)
	{
		delete _server;
		_server = NULL;
	}
}

void CServerController::StartServer(int nDefaultTcpTimeout, int nInitialBufferCount, int nMaxBufferCount, int nBufferSize)
{
	_server->_pServer = _pServer;

	_future = std::async(std::launch::async, [=]() {

		TCHAR szPath[MAX_PATH];
		HMODULE currentModule = GetModuleHandle(NULL);
		GetModuleFileName(currentModule, szPath, MAX_PATH);
		PathRemoveFileSpec(szPath);
		PathAddBackslash(szPath);
		_tcscat_s(szPath, _T("server.log"));

		_server->_serverInitializationInfo.pszLogFilePath = szPath;
		_server->_serverInitializationInfo.dwDefaultOperationTimeout = (DWORD)nDefaultTcpTimeout;

		//Buffer size
		if(nBufferSize > 16 && nBufferSize < 0x7FFFFFFF)
			_server->_serverInitializationInfo.dwBufferSize = nBufferSize;

		//Initial Buffer count
		if(nInitialBufferCount > 1 && nInitialBufferCount < 0x7FFFFFFF)
			_server->_serverInitializationInfo.dwBufferInitialCount = nInitialBufferCount;
		
		//Maximum buffer count
		if (nMaxBufferCount > 1 && nMaxBufferCount < 0x7FFFFFFF)
			_server->_serverInitializationInfo.dwBufferMaxCount = nMaxBufferCount;

		if (_server->_serverInitializationInfo.dwBufferMaxCount < _server->_serverInitializationInfo.dwBufferInitialCount)
			_server->_serverInitializationInfo.dwBufferMaxCount = _server->_serverInitializationInfo.dwBufferInitialCount;

		//Timer Interval
		_server->_serverInitializationInfo.dwTimerInterval = _serverInitializationInfo.dwTimerInterval;
		
		if (!_server->Initialize(GetModuleHandle(NULL), &_server->_serverInitializationInfo))
		{
		}
		else
		{
		}

		_server->StartServer();
		return 0;
	});
}

void CServerController::Terminate()
{
	if(_server->Terminate())
		_future.wait();
}

void CServerController::BeginTcpStreamServer(int nPort, std::wstring userParam, int nAcceptPost, long long nLimitClientCount)
{
	_server->BeginTcpServer(nPort, TcpServerTypeStream, userParam.c_str(), nAcceptPost, nLimitClientCount);
}

void CServerController::BeginHttpServer(int nPort, std::wstring userParam, int nAcceptPost, long long nLimitClientCount)
{
	_server->BeginTcpServer(nPort, TcpServerTypeHttp, userParam.c_str(), nAcceptPost, nLimitClientCount);
}

void CServerController::BeginTcpServer2(int nPort, std::wstring userParam, int nAcceptPost, long long nLimitClientCount)
{
	_server->BeginTcpServer(nPort, TcpServerTypeBinaryHeader2, userParam.c_str(), nAcceptPost, nLimitClientCount);
}

void CServerController::BeginTcpServer4(int nPort, std::wstring userParam, int nAcceptPost, long long nLimitClientCount)
{
	_server->BeginTcpServer(nPort, TcpServerTypeBinaryHeader4, userParam.c_str(), nAcceptPost, nLimitClientCount);
}

void CServerController::BeginTcpServerV(int nPort, std::wstring userParam, int nAcceptPost, long long nLimitClientCount)
{
	_server->BeginTcpServer(nPort, TcpServerTypeBinaryHeaderV, userParam.c_str(), nAcceptPost, nLimitClientCount);
}

void CServerController::BeginUdpServer(int nPort, std::wstring userParam)
{
	_server->BeginUdpServer(nPort, 0, userParam.c_str());
}

void CServerController::DestroyTcpServer(int nPort)
{
	_server->KillTcpServer(nPort);
}

void CServerController::DestroyUdpServer(int nPort)
{
	_server->KillUdpServer(nPort);
}

long CServerController::CreateTcpStreamConnection(std::wstring address, int nPort, std::wstring userParam)
{
	return _server->CreateTcpConnection(address.c_str(), nPort, TcpConnectionTypeStream, userParam.c_str(), AF_INET, TRUE);
}

long CServerController::PendingTcpStreamConnection(std::wstring address, int nPort, std::wstring userParam)
{
	return _server->PendingTcpConnection(address.c_str(), nPort, TcpConnectionTypeStream, userParam.c_str(), AF_INET, TRUE, NULL);
}

long CServerController::PendingTcpConnectionV(std::wstring address, int nPort, std::wstring userParam)
{
	return _server->PendingTcpConnection(address.c_str(), nPort, TcpConnectionTypeBinaryHeaderV, userParam.c_str(), AF_INET, TRUE, NULL);
}

void CServerController::SendStringToClient(__int64 nClientUID, std::string data)
{
	_server->SendRawResponseData(nClientUID, (void*)data.c_str(), data.size());
}

void CServerController::SendStringToUdpClient(int nServerPort, std::string clientAddress, int nClientPort, std::string data)
{
	USES_CONVERSION;
	std::wstring targetAddressW = (LPCTSTR)ATL::CA2W(clientAddress.c_str(), CP_UTF8);

	_server->SendUdpData(nServerPort, (LPVOID)data.c_str(), data.size(), targetAddressW.c_str(), nClientPort);
}

void CServerController::SendStringToUdpClientEx(int nServerPort, std::string clientAddressEx, std::string data)
{
	USES_CONVERSION;
	std::wstring targetAddressW = (LPCTSTR)ATL::CA2W(clientAddressEx.c_str(), CP_UTF8);

	UINT nClientPort = 0;
	TCHAR *ptrSplitter = (TCHAR*)_tcschr(targetAddressW.c_str(), _T(':'));
	if (ptrSplitter)
	{
		*ptrSplitter = 0;
		nClientPort = _ttoi(ptrSplitter + 1);
		_server->SendUdpData(nServerPort, (LPVOID)data.c_str(), data.size(), targetAddressW.c_str(), nClientPort);
	}
}

void CServerController::SendWebSocketStringToClient(__int64 nClientUID, std::string data)
{
	_server->SendWebSocketResponseData(nClientUID, (void*)data.c_str(), data.size());
}

void CServerController::SendPackageToClient(__int64 nClientUID, std::shared_ptr<CSTXProtocolLua> spData)
{
	_server->SendRawResponseData(nClientUID, (void*)spData->_protocol.GetBasePtr(), spData->_protocol.GetDataLen());
}

void CServerController::DisconnectClient(__int64 nClientUID)
{
	_server->DisconnectTcpClient(nClientUID);
}

void CServerController::SetTcpClientRole(__int64 nClientUID, int nRole)
{
	_server->SetTcpClientRole(nClientUID, (UniversalTcpClientRole)nRole);
}

int CServerController::GetTcpClientRole(__int64 nClientUID)
{
	return _server->GetTcpClientRole(nClientUID);
}

void CServerController::SetTcpClientTimeout(__int64 nClientUID, unsigned int nTimeout)
{
	_server->SetTcpClientTimeout(nClientUID, nTimeout);
}

void CServerController::SetRPCPort(unsigned int nPort)
{
	_server->SetRPCServerPort(nPort);
}

void CServerController::StartRPC(int nPort)
{
	if(nPort > 0)
		_server->CreateServerRPCThread((UINT)nPort);
	else
		_server->CreateServerRPCThread();
}

void CServerController::StopRPC()
{
	_server->StopServerRPC();
}

void CServerController::SendStringToConnection(long nConnectionID, std::string data)
{
	if (data.empty())
	{
		return;
	}
	_server->SendTcpData(nConnectionID, (LPVOID)data.c_str(), data.size() * sizeof(TCHAR));
}

void CServerController::SendPackageToConnection(long nConnectionID, std::shared_ptr<CSTXProtocolLua> spData)
{
	_server->SendTcpData(nConnectionID, (void*)spData->_protocol.GetBasePtr(), spData->_protocol.GetDataLen());
}

void CServerController::PutString(std::wstring key, std::wstring value)
{
	_datamap[key] = value;
}

std::wstring CServerController::GetString(std::wstring key)
{
	auto it = _datamap.find(key);
	if (it == _datamap.end())
		return _T("");

	return it->second;
}

void CServerController::Log(std::wstring logText, int nLogLevel)
{
	STXTRACELOGE(nLogLevel, _T("%s"), logText.c_str());
}

void CServerController::SetTcpServerReceivedScript(int nPort, std::wstring scriptFile)
{
	_server->SetTcpServerReceiveScript(nPort, scriptFile.c_str());
}

void CServerController::SetTcpConnectionReceivedScript(long nConnectionID, std::wstring scriptFile)
{
	_server->SetTcpConnectionReceiveScript(nConnectionID, scriptFile.c_str());
}

void CServerController::SetTcpConnectionDisconnectedScript(long nConnectionID, std::wstring scriptFile)
{
	_server->SetTcpConnectionDisconnectedScript(nConnectionID, scriptFile.c_str());
}

void CServerController::SetTcpServerClientConnectedScript(int nPort, std::wstring scriptFile)
{
	_server->SetTcpServerClientConnectedScript(nPort, scriptFile.c_str());
}

void CServerController::SetTcpServerClientDisconnectedScript(int nPort, std::wstring scriptFile)
{
	_server->SetTcpServerClientDisconnectedScript(nPort, scriptFile.c_str());
}

void CServerController::SetUdpServerReceivedScript(int nPort, std::wstring scriptFile)
{
	_server->SetUdpServerReceivedScript(nPort, scriptFile.c_str());
}

long CServerController::GetTcpClientCount(int port)
{
	return _server->GetTcpClientCount(port);
}

long long CServerController::GetSentBytesPerSecond()
{
	return _server->GetSentBytesPerSecond();
}

long long CServerController::GetSentCountPerSecond()
{
	return _server->GetSentCountPerSecond();
}

long long CServerController::GetReceivedBytesPerSecond()
{
	return _server->GetReceivedBytesPerSecond();
}

long long CServerController::GetReceivedCountPerSecond()
{
	return _server->GetReceivedCountPerSecond();
}

void CServerController::SetStatisticsLevel(unsigned int level)
{
	_server->SetStatisticsLevel(level);
}

unsigned int CServerController::GetStatisticsLevel()
{
	return _server->GetStatisticsLevel();
}

long long CServerController::GetTotalReceivedBytes()
{
	return _server->GetTotalReceivedBytes();
}

long long CServerController::GetTotalSentBytes()
{
	return _server->GetTotalSentBytes();
}

long long CServerController::GetTotalReceivedCount()
{
	return _server->GetTotalReceivedCount();
}

long long CServerController::GetTotalSentCount()
{
	return _server->GetTotalSentCount();
}

void CServerController::SetLogLevel(int level)
{
	_server->SetLogLevel(level);
}

void CServerController::SetDebugOutputLevel(int level)
{
	_server->SetDebugOutputLevel(level);
}

void CServerController::EnqueueWorkerThreadScriptString(LPCTSTR lpszScriptString)
{
	_server->EnqueueWorkerThreadScript(lpszScriptString);
}

void CServerController::AddFolderMonitorIgnoreFileExtension(long long monitorId, std::wstring fileExt)
{
	_server->AddFolderMonitorIgnoreFileExtension(monitorId, fileExt.c_str());
}

void CServerController::RemoveFolderMonitorIgnoreFileExtension(long long monitorId, std::wstring fileExt)
{
	_server->RemoveFolderMonitorIgnoreFileExtension(monitorId, fileExt.c_str());
}

long long CServerController::GetDefaultFolderMonitorId()
{
	return _server->GetDefaultFolderMonitorId();
}

unsigned int CServerController::GetTimerInterval()
{
	return _server->GetTimerInterval();
}

void CServerController::SetTimerInterval(unsigned int interval)
{
	_serverInitializationInfo.dwTimerInterval = interval;
	ChangeTimerInterval(interval);
}

void CServerController::SetTimerScript(std::wstring scriptFile)
{
	_server->SetTimerScript(scriptFile.c_str());
}

void CServerController::SetWorkerThreadInitializationScript(std::wstring scriptFile)
{
	_server->SetWorkerThreadInitializationScript(scriptFile.c_str());
}

void CServerController::ChangeTimerInterval(unsigned int interval)
{
	_server->ChangeTimerInterval(interval);
}

long long CServerController::GetNextClientUID()
{
	return _server->GetNextClientUID();
}

void CServerController::SetFileChangedScript(std::wstring scriptFile)
{
	_server->SetFileChangedScript(scriptFile.c_str());
}

size_t CServerController::GetWorkerThreadScriptCapacity()
{
	return _server->GetWorkerThreadScriptCapacity();
}

size_t CServerController::GetWorkerThreadScriptUsage()
{
	return _server->GetWorkerThreadScriptUsage();
}
