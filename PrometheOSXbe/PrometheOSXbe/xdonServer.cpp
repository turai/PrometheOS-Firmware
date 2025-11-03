#include "XDON/definitions.h"
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
	LONG connectedClients; // TODO Shut down other services that access the HDD
	HANDLE mServerThreadHandle;
	uint64_t sListenSock;
	uint64_t sIdSock;
	xdonServer::XDONClientData fakeClientData;
	DeviceInfo devices[] = {
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
		{"\\Device\\CdRom0", INVALID_HANDLE_VALUE, NULL}};
		bool memoryUnits[] = {
			false, false, false, false,
				false, false, false, false};
}

// https://www.cs.uaf.edu/2017/fall/cs301/lecture/10_06_string_inst.html
bool isZero(uint8_t* buffer, int len) {
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

bool WINAPI xdonServer::serverThread(LPVOID lParam)
{
	const timeval timeout = {0, 0};
	fd_set fds;
	int result;
	struct sockaddr_in sender;
	fakeClientData.sock = (SOCKET)sIdSock;
	fakeClientData.requestMemory = (uint8_t *)XMemAlloc(XDON_REQ_MEM_SIZE, PHYSICAL_MEMORY_ATTRS);
	fakeClientData.responseMemory = (uint8_t *)XMemAlloc(XDON_RSP_MEM_SIZE, PHYSICAL_MEMORY_ATTRS);
	while (mStopRequested == false)
	{
		FD_ZERO(&fds);
		FD_SET((SOCKET)sIdSock, &fds);
		FD_SET((SOCKET)sListenSock, &fds);
		result = select((int)max(sIdSock, sListenSock) + 1, &fds, NULL, NULL, &timeout);
		if (result == SOCKET_ERROR)
		{
			utils::debugPrint("Error waiting for sockets: %i\n", WSAGetLastError());
			break;
		}
		if (FD_ISSET(sListenSock, &fds))
		{
			result = accept((SOCKET)sListenSock, NULL, NULL);
			if (result == INVALID_SOCKET)
			{
				continue;
			}
			uint64_t clientSock = result;
			int timeout = 0;
			setsockopt((SOCKET)clientSock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
			setsockopt((SOCKET)clientSock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));

			XDONClientData *clientData = (XDONClientData *)XMemAlloc(sizeof(XDONClientData), HEAP_MEMORY_ATTRS);
			if (clientData == NULL)
			{
				continue;
			}
			clientData->sock = (SOCKET)clientSock;
			clientData->requestMemory = (uint8_t *)XMemAlloc(XDON_REQ_MEM_SIZE, PHYSICAL_MEMORY_ATTRS);
			if (clientData->requestMemory == NULL)
			{
				XMemFree(clientData, HEAP_MEMORY_ATTRS);
				socketUtility::closeSocket(clientSock);
				continue;
			}
			clientData->responseMemory = (uint8_t *)XMemAlloc(XDON_RSP_MEM_SIZE, PHYSICAL_MEMORY_ATTRS);
			if (clientData->responseMemory == NULL)
			{
				XMemFree(clientData->requestMemory, PHYSICAL_MEMORY_ATTRS);
				XMemFree(clientData, HEAP_MEMORY_ATTRS);
				socketUtility::closeSocket(clientSock);
				continue;
			}
			HANDLE clientHandler = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)clientThread, clientData, 0, NULL);
			if (clientHandler == NULL)
			{
				XMemFree(clientData->requestMemory, PHYSICAL_MEMORY_ATTRS);
				XMemFree(clientData->responseMemory, PHYSICAL_MEMORY_ATTRS);
				XMemFree(clientData, HEAP_MEMORY_ATTRS);
				socketUtility::closeSocket(clientSock);
				continue;
			}
			SetThreadPriority(clientHandler, THREAD_PRIORITY_ABOVE_NORMAL);
			CloseHandle(clientHandler);
			continue;
		}
		else if (FD_ISSET(sIdSock, &fds))
		{
			result = receiveAndValidateRequest(&fakeClientData, &sender, sizeof(sender));
			if (result < 0)
			{
				XDONResponse *response = (XDONResponse *)fakeClientData.responseMemory;
				response->identifier = XDON_RSP_FRAME_IDENTIFIER;
				response->version = XDON_PROTOCOL_VERSION;
				response->consoleType = XDON_CONSOLE_TYPE;
				response->statusCode = result;
				result = networkSendUDP((SOCKET)sIdSock, (uint8_t *)response, sizeof(XDONResponse), &sender);
				if (result < 0)
				{
					utils::debugPrint("Error sending validation failure response: %i\n", WSAGetLastError());
				}
				continue;
			}
			processRequest(&fakeClientData, &sender, sizeof(sender), (XDONCommand)result);
			continue;
		}
		Sleep(50);
	}
	socketUtility::closeSocket(sIdSock);
	socketUtility::closeSocket(sListenSock);
	XMemFree(fakeClientData.requestMemory, PHYSICAL_MEMORY_ATTRS);
	XMemFree(fakeClientData.responseMemory, PHYSICAL_MEMORY_ATTRS);
	return false;
}

