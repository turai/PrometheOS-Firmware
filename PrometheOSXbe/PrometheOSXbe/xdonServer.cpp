#include "Scenes/sceneManager.h"
#include "Threads/hddFormat.h"
#include "Threads/hddLockUnlock.h"
#include "XDON/definitions.h"
#include "driveManager.h"
#include "ftpServer.h"
#include "xdonServer.h"
#include "utils.h"
#include "xboxinternals.h"

const DWORD PHYSICAL_MEMORY_ATTRS = MAKE_XALLOC_ATTRIBUTES(
	0, FALSE, FALSE, FALSE, eXALLOCAllocatorId_GameMin,
	XALLOC_PHYSICAL_ALIGNMENT_4K,
	XALLOC_MEMPROTECT_READWRITE,
	FALSE,
	XALLOC_MEMTYPE_PHYSICAL);

const DWORD HEAP_MEMORY_ATTRS = MAKE_XALLOC_ATTRIBUTES(
	0, FALSE, FALSE, FALSE, eXALLOCAllocatorId_GameMin,
	XALLOC_ALIGNMENT_16,
	XALLOC_MEMPROTECT_READWRITE,
	FALSE,
	XALLOC_MEMTYPE_HEAP);

namespace
{
	bool mStopRequested;
	LONG mConnectedClients;
	HANDLE mBytesReadLock;
	uint64_t mBytesRead;
	HANDLE mBytesWrittenLock;
	uint64_t mBytesWritten;
	HANDLE mServerThreadHandle;
	uint64_t mListenSock;
	uint64_t mIdSock;
	xdonServer::XDONClientData mFakeClientData;
}

DeviceInfo xdonServer::mDevices[11] = {
	{"\\Device\\Harddisk0\\partition0", INVALID_HANDLE_VALUE, NULL},
	{"\\Device\\Harddisk1\\partition0", INVALID_HANDLE_VALUE, NULL},
	{"\\Device\\MU_0", INVALID_HANDLE_VALUE, NULL},
	{"\\Device\\MU_1", INVALID_HANDLE_VALUE, NULL},
	{"\\Device\\MU_2", INVALID_HANDLE_VALUE, NULL},
	{"\\Device\\MU_3", INVALID_HANDLE_VALUE, NULL},
	{"\\Device\\MU_4", INVALID_HANDLE_VALUE, NULL},
	{"\\Device\\MU_5", INVALID_HANDLE_VALUE, NULL},
	{"\\Device\\MU_6", INVALID_HANDLE_VALUE, NULL},
	{"\\Device\\MU_7", INVALID_HANDLE_VALUE, NULL},
	{"\\Device\\CdRom0", INVALID_HANDLE_VALUE, NULL}
};
bool xdonServer::mMemoryUnits[8] = {false, false, false, false, false, false, false, false};

// https://www.cs.uaf.edu/2017/fall/cs301/lecture/10_06_string_inst.html
inline bool isZero(uint8_t* buffer, int len) {
	uint8_t isZero;
	__asm {
		mov edi, buffer
		mov ecx, len
		mov al, 0
		repe scasb
		setz isZero
	};
	return isZero != 0;
}

bool xdonServer::hasConnectedClients() {
	int clients = InterlockedCompareExchange(&mConnectedClients, 0, 0);
	return clients != 0;
}

int xdonServer::connectedClients() {
	return InterlockedCompareExchange(&mConnectedClients, 0, 0);
}

unsigned long long xdonServer::bytesRead()
{
	uint64_t bytesRead;
	WaitForSingleObject(mBytesReadLock, INFINITE);
	bytesRead = mBytesRead;
	ReleaseMutex(mBytesReadLock);
	return bytesRead;
}

unsigned long long xdonServer::bytesWritten()
{
	uint64_t bytesWritten;
	WaitForSingleObject(mBytesWrittenLock, INFINITE);
	bytesWritten = mBytesWritten;
	ReleaseMutex(mBytesWrittenLock);
	return bytesWritten;
}

