#pragma once

#include "socketUtility.h"
#include "XDON/structs.h"

class xdonServer
{
public:
	static bool init();
	static void close();

	typedef struct
	{
		uint64_t sock;
		uint8_t *requestMemory;
		uint8_t *responseMemory;
	} XDONClientData;

private:
	static bool WINAPI serverThread(LPVOID lParam);
	static bool WINAPI clientThread(LPVOID lParam);
	
	static bool isDeviceOpen(DeviceIndex device);
	static NTSTATUS openDevice(DeviceIndex device);

	static int receiveAndValidateRequest(XDONClientData *clientData, struct sockaddr_in *sender, size_t senderSize);
	static void processRequest(XDONClientData *clientData, struct sockaddr_in *sender, size_t senderSize, XDONCommand command);
	static int networkRead(SOCKET sock, uint8_t *buffer, int bufferLen, struct sockaddr_in *sender);
	static int networkSendUDP(SOCKET sock, uint8_t *buffer, int bufferLen, struct sockaddr_in *sender);
	static int networkSendTCP(SOCKET sock, uint8_t *buffer, int bufferLen);

	static void getDevices(XDONDevices *devices);
	static int readDevice(DeviceIndex device, uint64_t offset, uint8_t* buffer, int bufferLen);
	static int writeDevice(DeviceIndex device, uint64_t offset, uint8_t* buffer, int bufferLen);
};