bool WINAPI xdonServer::clientThread(LPVOID lParam)
{
	InterlockedIncrement(&connectedClients);
	int result;
	XDONClientData *clientData = (XDONClientData *)lParam;
	while (mStopRequested == false)
	{
		result = receiveAndValidateRequest(clientData, NULL, 0);
		if (result < 0)
		{
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
		processRequest(clientData, NULL, 0, (XDONCommand)result);
		if (WSAGetLastError() == WSAECONNRESET)
		{
			utils::debugPrint("Client disconnected");
			break;
		}
		SwitchToThread();
	}
	InterlockedDecrement(&connectedClients);
	socketUtility::closeSocket(clientData->sock);
	XMemFree(clientData->requestMemory, PHYSICAL_MEMORY_ATTRS);
	XMemFree(clientData->responseMemory, PHYSICAL_MEMORY_ATTRS);
	XMemFree(clientData, HEAP_MEMORY_ATTRS);
	return false;
}

inline bool xdonServer::isDeviceOpen(DeviceIndex device) {
	return devices[device].mutex != NULL && devices[device].handle != INVALID_HANDLE_VALUE;
}

NTSTATUS xdonServer::openDevice(DeviceIndex device) {
	if (device >= XDON_DEVICE_MAX) {
		return -EINVAL;
	}
	struct DeviceInfo* deviceInfo = &devices[device];
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

bool xdonServer::init()
{
	mStopRequested = false;
	mServerThreadHandle = NULL;

	struct sockaddr_in saListen;
	memset(&saListen, 0, sizeof(SOCKADDR_IN));
	saListen.sin_family = AF_INET;
	saListen.sin_addr.S_un.S_addr = INADDR_ANY;
	saListen.sin_port = htons(1000);
	socketUtility::createSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, sListenSock);
	if (!socketUtility::bindSocket(sListenSock, &saListen))
	{
		socketUtility::closeSocket(sListenSock);
		return false;
	}
	socketUtility::listenSocket(sListenSock, SOMAXCONN);

	if (!socketUtility::createSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, sIdSock))
	{
		utils::debugPrint("Error creating XDON UDP socket: %i\n", WSAGetLastError());
		return false;
	}
	if (!socketUtility::bindSocket(sIdSock, &saListen))
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
	mStopRequested = true;
	WaitForSingleObject(mServerThreadHandle, INFINITE);
	CloseHandle(mServerThreadHandle);
}