bool WINAPI xdonServer::serverThread(LPVOID lParam)
{
	const timeval timeout = {0, 50000};
	fd_set fds;
	int result;
	struct sockaddr_in sender;
	mFakeClientData.sock = (SOCKET)mIdSock;
	mFakeClientData.requestMemory = (uint8_t *)XMemAlloc(XDON_REQ_MEM_SIZE, PHYSICAL_MEMORY_ATTRS);
	mFakeClientData.responseMemory = (uint8_t *)XMemAlloc(XDON_RSP_MEM_SIZE, PHYSICAL_MEMORY_ATTRS);
	while (mStopRequested == false)
	{
		FD_ZERO(&fds);
		FD_SET((SOCKET)mIdSock, &fds);
		FD_SET((SOCKET)mListenSock, &fds);
		result = select((int)max(mIdSock, mListenSock) + 1, &fds, NULL, NULL, &timeout);
		if (result == SOCKET_ERROR)
		{
			utils::debugPrint("Error waiting for sockets: %i\n", WSAGetLastError());
			break;
		}
		if (FD_ISSET(mListenSock, &fds))
		{
			if (hddLockUnlock::isActive()) {
				utils::debugPrint("HDD is being (un)locked, ignoring request from FATXplorer");
				continue;
			}
			if (hddFormat::isActive()) {
				utils::debugPrint("HDD is being (un)locked, ignoring request from FATXplorer");
				continue;
			}
			InterlockedIncrement(&mConnectedClients);
			result = accept((SOCKET)mListenSock, NULL, NULL);
			if (result == INVALID_SOCKET)
			{
				InterlockedDecrement(&mConnectedClients);
				continue;
			}
			uint64_t clientSock = result;
			u_long value = 1;
			ioctlsocket((SOCKET)clientSock, FIONBIO, &value);
			XDONClientData *clientData = (XDONClientData *)XMemAlloc(sizeof(XDONClientData), HEAP_MEMORY_ATTRS);
			if (clientData == NULL)
			{
				InterlockedDecrement(&mConnectedClients);
				continue;
			}
			clientData->sock = (SOCKET)clientSock;
			clientData->requestMemory = (uint8_t *)XMemAlloc(XDON_REQ_MEM_SIZE, PHYSICAL_MEMORY_ATTRS);
			if (clientData->requestMemory == NULL)
			{
				XMemFree(clientData, HEAP_MEMORY_ATTRS);
				socketUtility::closeSocket(clientSock);
				InterlockedDecrement(&mConnectedClients);
				continue;
			}
			clientData->responseMemory = (uint8_t *)XMemAlloc(XDON_RSP_MEM_SIZE, PHYSICAL_MEMORY_ATTRS);
			if (clientData->responseMemory == NULL)
			{
				XMemFree(clientData->requestMemory, PHYSICAL_MEMORY_ATTRS);
				XMemFree(clientData, HEAP_MEMORY_ATTRS);
				socketUtility::closeSocket(clientSock);
				InterlockedDecrement(&mConnectedClients);
				continue;
			}
			HANDLE clientHandler = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)clientThread, clientData, 0, NULL);
			if (clientHandler == NULL)
			{
				XMemFree(clientData->requestMemory, PHYSICAL_MEMORY_ATTRS);
				XMemFree(clientData->responseMemory, PHYSICAL_MEMORY_ATTRS);
				XMemFree(clientData, HEAP_MEMORY_ATTRS);
				socketUtility::closeSocket(clientSock);
				InterlockedDecrement(&mConnectedClients);
				continue;
			}
			SetThreadPriority(clientHandler, THREAD_PRIORITY_HIGHEST);
			CloseHandle(clientHandler);
			continue;
		}
		else if (FD_ISSET(mIdSock, &fds))
		{
			XDONRequest *request = (XDONRequest *)mFakeClientData.requestMemory;
			result = networkReadUDP((SOCKET)mFakeClientData.sock, (uint8_t *)request, sizeof(XDONRequest), &sender);
			if (result < 0)
			{
				return result;
			}
			if (hddLockUnlock::isActive()) {
				utils::debugPrint("HDD is being (un)locked, ignoring request from FATXplorer");
				continue;
			}
			if (hddFormat::isActive()) {
				utils::debugPrint("HDD is being (un)locked, ignoring request from FATXplorer");
				continue;
			}
			result = processRequest(&mFakeClientData, &sender, sizeof(sender));
			if (result < 0)
			{
				XDONResponse *response = (XDONResponse *)mFakeClientData.responseMemory;
				response->identifier = XDON_RSP_FRAME_IDENTIFIER;
				response->version = XDON_PROTOCOL_VERSION;
				response->consoleType = XDON_CONSOLE_TYPE;
				response->statusCode = result;
				result = networkSendUDP((SOCKET)mIdSock, (uint8_t *)response, sizeof(XDONResponse), &sender);
				if (result < 0)
				{
					utils::debugPrint("Error sending validation failure response: %i\n", WSAGetLastError());
				}
				continue;
			}
		}
	}
	socketUtility::closeSocket(mIdSock);
	socketUtility::closeSocket(mListenSock);
	XMemFree(mFakeClientData.requestMemory, PHYSICAL_MEMORY_ATTRS);
	XMemFree(mFakeClientData.responseMemory, PHYSICAL_MEMORY_ATTRS);
	return false;
}

