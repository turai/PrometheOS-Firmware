#pragma once

#include "krnStructs.h"
#include "harddrive.h"
#include "xboxinternals.h"

typedef enum DeviceIndex
{
	XDON_DEVICE_HARD_DISK_0 = 0,
	XDON_DEVICE_HARD_DISK_1 = 1,
	XDON_DEVICE_MU0 = 2,
	XDON_DEVICE_MU1 = 3,
	XDON_DEVICE_MU2 = 4,
	XDON_DEVICE_MU3 = 5,
	XDON_DEVICE_MU4 = 6,
	XDON_DEVICE_MU5 = 7,
	XDON_DEVICE_MU6 = 8,
	XDON_DEVICE_MU7 = 9,
	XDON_DEVICE_DVDROM = 10,
	XDON_DEVICE_MAX = 11,
	XDON_DEVICE_MUS_MIN = 2,
	XDON_DEVICE_MUS_MAX = 9,
} DeviceIndex;

struct DeviceInfo
{
	char path[29];
	HANDLE handle;
	HANDLE mutex;
};

typedef enum XDONCommand
{
	UnknownCommand = 0,
	Identify = 1,
	GetDevices = 2,
	Read = 3,
	Write = 4,
	WriteSame = 5,
	ATACommand = 6,
	RebootShutdown = 7,
	XDON_COMMAND_MAX = 8
} XDONCommand;

#pragma pack(push, 1)

typedef struct
{
	uint64_t identifier;
	uint8_t version;
	uint8_t command;
} XDONRequest;

typedef struct
{
	uint64_t identifier;
	uint8_t version;
	uint8_t consoleType;
	int statusCode;
} XDONResponse;

typedef struct
{
	uint16_t major;
	uint16_t minor;
	uint16_t build;
	uint16_t qfe;
} XBOXKernelVersion;

typedef struct
{
	uint32_t flags;
	uint8_t gpuRevision;
	uint8_t mcpRevision;
	uint8_t reserved[2];
} XBOXHardwareInfo;

typedef struct
{
	uint8_t xdonVersion;
	XBOXKernelVersion kernelVersion;
	XBOXHardwareInfo hardwareInfo;
	uint32_t maxIOSize;
} XDONIdentity;

typedef struct
{
	struct
	{
		bool hardDrive0 : 1;
		bool hardDrive1 : 1;
		bool memoryUnit0 : 1;
		bool memoryUnit1 : 1;
		bool memoryUnit2 : 1;
		bool memoryUnit3 : 1;
		bool memoryUnit4 : 1;
		bool memoryUnit5 : 1;
		bool memoryUnit6 : 1;
		bool memoryUnit7 : 1;
		bool dvdRom : 1;
	} availableDevices;
	DISK_GEOMETRY hardDrive0Geometry;
	DISK_GEOMETRY hardDrive1Geometry;
	DISK_GEOMETRY memoryUnit0Geometry;
	DISK_GEOMETRY memoryUnit1Geometry;
	DISK_GEOMETRY memoryUnit2Geometry;
	DISK_GEOMETRY memoryUnit3Geometry;
	DISK_GEOMETRY memoryUnit4Geometry;
	DISK_GEOMETRY memoryUnit5Geometry;
	DISK_GEOMETRY memoryUnit6Geometry;
	DISK_GEOMETRY memoryUnit7Geometry;
	DISK_GEOMETRY dvdRomGeometry;
	bool hardDrive0InfoAvailable;
	IDENTIFY_DEVICE_DATA hardDrive0Info;
	bool hardDrive1InfoAvailable;
	IDENTIFY_DEVICE_DATA hardDrive1Info;
} XDONDevices;

typedef struct
{
	uint8_t device;
	uint64_t offset;
	uint32_t length;
	bool compress;
} XDONReadRequest;

#pragma warning(push)
#pragma warning(disable:4200)
typedef struct
{
	uint32_t length;
	uint32_t uncompressedLength;
	uint8_t data[0];
} XDONReadResponse;

typedef struct
{
	uint8_t device;
	uint64_t offset;
	uint32_t length;
	bool compressed;
	uint8_t data[0];
} XDONWriteRequest;

typedef struct
{
	uint8_t device;
	uint64_t offset;
	uint32_t length;
	uint8_t value;
} XDONWriteSameRequest;

typedef struct
{
	IDEREGS registers;
	bool hasOutgoingData;
	uint32_t length;
	uint8_t data[0];
} XDONATACommandRequest;

typedef struct
{
	IDEREGS registers;
	uint8_t data[0];
} XDONATACommandResponse;
#pragma warning(pop)

typedef struct
{
	uint8_t action;
} XDONRebootShutdownRequest;

#pragma pack(pop)