int xdonServer::receiveAndValidateRequest(XDONClientData *clientData, sockaddr_in *sender, size_t senderSize)
{
	XDONRequest *request = (XDONRequest *)clientData->requestMemory;
	if (networkRead((SOCKET)clientData->sock, (uint8_t *)request, sizeof(XDONRequest), sender) < 0)
	{
		return -1;
	}
	if (request->identifier != XDON_REQ_FRAME_IDENTIFIER)
	{
		utils::debugPrint("Invalid frame identifier received!\n");
		return -1;
	}
	if (request->version != XDON_PROTOCOL_VERSION)
	{
		utils::debugPrint("Unsupported XDON protocol!\n");
		return -1;
	}
	if (sender != NULL && request->command != Identify)
	{
		utils::debugPrint("UDP only supports identify commands!\n");
		return -1;
	}
	int requestSize;
	uint8_t command = request->command;
	switch (command)
	{
	case Identify:
	case GetDevices:
		return request->command;
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
	if (networkRead((SOCKET)clientData->sock, clientData->requestMemory, requestSize, sender) < 0)
	{
		return -1;
	}
	switch (command)
	{
	case Read:
		{
			XDONReadRequest *request = (XDONReadRequest *)clientData->requestMemory;
			if (request->device >= XDON_DEVICE_MAX || request->length == 0 || request->length > XDON_MAX_IO_SIZE)
			{
				utils::debugPrint("bad read req off=%d len=%d!\n", request->offset, request->length);
				return -1;
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
			if (networkRead((SOCKET)clientData->sock, request->data, request->length, sender) < 0)
			{
				utils::debugPrint("err read wdata\n", request->offset, request->length);
				return -1;
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
				if (networkRead((SOCKET)clientData->sock, request->data, request->length, sender) < 0)
				{
					return -1;
				}
			}
			break;
		}
	case RebootShutdown:
		break;
	default: // Unreachable
		return -1;
	}
	return command;
}

void xdonServer::processRequest(XDONClientData *clientData, sockaddr_in *sender, size_t senderSize, XDONCommand command)
{
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
			XDONReadRequest *request = (XDONReadRequest*)clientData->requestMemory;
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
			XDONWriteRequest *request = (XDONWriteRequest*)clientData->requestMemory;
			response->statusCode = writeDevice((DeviceIndex)request->device, request->offset, request->data, request->length);
			if (response->statusCode < 0) {
				utils::debugPrint("write status ERR=%d off=%d len=%d\n", response->statusCode, request->offset, request->length);
			}
			break;
		}
	case WriteSame:
		{
			XDONWriteSameRequest *request = (XDONWriteSameRequest*)clientData->requestMemory;
			uint8_t value = request->value;
			int size = request->length;
			uint64_t offset = request->offset;
			memset(clientData->requestMemory, value, size);
			response->statusCode = writeDevice((DeviceIndex)request->device, offset, (uint8_t*)request, size);
			break;
		}
	case ATACommand:
		{
			XDONATACommandRequest* request = (XDONATACommandRequest*)clientData->requestMemory;
			XDONATACommandResponse* payload = (XDONATACommandResponse*)clientData->responseMemory;
			offset += sizeof(XDONATACommandResponse);
			memset(payload, 0, sizeof(XDONATACommandResponse));
			ATA_PASS_THROUGH ataPassthrough;
			memcpy(&ataPassthrough.IdeReg, &request->registers, sizeof(request->registers));
			ataPassthrough.DataBuffer = request->hasOutgoingData ? request->data : payload->data;
			ataPassthrough.DataBufferSize = request->length;
			IO_STATUS_BLOCK ioStatusBlock;
			response->statusCode = NtDeviceIoControlFile(devices[XDON_DEVICE_HARD_DISK_0].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_IDE_PASS_THROUGH, &ataPassthrough, sizeof(ataPassthrough), &ataPassthrough, sizeof(ataPassthrough));
			if (response->statusCode >= 0) {
				memcpy(&payload->registers, &ataPassthrough.IdeReg, sizeof(ataPassthrough.IdeReg));
				offset += request->hasOutgoingData ? 0 : request->length;
			}
			break;
		}
	case RebootShutdown:
		{
			/* PXDON_COMMAND_REBOOT_SHUTDOWN_CONSOLE_REQUEST request = (PXDON_COMMAND_REBOOT_SHUTDOWN_CONSOLE_REQUEST)ThreadParam->Memories.RequestMemory;
				Print(PRINT_VERBOSITY_FLAG_REQUESTS, "FulfillRequest (%X): Fulfilling XDON_COMMAND_REBOOT_SHUTDOWN_CONSOLE.", ThreadParam->ClientSocket);
				frame.StatusCode = STATUS_SUCCESS;

				if (!SockSend(ThreadParam->ClientSocket, &frame, sizeof(frame), NULL, 0, To, ToLen)) Print(PRINT_VERBOSITY_FLAG_ESSENTIAL_AND_ERRORS, "FulfillRequest (%X): Failed to send response: %d", ThreadParam->ClientSocket, WSAGetLastError());

				Print(PRINT_VERBOSITY_FLAG_REQUESTS, "FulfillRequest (%X): Executing routine %d right now.", ThreadParam->ClientSocket, request->Routine);
                //Let's use max to determine whether to shutdown. There is no other valid use for it so we can use it to keep the command the same as the Xbox 360 version.
                if ((FIRMWARE_REENTRY)request->Routine == HalMaximumRoutine) HalInitiateShutdown();
				else HalReturnToFirmware((FIRMWARE_REENTRY)request->Routine);*/
			break;
		}
	default:
		utils::debugPrint("!! TODO !! Command %d\n", command);
		break;
	}
	if (sender != NULL)
	{
		if (networkSendUDP((SOCKET)clientData->sock, clientData->responseMemory, offset, sender) < 0)
		{
			utils::debugPrint("Error sending UDP response to the client: %i\n", WSAGetLastError());
		}
	}
	else if (networkSendTCP((SOCKET)clientData->sock, clientData->responseMemory, offset) < 0)
	{
		utils::debugPrint("Error sending response to the client: %i\n", WSAGetLastError());
	}
}