bool WINAPI xdonServer::clientThread(LPVOID lParam)
{
	XDONClientData *clientData = (XDONClientData *)lParam;
	// Fine to terminate this late, write is not supported over UDP
	// We don't know if the user wants to write to the disk (TODO: Add read only mode!) and if the program
	// has to wait for the FTP server to terminate during a Write(Same) request then FATXplorer will
	// potentially time out
	ftpServer::close();
	driveManager::unmountAllDrives();
	sceneManager::lock();
	if (sceneManager::getSceneItem() != sceneItemXDONLockout) {
		sceneManager::pushScene(sceneItemXDONLockout);
	}
	sceneManager::unlock();
	int result;
	fd_set fds;
	fd_set xfds;
	while (mStopRequested == false)
	{
		XDONRequest *request = (XDONRequest *)clientData->requestMemory;
		result = networkReadTCP((SOCKET)clientData->sock, (uint8_t *)request, sizeof(XDONRequest));
		if (result == -WAIT_TIMEOUT) {
			continue;
		}
		if (result < 0)
		{
			utils::debugPrint("sock read err %d %d\n", result, WSAGetLastError());
			goto cleanup;
		}
		result = processRequest(clientData, NULL, 0);
		if (result < 0)
		{
			utils::debugPrint("processreq error %d", result);
			XDONResponse *response = (XDONResponse *)clientData->responseMemory;
			response->identifier = XDON_RSP_FRAME_IDENTIFIER;
			response->version = XDON_PROTOCOL_VERSION;
			response->consoleType = XDON_CONSOLE_TYPE;
			response->statusCode = result;
			result = networkSendTCP((SOCKET)clientData->sock, (uint8_t *)response, sizeof(XDONResponse));
			if (result < 0)
			{
				utils::debugPrint("Error sending validation failure response: %i\n", WSAGetLastError());
				break;
			}
			continue;
		}
		if (WSAGetLastError() == WSAECONNRESET)
		{
			utils::debugPrint("Client disconnected");
			break;
		}
		SwitchToThread();
	}
cleanup:
	InterlockedDecrement(&mConnectedClients);
	if (!hasConnectedClients()) {
		WaitForSingleObject(mBytesReadLock, INFINITE);
		mBytesRead = 0;
		ReleaseMutex(mBytesReadLock);
		WaitForSingleObject(mBytesWrittenLock, INFINITE);
		mBytesWritten = 0;
		ReleaseMutex(mBytesWrittenLock);
		sceneManager::lock();
		if (sceneManager::getSceneItem() == sceneItemXDONLockout) {
			sceneManager::popScene(sceneResultNone);
		}
		sceneManager::unlock();
		// TODO There's a possibility driveMounter::startThread(true) will be used in the future, must be tracked somehow
		driveManager::mountAllDrives();
		ftpServer::init();
	}
	socketUtility::closeSocket(clientData->sock);
	XMemFree(clientData->requestMemory, PHYSICAL_MEMORY_ATTRS);
	XMemFree(clientData->responseMemory, PHYSICAL_MEMORY_ATTRS);
	XMemFree(clientData, HEAP_MEMORY_ATTRS);
	return false;
}


bool xdonServer::init()
{
	mStopRequested = false;
	mServerThreadHandle = NULL;
	mBytesReadLock = CreateMutex(NULL, false, NULL);
	mBytesWrittenLock = CreateMutex(NULL, false, NULL);

	struct sockaddr_in saListen;
	memset(&saListen, 0, sizeof(SOCKADDR_IN));
	saListen.sin_family = AF_INET;
	saListen.sin_addr.S_un.S_addr = INADDR_ANY;
	saListen.sin_port = htons(1000);
	socketUtility::createSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, mListenSock);
	if (!socketUtility::bindSocket(mListenSock, &saListen))
	{
		socketUtility::closeSocket(mListenSock);
		return false;
	}
	socketUtility::listenSocket(mListenSock, SOMAXCONN);

	if (!socketUtility::createSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, mIdSock))
	{
		utils::debugPrint("Error creating XDON UDP socket: %i\n", WSAGetLastError());
		return false;
	}
	if (!socketUtility::bindSocket(mIdSock, &saListen))
	{
		utils::debugPrint("Error binding XDON UDP socket: %i\n", WSAGetLastError());
		return false;
	}
	mServerThreadHandle = CreateThread(0, 0, (LPTHREAD_START_ROUTINE)serverThread, 0, 0, NULL);
	if (mServerThreadHandle != NULL)
	{
		SetThreadPriority(mServerThreadHandle, 2);
	}
	return true;
}

void xdonServer::close()
{
	if (mServerThreadHandle != NULL) {
		mStopRequested = true;
		WaitForSingleObject(mServerThreadHandle, INFINITE);
		CloseHandle(mServerThreadHandle);
		for (int i = 0; i < XDON_DEVICE_MAX; i++) {
			if (mDevices[i].handle != INVALID_HANDLE_VALUE) {
				NtClose(mDevices[i].handle);
				mDevices[i].handle = INVALID_HANDLE_VALUE;
			}
			if (mDevices[i].mutex != NULL) {
				CloseHandle(mDevices[i].mutex);
				mDevices[i].mutex = NULL;
			}
		}
		mServerThreadHandle = NULL;
	}
	CloseHandle(mBytesReadLock);
	CloseHandle(mBytesWrittenLock);
}

inline int xdonServer::networkReadTCP(SOCKET sock, uint8_t* outBuf, u_long outBufSize)
{
	const DWORD FLAGS = 0;
	int result;
	u_long lastRecvMs = GetTickCount(), nowMs, cycRcvd = 0, totalRcvd = 0, bytesAvailable;
	timeval timeout;
	fd_set rfds, xfds;
	WSABUF recvBuf;
	memset(&timeout, 0, sizeof(timeval));
	do {
		nowMs = GetTickCount();
		recvBuf.buf = (char*)outBuf + totalRcvd;
		recvBuf.len = outBufSize - totalRcvd;
		ioctlsocket(sock, FIONREAD, &bytesAvailable);
		if (bytesAvailable == 0) {
			timeout.tv_usec = 16666;
			FD_ZERO(&rfds);
			FD_ZERO(&xfds);
			FD_SET(sock, &rfds);
			FD_SET(sock, &xfds);
			result = select(sock + 1, &rfds, NULL, &xfds, &timeout);
			if (result == SOCKET_ERROR) {
				return -WSAGetLastError();
			}
			if (FD_ISSET(sock, &xfds)) {
				return -EPIPE;
			}
			if (result == 0 || !FD_ISSET(sock, &rfds)) {
				if ((nowMs - lastRecvMs) > 60000) {
					return -WAIT_TIMEOUT;
				}
				continue;
			}
		}
		result = WSARecv(sock, &recvBuf, 1, &cycRcvd, (LPDWORD)&FLAGS, NULL, NULL);
		if (result == SOCKET_ERROR) {
			return -WSAGetLastError();
		}
		if (cycRcvd == 0) {
			return -EPIPE;
		}
		lastRecvMs = GetTickCount();
		totalRcvd += cycRcvd;
	} while (totalRcvd < outBufSize);
	return totalRcvd;
}

inline int xdonServer::networkSendTCP(SOCKET sock, uint8_t *buffer, int bufferLen)
{
	int result;
	u_long sentLen = 0, cycSent = 0;
	fd_set wfds;
	timeval timeout;
	WSABUF sendBuf;
	memset(&timeout, 0, sizeof(timeval));
	do {
		sendBuf.buf = (char*)buffer + sentLen;
		sendBuf.len = bufferLen - sentLen;
		timeout.tv_usec = 16666;
		FD_ZERO(&wfds);
		FD_SET(sock, &wfds);
		result = select(sock + 1, NULL, &wfds, NULL, &timeout);
		if (!FD_ISSET(sock, &wfds)) {
			continue;
		}
		result = WSASend(sock, &sendBuf, 1, &cycSent, 0, NULL, NULL);
		if (result == SOCKET_ERROR) {
			return -WSAGetLastError();
		}
		sentLen += cycSent;
	} while (sentLen < (u_long)bufferLen);
	return sentLen;
}

inline int xdonServer::networkReadUDP(SOCKET sock, uint8_t *outBuf, int outBufSize, struct sockaddr_in *sender)
{
	const DWORD FLAGS = 0;
	int cycRcvd = 0, totalRcvd = 0;
	int result, fromLen = sizeof(*sender);
	WSABUF recvBuf;
	recvBuf.buf = (char*)outBuf;
	recvBuf.len = outBufSize;
	do {
		result = WSARecvFrom(sock, &recvBuf, 1, (LPDWORD)&cycRcvd, (LPDWORD)&FLAGS, (struct sockaddr*) sender, &fromLen, NULL, NULL);
		if (result < 0)
		{
			return -WSAGetLastError();
		}
		totalRcvd += (size_t)cycRcvd;
	} while (totalRcvd < outBufSize);
	return totalRcvd;
}

inline int xdonServer::networkSendUDP(SOCKET sock, uint8_t *buffer, int bufferLen, struct sockaddr_in *sender)
{
	if (sender == NULL)
	{
		return -1;
	}
	// According to XDK docs
	if (bufferLen > 1304)
	{
		utils::debugPrint("Tried to send a huge %d bytes long UDP packet, ignoring.", bufferLen);
		return -1;
	}
	int result;
	u_long sentLen = 0, cycSent = 0;
	WSABUF sendBuf;
	do {
		sendBuf.buf = (char*)buffer + sentLen;
		sendBuf.len = bufferLen - sentLen;
		result = WSASendTo(sock, &sendBuf, 1, (LPDWORD)&sentLen, 0, (const sockaddr*)sender, sizeof(struct sockaddr_in), NULL, NULL);
		if (result == SOCKET_ERROR) {
			return -WSAGetLastError();
		}
		sentLen += cycSent;
	} while (sentLen < (u_long)bufferLen);
	return sentLen;
}