int xdonServer::networkRead(SOCKET sock, uint8_t *outBuf, int outBufSize, struct sockaddr_in *sender)
{
	int recvSize = 0;
	int result, fromLen = sizeof(*sender);
	while (recvSize < outBufSize)
	{
		result = recvfrom(sock, (char *)(outBuf + recvSize), outBufSize - recvSize, 0, (struct sockaddr *)sender, &fromLen);
		if (result < 0)
		{
			return result;
		}
		recvSize += (size_t)result;
	}
	return 0;
}

int xdonServer::networkSendUDP(SOCKET sock, uint8_t *buffer, int bufferLen, struct sockaddr_in *sender)
{
	if (sender == NULL)
	{
		return -1;
	}
	if (bufferLen > 1472)
	{
		utils::debugPrint("Tried to send a huge %d bytes long UDP packet, ignoring.", bufferLen);
		return -1;
	}
	int sentLen = 0, result;
	while (sentLen < bufferLen)
	{
		result = sendto(sock, (const char *)(buffer + sentLen), bufferLen - sentLen, 0, (const sockaddr *)sender, sizeof(*sender));
		if (result < 0)
		{
			return result;
		}
		sentLen += result;
	}
	return 0;
}

int xdonServer::networkSendTCP(SOCKET sock, uint8_t *buffer, int bufferLen)
{
	int sentLen = 0, result;
	while (sentLen < bufferLen)
	{
		result = send(sock, (const char *)(buffer + sentLen), bufferLen - sentLen, 0);
		if (result < 0)
		{
			return result;
		}
		sentLen += result;
	}
	return 0;
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
		while (WaitForSingleObject(devices[i].mutex, 250) != WAIT_OBJECT_0) {
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
		if (memoryUnits[i])
		{
			MU_CloseDeviceObject(i / 2, XDEVICE_TOP_SLOT);
			memoryUnits[i] = false;
		}
		status = MU_CreateDeviceObject(i / 2, XDEVICE_TOP_SLOT, &str);
		memoryUnits[i] = status >= 0;
		if (memoryUnits[i + 1])
		{
			MU_CloseDeviceObject(i / 2, XDEVICE_TOP_SLOT);
			memoryUnits[i + 1] = false;
		}
		status = MU_CreateDeviceObject(i / 2, XDEVICE_BOTTOM_SLOT, &str);
		memoryUnits[i + 1] = status >= 0;
	}
	IO_STATUS_BLOCK ioStatusBlock;
	if (devices[XDON_DEVICE_HARD_DISK_0].handle != INVALID_HANDLE_VALUE)
	{
		status = NtDeviceIoControlFile(devices[XDON_DEVICE_HARD_DISK_0].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &payload->hardDrive0Geometry, sizeof(DISK_GEOMETRY));
		if (status >= 0)
		{
			payload->availableDevices.hardDrive0 = true;
			ATA_PASS_THROUGH ataPassthrough;
			memset(&ataPassthrough.IdeReg, 0, sizeof(ataPassthrough.IdeReg));
			ataPassthrough.IdeReg.bCommandReg = IDE_COMMAND_IDENTIFY_DEVICE;
			ataPassthrough.DataBuffer = &payload->hardDrive0Info;
			ataPassthrough.DataBufferSize = sizeof(payload->hardDrive0Info);
			status = NtDeviceIoControlFile(devices[XDON_DEVICE_HARD_DISK_0].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_IDE_PASS_THROUGH, &ataPassthrough, sizeof(ataPassthrough), &ataPassthrough, sizeof(ataPassthrough));
			if (status >= 0)
			{
				payload->hardDrive0InfoAvailable = true;
			}
		}
		else
		{
			NtClose(devices[XDON_DEVICE_HARD_DISK_0].handle);
			devices[XDON_DEVICE_HARD_DISK_0].handle = INVALID_HANDLE_VALUE;
		}
	}
	if (devices[XDON_DEVICE_HARD_DISK_1].handle != INVALID_HANDLE_VALUE)
	{
		status = NtDeviceIoControlFile(devices[XDON_DEVICE_HARD_DISK_1].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &payload->hardDrive1Geometry, sizeof(DISK_GEOMETRY));
		if (status >= 0)
		{
			payload->availableDevices.hardDrive1 = true;
			ATA_PASS_THROUGH ataPassthrough;
			memset(&ataPassthrough.IdeReg, 0, sizeof(ataPassthrough.IdeReg));
			ataPassthrough.IdeReg.bCommandReg = IDE_COMMAND_IDENTIFY_DEVICE;
			ataPassthrough.DataBuffer = &payload->hardDrive1Info;
			ataPassthrough.DataBufferSize = sizeof(payload->hardDrive1Info);
			status = NtDeviceIoControlFile(devices[XDON_DEVICE_HARD_DISK_1].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_IDE_PASS_THROUGH, &ataPassthrough, sizeof(ataPassthrough), &ataPassthrough, sizeof(ataPassthrough));
			if (status >= 0)
			{
				payload->hardDrive1InfoAvailable = true;
			}
		}
		else
		{
			NtClose(devices[XDON_DEVICE_HARD_DISK_1].handle);
			devices[XDON_DEVICE_HARD_DISK_1].handle = INVALID_HANDLE_VALUE;
		}
	}
	status = NtDeviceIoControlFile(devices[XDON_DEVICE_MU0].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &payload->memoryUnit0Geometry, sizeof(DISK_GEOMETRY));
	payload->availableDevices.memoryUnit0 = status >= 0;
	status = NtDeviceIoControlFile(devices[XDON_DEVICE_MU1].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &payload->memoryUnit1Geometry, sizeof(DISK_GEOMETRY));
	payload->availableDevices.memoryUnit1 = status >= 0;
	status = NtDeviceIoControlFile(devices[XDON_DEVICE_MU2].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &payload->memoryUnit2Geometry, sizeof(DISK_GEOMETRY));
	payload->availableDevices.memoryUnit2 = status >= 0;
	status = NtDeviceIoControlFile(devices[XDON_DEVICE_MU3].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &payload->memoryUnit3Geometry, sizeof(DISK_GEOMETRY));
	payload->availableDevices.memoryUnit3 = status >= 0;
	status = NtDeviceIoControlFile(devices[XDON_DEVICE_MU4].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &payload->memoryUnit4Geometry, sizeof(DISK_GEOMETRY));
	payload->availableDevices.memoryUnit4 = status >= 0;
	status = NtDeviceIoControlFile(devices[XDON_DEVICE_MU5].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &payload->memoryUnit5Geometry, sizeof(DISK_GEOMETRY));
	payload->availableDevices.memoryUnit5 = status >= 0;
	status = NtDeviceIoControlFile(devices[XDON_DEVICE_MU6].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &payload->memoryUnit6Geometry, sizeof(DISK_GEOMETRY));
	payload->availableDevices.memoryUnit6 = status >= 0;
	status = NtDeviceIoControlFile(devices[XDON_DEVICE_MU7].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &payload->memoryUnit7Geometry, sizeof(DISK_GEOMETRY));
	payload->availableDevices.memoryUnit7 = status >= 0;
	if (devices[10].handle != INVALID_HANDLE_VALUE)
	{
		status = NtDeviceIoControlFile(devices[XDON_DEVICE_DVDROM].handle, NULL, NULL, NULL, &ioStatusBlock, IOCTL_CDROM_GET_DRIVE_GEOMETRY, NULL, 0, &payload->dvdRomGeometry, sizeof(DISK_GEOMETRY));
		payload->availableDevices.dvdRom = status >= 0;
		if (!payload->availableDevices.dvdRom)
		{
			NtClose(devices[10].handle);
			devices[10].handle = INVALID_HANDLE_VALUE;
		}
	}
	for (int i = 0; i < 11; i++)
	{
		ReleaseMutex(devices[i].mutex);
	}
}

int xdonServer::readDevice(DeviceIndex device, uint64_t offset_, uint8_t *buffer, int bufferLen)
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
	struct DeviceInfo* deviceInfo = &devices[device];
	while (WaitForSingleObject(devices[device].mutex, 250) != WAIT_OBJECT_0) {
		SwitchToThread();
	}
	IO_STATUS_BLOCK ioStatusBlock;
	LARGE_INTEGER offset;
	offset.QuadPart = offset_;
	status = NtReadFile(devices[device].handle, NULL, NULL, NULL, &ioStatusBlock, buffer, bufferLen, &offset);
	ReleaseMutex(devices[device].mutex);
	return status;
}

int xdonServer::writeDevice(DeviceIndex device, uint64_t offset_, uint8_t *buffer, int bufferLen)
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
	while (WaitForSingleObject(devices[device].mutex, 250) != WAIT_OBJECT_0) {
		SwitchToThread();
	}
	status = NtWriteFile(devices[device].handle, NULL, NULL, NULL, &ioStatusBlock, buffer, bufferLen, &offset);
	ReleaseMutex(devices[device].mutex);
	return status;
}