inline bool xdonServer::isDeviceOpen(DeviceIndex device) {
	return mDevices[device].mutex != NULL && mDevices[device].handle != INVALID_HANDLE_VALUE;
}

NTSTATUS xdonServer::openDevice(DeviceIndex device) {
	if (device >= XDON_DEVICE_MAX) {
		return -EINVAL;
	}
	struct DeviceInfo* deviceInfo = &mDevices[device];
	if (deviceInfo->mutex == NULL) {
		deviceInfo->mutex = CreateMutex(NULL, true, NULL);
	}
	NTSTATUS status;
	OBJECT_ATTRIBUTES objectAttrs;
	IO_STATUS_BLOCK ioStatusBlock;
	char deviceName[64];
	STRING str;
	str.Buffer = deviceName;
	str.Length = 0;
	str.MaximumLength = sizeof(deviceName) / sizeof(char) - 2;
	if (deviceInfo->handle != INVALID_HANDLE_VALUE) {
		NtClose(deviceInfo->handle);
		deviceInfo->handle = INVALID_HANDLE_VALUE;
	}
	RtlInitAnsiString(&str, deviceInfo->path);
	InitializeObjectAttributes(&objectAttrs, &str, OBJ_CASE_INSENSITIVE, NULL);
	status = NtOpenFile(&deviceInfo->handle, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &objectAttrs, &ioStatusBlock, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SYNCHRONOUS_IO_NONALERT);
	if (status < 0) {
		deviceInfo->handle = INVALID_HANDLE_VALUE;
	}
	ReleaseMutex(deviceInfo->mutex);
	return status;
}

void xdonServer::getDevices(XDONDevices *payload)
{
	for (int i = 0; i < XDON_DEVICE_MAX; i++)
	{
		if (!isDeviceOpen((DeviceIndex)i)) {
			if (openDevice((DeviceIndex)i) < 0) {
				continue;
			}
		}
		while (WaitForSingleObject(mDevices[i].mutex, 250) != WAIT_OBJECT_0) {
			SwitchToThread();
		}
	}
	NTSTATUS status;
	char deviceName[64];
	STRING str;
	str.Length = 0;
	str.MaximumLength = sizeof(deviceName) / sizeof(char) - 2;
	str.Buffer = deviceName;
	for (int i = 0; i < XDON_DEVICE_MUS_MAX - XDON_DEVICE_MUS_MIN; i += 2)
	{
		if (mMemoryUnits[i])
		{
			MU_CloseDeviceObject(i / 2, XDEVICE_TOP_SLOT);
			mMemoryUnits[i] = false;
		}
		status = MU_CreateDeviceObject(i / 2, XDEVICE_TOP_SLOT, &str);
		mMemoryUnits[i] = status >= 0;
		if (mMemoryUnits[i + 1])
		{
			MU_CloseDeviceObject(i / 2, XDEVICE_TOP_SLOT);
			mMemoryUnits[i + 1] = false;
		}
		status = MU_CreateDeviceObject(i / 2, XDEVICE_BOTTOM_SLOT, &str);
		mMemoryUnits[i + 1] = status >= 0;
	}
	IO_STATUS_BLOCK ioStatusBlock;
	if (mDevices[XDON_DEVICE_HARD_DISK_0].handle != INVALID_HANDLE_VALUE)
	{
		status = NtDeviceIoControlFile(mDevices[XDON_DEVICE_HARD_DISK_0].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &payload->hardDrive0Geometry, sizeof(DISK_GEOMETRY));
		if (status >= 0)
		{
			payload->availableDevices.hardDrive0 = true;
			ATA_PASS_THROUGH ataPassthrough;
			memset(&ataPassthrough.IdeReg, 0, sizeof(ataPassthrough.IdeReg));
			ataPassthrough.IdeReg.bCommandReg = IDE_COMMAND_IDENTIFY_DEVICE;
			ataPassthrough.DataBuffer = &payload->hardDrive0Info;
			ataPassthrough.DataBufferSize = sizeof(payload->hardDrive0Info);
			status = NtDeviceIoControlFile(mDevices[XDON_DEVICE_HARD_DISK_0].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_IDE_PASS_THROUGH, &ataPassthrough, sizeof(ataPassthrough), &ataPassthrough, sizeof(ataPassthrough));
			if (status >= 0)
			{
				payload->hardDrive0InfoAvailable = true;
			}
		}
		else
		{
			NtClose(mDevices[XDON_DEVICE_HARD_DISK_0].handle);
			mDevices[XDON_DEVICE_HARD_DISK_0].handle = INVALID_HANDLE_VALUE;
		}
	}
	if (mDevices[XDON_DEVICE_HARD_DISK_1].handle != INVALID_HANDLE_VALUE)
	{
		status = NtDeviceIoControlFile(mDevices[XDON_DEVICE_HARD_DISK_1].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &payload->hardDrive1Geometry, sizeof(DISK_GEOMETRY));
		if (status >= 0)
		{
			payload->availableDevices.hardDrive1 = true;
			ATA_PASS_THROUGH ataPassthrough;
			memset(&ataPassthrough.IdeReg, 0, sizeof(ataPassthrough.IdeReg));
			ataPassthrough.IdeReg.bCommandReg = IDE_COMMAND_IDENTIFY_DEVICE;
			ataPassthrough.DataBuffer = &payload->hardDrive1Info;
			ataPassthrough.DataBufferSize = sizeof(payload->hardDrive1Info);
			status = NtDeviceIoControlFile(mDevices[XDON_DEVICE_HARD_DISK_1].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_IDE_PASS_THROUGH, &ataPassthrough, sizeof(ataPassthrough), &ataPassthrough, sizeof(ataPassthrough));
			if (status >= 0)
			{
				payload->hardDrive1InfoAvailable = true;
			}
		}
		else
		{
			NtClose(mDevices[XDON_DEVICE_HARD_DISK_1].handle);
			mDevices[XDON_DEVICE_HARD_DISK_1].handle = INVALID_HANDLE_VALUE;
		}
	}
	status = NtDeviceIoControlFile(mDevices[XDON_DEVICE_MU0].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &payload->memoryUnit0Geometry, sizeof(DISK_GEOMETRY));
	payload->availableDevices.memoryUnit0 = status >= 0;
	status = NtDeviceIoControlFile(mDevices[XDON_DEVICE_MU1].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &payload->memoryUnit1Geometry, sizeof(DISK_GEOMETRY));
	payload->availableDevices.memoryUnit1 = status >= 0;
	status = NtDeviceIoControlFile(mDevices[XDON_DEVICE_MU2].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &payload->memoryUnit2Geometry, sizeof(DISK_GEOMETRY));
	payload->availableDevices.memoryUnit2 = status >= 0;
	status = NtDeviceIoControlFile(mDevices[XDON_DEVICE_MU3].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &payload->memoryUnit3Geometry, sizeof(DISK_GEOMETRY));
	payload->availableDevices.memoryUnit3 = status >= 0;
	status = NtDeviceIoControlFile(mDevices[XDON_DEVICE_MU4].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &payload->memoryUnit4Geometry, sizeof(DISK_GEOMETRY));
	payload->availableDevices.memoryUnit4 = status >= 0;
	status = NtDeviceIoControlFile(mDevices[XDON_DEVICE_MU5].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &payload->memoryUnit5Geometry, sizeof(DISK_GEOMETRY));
	payload->availableDevices.memoryUnit5 = status >= 0;
	status = NtDeviceIoControlFile(mDevices[XDON_DEVICE_MU6].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &payload->memoryUnit6Geometry, sizeof(DISK_GEOMETRY));
	payload->availableDevices.memoryUnit6 = status >= 0;
	status = NtDeviceIoControlFile(mDevices[XDON_DEVICE_MU7].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &payload->memoryUnit7Geometry, sizeof(DISK_GEOMETRY));
	payload->availableDevices.memoryUnit7 = status >= 0;
	if (mDevices[10].handle != INVALID_HANDLE_VALUE)
	{
		status = NtDeviceIoControlFile(mDevices[XDON_DEVICE_DVDROM].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_CDROM_GET_DRIVE_GEOMETRY, NULL, 0, &payload->dvdRomGeometry, sizeof(DISK_GEOMETRY));
		payload->availableDevices.dvdRom = status >= 0;
		if (!payload->availableDevices.dvdRom)
		{
			NtClose(mDevices[10].handle);
			mDevices[10].handle = INVALID_HANDLE_VALUE;
		}
	}
	for (int i = 0; i < 11; i++)
	{
		ReleaseMutex(mDevices[i].mutex);
	}
}

inline int xdonServer::readDevice(DeviceIndex device, uint64_t offset_, uint8_t *buffer, int bufferLen)
{
	NTSTATUS status;
	if (device > XDON_DEVICE_MAX)
	{
		return -EINVAL;
	}
	// This is required if FATXplorer didn't do a GetDevices call (eg. the user restarts prom after a refresh)
	if (!isDeviceOpen(device))
	{
		status = openDevice(device);
		if (status < 0) {
			return status;
		}
	}
	struct DeviceInfo* deviceInfo = &mDevices[device];
	while (WaitForSingleObject(mDevices[device].mutex, 250) != WAIT_OBJECT_0) {
		SwitchToThread();
	}
	IO_STATUS_BLOCK ioStatusBlock;
	LARGE_INTEGER offset;
	offset.QuadPart = offset_;
	status = NtReadFile(mDevices[device].handle, NULL, NULL, NULL, &ioStatusBlock, buffer, bufferLen, &offset);
	ReleaseMutex(mDevices[device].mutex);
	WaitForSingleObject(mBytesReadLock, INFINITE);
	mBytesRead += (uint64_t)ioStatusBlock.Information;
	ReleaseMutex(mBytesReadLock);
	return status;
}

inline int xdonServer::writeDevice(DeviceIndex device, uint64_t offset_, uint8_t *buffer, int bufferLen)
{
	NTSTATUS status;
	if (device > XDON_DEVICE_MAX)
	{
		return -EINVAL;
	}
	if (!isDeviceOpen(device))
	{
		status = openDevice(device);
		if (status < 0) {
			return status;
		}
	}
	IO_STATUS_BLOCK ioStatusBlock;
	LARGE_INTEGER offset;
	offset.QuadPart = offset_;
	while (WaitForSingleObject(mDevices[device].mutex, 250) != WAIT_OBJECT_0) {
		SwitchToThread();
	}
	status = NtWriteFile(mDevices[device].handle, NULL, NULL, NULL, &ioStatusBlock, buffer, bufferLen, &offset);
	ReleaseMutex(mDevices[device].mutex);
	WaitForSingleObject(mBytesWrittenLock, INFINITE);
	mBytesWritten += (uint64_t)ioStatusBlock.Information;
	ReleaseMutex(mBytesWrittenLock);
	return status;
}

int xdonServer::processRequest(XDONClientData *clientData, sockaddr_in *sender, size_t senderSize)
{
	XDONRequest *request = (XDONRequest *)clientData->requestMemory;
	if (request->identifier != XDON_REQ_FRAME_IDENTIFIER)
	{
		utils::debugPrint("Invalid frame identifier received!\n");
		return -EINVAL;
	}
	if (request->version != XDON_PROTOCOL_VERSION)
	{
		utils::debugPrint("Unsupported XDON protocol!\n");
		return -EINVAL;
	}
	if (sender != NULL && request->command != Identify)
	{
		utils::debugPrint("UDP only supports identify commands!\n");
		return -EINVAL;
	}
	int requestSize;
	uint8_t command = request->command;
	switch (command)
	{
	case Identify:
	case GetDevices:
		requestSize = 0;
		break;
	case Read:
		requestSize = sizeof(XDONReadRequest);
		break;
	case Write:
		requestSize = sizeof(XDONWriteRequest);
		break;
	case WriteSame:
		requestSize = sizeof(XDONWriteSameRequest);
		break;
	case ATACommand:
		requestSize = sizeof(XDONATACommandRequest);
		break;
	case RebootShutdown:
		requestSize = sizeof(XDONRebootShutdownRequest);
		break;
	default:
		utils::debugPrint("Unknown command: %d!\n", request->command);
		return -1;
	}
	if (requestSize > 0 && networkReadTCP((SOCKET)clientData->sock, clientData->requestMemory, requestSize) < 0)
	{
		return -1;
	}
	int offset = sizeof(XDONResponse);
	XDONResponse *response = (XDONResponse *)clientData->responseMemory;
	response->identifier = XDON_RSP_FRAME_IDENTIFIER;
	response->version = XDON_PROTOCOL_VERSION;
	response->consoleType = 0;
	switch (command)
	{
	case Identify:
		{
			XDONIdentity *payload = (XDONIdentity *)(clientData->responseMemory + offset);
			offset += sizeof(XDONIdentity);
			payload->xdonVersion = 1;
			payload->kernelVersion.major = 0x69;
			payload->kernelVersion.minor = 0x420;
			payload->kernelVersion.build = 0x4b45;
			payload->kernelVersion.qfe = 0x4b57;
			payload->hardwareInfo.flags = 0x50524f4d;
			payload->hardwareInfo.gpuRevision = 1;
			payload->hardwareInfo.mcpRevision = 1;
			payload->maxIOSize = XDON_MAX_IO_SIZE;
			response->statusCode = STATUS_SUCCESS;
			break;
		}
	case GetDevices:
		{
			XDONDevices *payload = (XDONDevices *)(clientData->responseMemory + offset);
			offset += sizeof(XDONDevices);
			memset(payload, 0, sizeof(XDONDevices));
			getDevices(payload);
			response->statusCode = STATUS_SUCCESS;
			break;
		}
	case Read:
		{
			XDONReadRequest *request = (XDONReadRequest *)clientData->requestMemory;
			if (request->device >= XDON_DEVICE_MAX || request->length == 0 || request->length > XDON_MAX_IO_SIZE)
			{
				utils::debugPrint("bad read req off=%d len=%d!\n", request->offset, request->length);
				return -1;
			}
			XDONReadResponse *payload = (XDONReadResponse *)(clientData->responseMemory + offset);
			offset += sizeof(XDONReadResponse);
			memset(payload, 0, sizeof(XDONReadResponse));
			response->statusCode = readDevice((DeviceIndex)request->device, request->offset, payload->data, request->length);
			if (response->statusCode >= 0)
			{
				payload->length = isZero(payload->data, request->length) ? 0 : request->length;
				payload->uncompressedLength = payload->length;
				offset += payload->length;
			} else {
				utils::debugPrint("read status ERR=%d off=%d len=%d\n", response->statusCode, request->offset, request->length);
			}
			break;
		}
	case Write:
		{
			XDONWriteRequest *request = (XDONWriteRequest *)clientData->requestMemory;
			if (request->device >= XDON_DEVICE_MAX || request->length == 0 || request->length > XDON_MAX_IO_SIZE)
			{
				utils::debugPrint("bad write req off=%d len=%d!\n", request->offset, request->length);
				return -1;
			}
			if (networkReadTCP((SOCKET)clientData->sock, request->data, request->length) < 0)
			{
				utils::debugPrint("err read wdata\n", request->offset, request->length);
				return -1;
			}
			response->statusCode = writeDevice((DeviceIndex)request->device, request->offset, request->data, request->length);
			if (response->statusCode < 0) {
				utils::debugPrint("write status ERR=%d off=%d len=%d\n", response->statusCode, request->offset, request->length);
			}
			break;
		}
	case WriteSame:
		{
			XDONWriteSameRequest *request = (XDONWriteSameRequest *)clientData->requestMemory;
			if (request->device >= XDON_DEVICE_MAX || request->length == 0 || request->length > XDON_MAX_IO_SIZE)
			{
				utils::debugPrint("bad write same req off=%d val=%d len=%d!\n", request->offset, request->value, request->length);
				return -1;
			}
			uint8_t value = request->value;
			int size = request->length;
			uint64_t offset = request->offset;
			memset(clientData->requestMemory, value, size);
			response->statusCode = writeDevice((DeviceIndex)request->device, offset, (uint8_t*)request, size);
			break;
		}
	case ATACommand:
		{
			XDONATACommandRequest *request = (XDONATACommandRequest *)clientData->requestMemory;
			if (request->length > XDON_MAX_IO_SIZE)
			{
				return -1;
			}
			if (request->hasOutgoingData && request->length > 0)
			{
				if (networkReadTCP((SOCKET)clientData->sock, request->data, request->length) < 0)
				{
					return -1;
				}
			}
			XDONATACommandResponse* payload = (XDONATACommandResponse*)clientData->responseMemory;
			offset += sizeof(XDONATACommandResponse);
			memset(payload, 0, sizeof(XDONATACommandResponse));
			ATA_PASS_THROUGH ataPassthrough;
			memcpy(&ataPassthrough.IdeReg, &request->registers, sizeof(request->registers));
			ataPassthrough.DataBuffer = request->hasOutgoingData ? request->data : payload->data;
			ataPassthrough.DataBufferSize = request->length;
			IO_STATUS_BLOCK ioStatusBlock;
			response->statusCode = NtDeviceIoControlFile(mDevices[XDON_DEVICE_HARD_DISK_0].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_IDE_PASS_THROUGH, &ataPassthrough, sizeof(ataPassthrough), &ataPassthrough, sizeof(ataPassthrough));
			if (response->statusCode >= 0) {
				memcpy(&payload->registers, &ataPassthrough.IdeReg, sizeof(ataPassthrough.IdeReg));
				offset += request->hasOutgoingData ? 0 : request->length;
			}
			break;
		}
	case RebootShutdown:
		{
			// TODO Send response before XDON goes offline
			XDONRebootShutdownRequest* request = (XDONRebootShutdownRequest*)clientData->requestMemory;
			response->statusCode = 0;
			switch (request->action) {
	case 1:
		HalReturnToFirmware(RETURN_FIRMWARE_REBOOT);
		break;
	case 2:
		HalReturnToFirmware(RETURN_FIRMWARE_QUICK_REBOOT);
		break;
	case 3:
		HalReturnToFirmware(RETURN_FIRMWARE_HARD);
		break;
	case 4:
		HalReturnToFirmware(RETURN_FIRMWARE_FATAL);
		break;
	default:
		HalReturnToFirmware(RETURN_FIRMWARE_HALT);
		break;
			}
			break;
		}
	default: // Unreachable
		return -1;
	}
	if (sender == NULL) {
		if (networkSendTCP((SOCKET)clientData->sock, clientData->responseMemory, offset) < 0)
		{
			utils::debugPrint("Error sending response to the client: %i\n", WSAGetLastError());
		}
	} else {
		if (networkSendUDP((SOCKET)clientData->sock, clientData->responseMemory, offset, sender) < 0)
		{
			utils::debugPrint("Error sending UDP response to the client: %i\n", WSAGetLastError());
		}
	}
	return 0;
}


