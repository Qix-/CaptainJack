/*
	,---.         .              ,-_/
	|  -' ,-. ,-. |- ,-. . ,-.   '  | ,-. ,-. . ,
	|   . ,-| | | |  ,-| | | |      | ,-| |   |/
	`---' `-^ |-' `' `-^ ' ' '      | `-^ `-' |\
	          |                  /  |         ' `
	          '                  `--'
	          captain jack audio device
	         github.com/qix-/captainjack

	        copyright (c) 2016 josh junon
	        released under the MIT license
*/

#include <CoreAudio/AudioServerPlugIn.h>
#include <dispatch/dispatch.h>
#include <jack/jack.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/syslog.h>

#include "xmit.h"

#define DebugMsg(inFormat, ...) syslog(LOG_NOTICE, inFormat, ## __VA_ARGS__)

//  - a box
//  - a device
//      - supports 44100 and 48000 sample rates
//      - provides a rate scalar of 1.0 via hard coding
//  - a single input stream
//      - supports 2 channels of 32 bit float LPCM samples
//  - a single output stream
//      - supports 2 channels of 32 bit float LPCM samples


//  Declare the internal object ID numbers for all the objects this driver implements. Note that
//  because the driver has fixed set of objects that never grows or shrinks. If this were not the
//  case, the driver would need to have a means to dynamically allocate these IDs. It is important
//  to realize that a lot of the structure of this driver is vastly simpler when the IDs are all
//  known a priori. Comments in the code will try to identify some of these simplifications and
//  point out what a more complicated driver will need to do.
enum {
	kObjectID_PlugIn                    = kAudioObjectPlugInObject,
	kObjectID_Box                       = 2,
	kObjectID_Device                    = 3,
	kObjectID_Stream_Input              = 4,
	kObjectID_Volume_Input_Master       = 5,
	kObjectID_Mute_Input_Master         = 6,
	kObjectID_DataSource_Input_Master   = 7,
	kObjectID_Stream_Output             = 8,
	kObjectID_Volume_Output_Master      = 9,
	kObjectID_Mute_Output_Master        = 10,
	kObjectID_DataSource_Output_Master  = 11
};

#define                         kPlugIn_BundleID                "me.junon.CaptainJack"
static pthread_mutex_t          gPlugIn_StateMutex              = PTHREAD_MUTEX_INITIALIZER;
static UInt32                   gPlugIn_RefCount                = 0;
static AudioServerPlugInHostRef gPlugIn_Host                    = NULL;

#define                         kBox_UID                        "CaptainJackBox_UID"
static CFStringRef              gBox_Name                       = NULL;
static Boolean                  gBox_Acquired                   = true;

#define                         kDevice_UID                     "CaptainJackDevice_UID"
#define                         kDevice_ModelUID                "CaptainJackDevice_ModelUID"
static pthread_mutex_t          gDevice_IOMutex                 = PTHREAD_MUTEX_INITIALIZER;
static Float64                  gDevice_SampleRate              = 44100.0;
static UInt64                   gDevice_IOIsRunning             = 0;
static const UInt32             kDevice_RingBufferSize          = 16384;
static Float64                  gDevice_HostTicksPerFrame       = 0.0;
static UInt64                   gDevice_NumberTimeStamps        = 0;
static Float64                  gDevice_AnchorSampleTime        = 0.0;
static UInt64                   gDevice_AnchorHostTime          = 0;

static bool                     gStream_Input_IsActive          = true;
static bool                     gStream_Output_IsActive         = true;

static const Float32            kVolume_MinDB                   = -96.0;
static const Float32            kVolume_MaxDB                   = 6.0;
static Float32                  gVolume_Input_Master_Value      = 0.0;
static Float32                  gVolume_Output_Master_Value     = 0.0;

static bool                     gMute_Input_Master_Value        = false;
static bool                     gMute_Output_Master_Value       = false;

static UInt32                   gDataSource_Input_Master_Value  = 0;
static UInt32                   gDataSource_Output_Master_Value = 0;

static CaptainJack_Xmitter     *gXmitter                        = 0;

void               *CaptainJack_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID);
static HRESULT      CaptainJack_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface);
static ULONG        CaptainJack_AddRef(void *inDriver);
static ULONG        CaptainJack_Release(void *inDriver);
static OSStatus     CaptainJack_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus     CaptainJack_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo *inClientInfo, AudioObjectID *outDeviceObjectID);
static OSStatus     CaptainJack_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID);
static OSStatus     CaptainJack_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo *inClientInfo);
static OSStatus     CaptainJack_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo *inClientInfo);
static OSStatus     CaptainJack_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void *inChangeInfo);
static OSStatus     CaptainJack_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void *inChangeInfo);
static Boolean      CaptainJack_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress);
static OSStatus     CaptainJack_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, Boolean *outIsSettable);
static OSStatus     CaptainJack_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 *outDataSize);
static OSStatus     CaptainJack_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData);
static OSStatus     CaptainJack_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, const void *inData);
static OSStatus     CaptainJack_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus     CaptainJack_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus     CaptainJack_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed);
static OSStatus     CaptainJack_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean *outWillDo, Boolean *outWillDoInPlace);
static OSStatus     CaptainJack_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo);
static OSStatus     CaptainJack_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo, void *ioMainBuffer, void *ioSecondaryBuffer);
static OSStatus     CaptainJack_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo);

static Boolean      CaptainJack_HasPlugInProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress);
static OSStatus     CaptainJack_IsPlugInPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, Boolean *outIsSettable);
static OSStatus     CaptainJack_GetPlugInPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 *outDataSize);
static OSStatus     CaptainJack_GetPlugInPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData);
static OSStatus     CaptainJack_SetPlugInPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, const void *inData, UInt32 *outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]);

static Boolean      CaptainJack_HasBoxProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress);
static OSStatus     CaptainJack_IsBoxPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, Boolean *outIsSettable);
static OSStatus     CaptainJack_GetBoxPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 *outDataSize);
static OSStatus     CaptainJack_GetBoxPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData);
static OSStatus     CaptainJack_SetBoxPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, const void *inData, UInt32 *outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]);

static Boolean      CaptainJack_HasDeviceProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress);
static OSStatus     CaptainJack_IsDevicePropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, Boolean *outIsSettable);
static OSStatus     CaptainJack_GetDevicePropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 *outDataSize);
static OSStatus     CaptainJack_GetDevicePropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData);
static OSStatus     CaptainJack_SetDevicePropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, const void *inData, UInt32 *outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]);

static Boolean      CaptainJack_HasStreamProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress);
static OSStatus     CaptainJack_IsStreamPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, Boolean *outIsSettable);
static OSStatus     CaptainJack_GetStreamPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 *outDataSize);
static OSStatus     CaptainJack_GetStreamPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData);
static OSStatus     CaptainJack_SetStreamPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, const void *inData, UInt32 *outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]);

static Boolean      CaptainJack_HasControlProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress);
static OSStatus     CaptainJack_IsControlPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, Boolean *outIsSettable);
static OSStatus     CaptainJack_GetControlPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 *outDataSize);
static OSStatus     CaptainJack_GetControlPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData);
static OSStatus     CaptainJack_SetControlPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, const void *inData, UInt32 *outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]);


static AudioServerPlugInDriverInterface gAudioServerPlugInDriverInterface = {
	NULL,
	CaptainJack_QueryInterface,
	CaptainJack_AddRef,
	CaptainJack_Release,
	CaptainJack_Initialize,
	CaptainJack_CreateDevice,
	CaptainJack_DestroyDevice,
	CaptainJack_AddDeviceClient,
	CaptainJack_RemoveDeviceClient,
	CaptainJack_PerformDeviceConfigurationChange,
	CaptainJack_AbortDeviceConfigurationChange,
	CaptainJack_HasProperty,
	CaptainJack_IsPropertySettable,
	CaptainJack_GetPropertyDataSize,
	CaptainJack_GetPropertyData,
	CaptainJack_SetPropertyData,
	CaptainJack_StartIO,
	CaptainJack_StopIO,
	CaptainJack_GetZeroTimeStamp,
	CaptainJack_WillDoIOOperation,
	CaptainJack_BeginIOOperation,
	CaptainJack_DoIOOperation,
	CaptainJack_EndIOOperation
};

static AudioServerPlugInDriverInterface    *gAudioServerPlugInDriverInterfacePtr    = &gAudioServerPlugInDriverInterface;
static AudioServerPlugInDriverRef           gAudioServerPlugInDriverRef             = &gAudioServerPlugInDriverInterfacePtr;


void *CaptainJack_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID) {
#pragma unused(inAllocator)

	if (CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) {
		return gAudioServerPlugInDriverRef;
	}

	return NULL;
}


static HRESULT CaptainJack_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface) {
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_QueryInterface: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (outInterface == NULL) {
		DebugMsg("CaptainJack_QueryInterface: no place to store the returned interface");
		return kAudioHardwareIllegalOperationError;
	}

	CFUUIDRef theRequestedUUID = NULL;
	theRequestedUUID = CFUUIDCreateFromUUIDBytes(NULL, inUUID);

	if (theRequestedUUID == NULL) {
		DebugMsg("CaptainJack_QueryInterface: failed to create the CFUUIDRef");
		return kAudioHardwareIllegalOperationError;
	}

	if (CFEqual(theRequestedUUID, IUnknownUUID) || CFEqual(theRequestedUUID, kAudioServerPlugInDriverInterfaceUUID)) {
		pthread_mutex_lock(&gPlugIn_StateMutex);
		++gPlugIn_RefCount;
		pthread_mutex_unlock(&gPlugIn_StateMutex);
		*outInterface = gAudioServerPlugInDriverRef;
	} else {
		return E_NOINTERFACE;
	}

	CFRelease(theRequestedUUID);
	return 0;
}

static ULONG CaptainJack_AddRef(void *inDriver) {
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_AddRef: bad driver reference");
		return 0;
	}

	pthread_mutex_lock(&gPlugIn_StateMutex);

	if (gPlugIn_RefCount < UINT32_MAX) {
		++gPlugIn_RefCount;
	}

	pthread_mutex_unlock(&gPlugIn_StateMutex);
	return gPlugIn_RefCount;
}

static ULONG CaptainJack_Release(void *inDriver) {
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_Release: bad driver reference");
		return 0;
	}

	pthread_mutex_lock(&gPlugIn_StateMutex);

	if (gPlugIn_RefCount > 0) {
		--gPlugIn_RefCount;
	}

	pthread_mutex_unlock(&gPlugIn_StateMutex);
	return gPlugIn_RefCount;
}


static OSStatus CaptainJack_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) {
	openlog("CaptainJack-Driver", LOG_CONS | LOG_NDELAY | LOG_PID, LOG_DAEMON);
	setlogmask(0);
	syslog(LOG_NOTICE, "Captain Jack is sailing the seas!");

	gXmitter = CaptainJack_GetXmitterServer();

	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_Initialize: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	gPlugIn_Host = inHost;
	CFPropertyListRef theSettingsData = NULL;
	gPlugIn_Host->CopyFromStorage(gPlugIn_Host, CFSTR("box acquired"), &theSettingsData);

	if (theSettingsData != NULL) {
		if (CFGetTypeID(theSettingsData) == CFBooleanGetTypeID()) {
			gBox_Acquired = CFBooleanGetValue((CFBooleanRef)theSettingsData);
		} else if (CFGetTypeID(theSettingsData) == CFNumberGetTypeID()) {
			SInt32 theValue = 0;
			CFNumberGetValue((CFNumberRef)theSettingsData, kCFNumberSInt32Type, &theValue);
			gBox_Acquired = theValue ? 1 : 0;
		}

		CFRelease(theSettingsData);
	}

	gPlugIn_Host->CopyFromStorage(gPlugIn_Host, CFSTR("box acquired"), &theSettingsData);

	if (theSettingsData != NULL) {
		if (CFGetTypeID(theSettingsData) == CFStringGetTypeID()) {
			gBox_Name = (CFStringRef)theSettingsData;
			CFRetain(gBox_Name);
		}

		CFRelease(theSettingsData);
	}

	if (gBox_Name == NULL) {
		gBox_Name = CFSTR("Null Box");
	}

	struct mach_timebase_info theTimeBaseInfo;

	mach_timebase_info(&theTimeBaseInfo);

	Float64 theHostClockFrequency = theTimeBaseInfo.denom / theTimeBaseInfo.numer;

	theHostClockFrequency *= 1000000000.0;

	gDevice_HostTicksPerFrame = theHostClockFrequency / gDevice_SampleRate;

	gXmitter->do_device_ready();

	return 0;
}

static OSStatus CaptainJack_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo *inClientInfo, AudioObjectID *outDeviceObjectID) {
#pragma unused(inDescription, inClientInfo, outDeviceObjectID)

	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_CreateDevice: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus CaptainJack_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID) {
#pragma unused(inDeviceObjectID)

	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_DestroyDevice: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus CaptainJack_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo *inClientInfo) {
#pragma unused(inClientInfo)

	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_AddDeviceClient: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inDeviceObjectID != kObjectID_Device) {
		DebugMsg("CaptainJack_AddDeviceClient: bad device ID");
		return kAudioHardwareBadObjectError;
	}

	gXmitter->do_client_connect(inClientInfo->mClientID, inClientInfo->mProcessID);

	return 0;
}

static OSStatus CaptainJack_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo *inClientInfo) {
#pragma unused(inClientInfo)

	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_RemoveDeviceClient: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inDeviceObjectID != kObjectID_Device) {
		DebugMsg("CaptainJack_RemoveDeviceClient: bad device ID");
		return kAudioHardwareBadObjectError;
	}

	gXmitter->do_client_disconnect(inClientInfo->mClientID, inClientInfo->mProcessID);

	return 0;
}

static OSStatus CaptainJack_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void *inChangeInfo) {
#pragma unused(inChangeInfo)

	//  This method is called to tell the device that it can perform the configuation change that it
	//  had requested via a call to the host method, RequestDeviceConfigurationChange(). The
	//  arguments, inChangeAction and inChangeInfo are the same as what was passed to
	//  RequestDeviceConfigurationChange().
	//
	//  The HAL guarantees that IO will be stopped while this method is in progress. The HAL will
	//  also handle figuring out exactly what changed for the non-control related properties. This
	//  means that the only notifications that would need to be sent here would be for either
	//  custom properties the HAL doesn't know about or for controls.
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_PerformDeviceConfigurationChange: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inDeviceObjectID != kObjectID_Device) {
		DebugMsg("CaptainJack_PerformDeviceConfigurationChange: bad device ID");
		return kAudioHardwareBadObjectError;
	}

	if ((inChangeAction != 44100) && (inChangeAction != 48000)) {
		DebugMsg("CaptainJack_PerformDeviceConfigurationChange: bad sample rate");
		return kAudioHardwareBadObjectError;
	}

	pthread_mutex_lock(&gPlugIn_StateMutex);
	gDevice_SampleRate = inChangeAction;
	struct mach_timebase_info theTimeBaseInfo;
	mach_timebase_info(&theTimeBaseInfo);
	Float64 theHostClockFrequency = theTimeBaseInfo.denom / theTimeBaseInfo.numer;
	theHostClockFrequency *= 1000000000.0;
	gDevice_HostTicksPerFrame = theHostClockFrequency / gDevice_SampleRate;
	pthread_mutex_unlock(&gPlugIn_StateMutex);
	return 0;
}

static OSStatus CaptainJack_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void *inChangeInfo) {
#pragma unused(inChangeAction, inChangeInfo)
	//  This method is called to tell the driver that a request for a config change has been denied.
	//  This provides the driver an opportunity to clean up any state associated with the request.
	//  declare the local variables

	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_PerformDeviceConfigurationChange: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inDeviceObjectID != kObjectID_Device) {
		DebugMsg("CaptainJack_PerformDeviceConfigurationChange: bad device ID");
		return kAudioHardwareBadObjectError;
	}

	return 0;
}

static Boolean CaptainJack_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress) {
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_HasProperty: bad driver reference");
		return false;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_HasProperty: no address");
		return false;
	}

	switch (inObjectID) {
	case kObjectID_PlugIn:
		return CaptainJack_HasPlugInProperty(inDriver, inObjectID, inClientProcessID, inAddress);

	case kObjectID_Box:
		return CaptainJack_HasBoxProperty(inDriver, inObjectID, inClientProcessID, inAddress);

	case kObjectID_Device:
		return CaptainJack_HasDeviceProperty(inDriver, inObjectID, inClientProcessID, inAddress);

	case kObjectID_Stream_Input:
	case kObjectID_Stream_Output:
		return CaptainJack_HasStreamProperty(inDriver, inObjectID, inClientProcessID, inAddress);

	case kObjectID_Volume_Input_Master:
	case kObjectID_Volume_Output_Master:
	case kObjectID_Mute_Input_Master:
	case kObjectID_Mute_Output_Master:
	case kObjectID_DataSource_Input_Master:
	case kObjectID_DataSource_Output_Master:
		return CaptainJack_HasControlProperty(inDriver, inObjectID, inClientProcessID, inAddress);
	};

	return false;
}

static OSStatus CaptainJack_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, Boolean *outIsSettable) {
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_IsPropertySettable: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_IsPropertySettable: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outIsSettable == NULL) {
		DebugMsg("CaptainJack_IsPropertySettable: no place to put the return value");
		return kAudioHardwareIllegalOperationError;
	}

	switch (inObjectID) {
	case kObjectID_PlugIn:
		return CaptainJack_IsPlugInPropertySettable(inDriver, inObjectID, inClientProcessID, inAddress, outIsSettable);

	case kObjectID_Box:
		return CaptainJack_IsBoxPropertySettable(inDriver, inObjectID, inClientProcessID, inAddress, outIsSettable);

	case kObjectID_Device:
		return CaptainJack_IsDevicePropertySettable(inDriver, inObjectID, inClientProcessID, inAddress, outIsSettable);

	case kObjectID_Stream_Input:
	case kObjectID_Stream_Output:
		return CaptainJack_IsStreamPropertySettable(inDriver, inObjectID, inClientProcessID, inAddress, outIsSettable);

	case kObjectID_Volume_Input_Master:
	case kObjectID_Volume_Output_Master:
	case kObjectID_Mute_Input_Master:
	case kObjectID_Mute_Output_Master:
	case kObjectID_DataSource_Input_Master:
	case kObjectID_DataSource_Output_Master:
		return CaptainJack_IsControlPropertySettable(inDriver, inObjectID, inClientProcessID, inAddress, outIsSettable);
	};

	return kAudioHardwareBadObjectError;
}

static OSStatus CaptainJack_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 *outDataSize) {
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_GetPropertyDataSize: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_GetPropertyDataSize: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outDataSize == NULL) {
		DebugMsg("CaptainJack_GetPropertyDataSize: no place to put the return value");
		return kAudioHardwareIllegalOperationError;
	}

	switch (inObjectID) {
	case kObjectID_PlugIn:
		return CaptainJack_GetPlugInPropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, outDataSize);

	case kObjectID_Box:
		return CaptainJack_GetBoxPropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, outDataSize);

	case kObjectID_Device:
		return CaptainJack_GetDevicePropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, outDataSize);

	case kObjectID_Stream_Input:
	case kObjectID_Stream_Output:
		return CaptainJack_GetStreamPropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, outDataSize);

	case kObjectID_Volume_Input_Master:
	case kObjectID_Volume_Output_Master:
	case kObjectID_Mute_Input_Master:
	case kObjectID_Mute_Output_Master:
	case kObjectID_DataSource_Input_Master:
	case kObjectID_DataSource_Output_Master:
		return CaptainJack_GetControlPropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, outDataSize);
	};

	return kAudioHardwareBadObjectError;
}

static OSStatus CaptainJack_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_GetPropertyData: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_GetPropertyData: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outDataSize == NULL) {
		DebugMsg("CaptainJack_GetPropertyData: no place to put the return value size");
		return kAudioHardwareIllegalOperationError;
	}

	if (outData == NULL) {
		DebugMsg("CaptainJack_GetPropertyData: no place to put the return value");
		return kAudioHardwareIllegalOperationError;
	}

	switch (inObjectID) {
	case kObjectID_PlugIn:
		return CaptainJack_GetPlugInPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);

	case kObjectID_Box:
		return CaptainJack_GetBoxPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);

	case kObjectID_Device:
		return CaptainJack_GetDevicePropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);

	case kObjectID_Stream_Input:
	case kObjectID_Stream_Output:
		return CaptainJack_GetStreamPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);

	case kObjectID_Volume_Input_Master:
	case kObjectID_Volume_Output_Master:
	case kObjectID_Mute_Input_Master:
	case kObjectID_Mute_Output_Master:
	case kObjectID_DataSource_Input_Master:
	case kObjectID_DataSource_Output_Master:
		return CaptainJack_GetControlPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
	};

	return kAudioHardwareBadObjectError;
}

static OSStatus CaptainJack_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, const void *inData) {
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_SetPropertyData: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_SetPropertyData: no address");
		return kAudioHardwareIllegalOperationError;
	}

	UInt32 theNumberPropertiesChanged = 0;
	AudioObjectPropertyAddress theChangedAddresses[2];
	OSStatus status = 0;

	switch (inObjectID) {
	case kObjectID_PlugIn:
		status = CaptainJack_SetPlugInPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData, &theNumberPropertiesChanged, theChangedAddresses);

	case kObjectID_Box:
		status = CaptainJack_SetBoxPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData, &theNumberPropertiesChanged, theChangedAddresses);

	case kObjectID_Device:
		status = CaptainJack_SetDevicePropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData, &theNumberPropertiesChanged, theChangedAddresses);

	case kObjectID_Stream_Input:
	case kObjectID_Stream_Output:
		status = CaptainJack_SetStreamPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData, &theNumberPropertiesChanged, theChangedAddresses);

	case kObjectID_Volume_Input_Master:
	case kObjectID_Volume_Output_Master:
	case kObjectID_Mute_Input_Master:
	case kObjectID_Mute_Output_Master:
	case kObjectID_DataSource_Input_Master:
	case kObjectID_DataSource_Output_Master:
		status = CaptainJack_SetControlPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData, &theNumberPropertiesChanged, theChangedAddresses);

	default:
		return kAudioHardwareBadObjectError;
	};

	if (theNumberPropertiesChanged > 0) {
		gPlugIn_Host->PropertiesChanged(gPlugIn_Host, inObjectID, theNumberPropertiesChanged, theChangedAddresses);
	}

	return status;
}


static Boolean  CaptainJack_HasPlugInProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress) {
#pragma unused(inClientProcessID)
	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_HasPlugInProperty: bad driver reference");
		return false;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_HasPlugInProperty: no address");
		return false;
	}

	if (inObjectID != kObjectID_PlugIn) {
		DebugMsg("CaptainJack_HasPlugInProperty: not the plug-in object");
		return false;
	}

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required. There is more detailed commentary about each
	//  property in the CaptainJack_GetPlugInPropertyData() method.
	switch (inAddress->mSelector) {
	case kAudioObjectPropertyBaseClass:
	case kAudioObjectPropertyClass:
	case kAudioObjectPropertyOwner:
	case kAudioObjectPropertyManufacturer:
	case kAudioObjectPropertyOwnedObjects:
	case kAudioPlugInPropertyBoxList:
	case kAudioPlugInPropertyTranslateUIDToBox:
	case kAudioPlugInPropertyDeviceList:
	case kAudioPlugInPropertyTranslateUIDToDevice:
	case kAudioPlugInPropertyResourceBundle:
		return true;
	};

	return false;
}

static OSStatus CaptainJack_IsPlugInPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, Boolean *outIsSettable) {
#pragma unused(inClientProcessID)
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_IsPlugInPropertySettable: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_IsPlugInPropertySettable: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outIsSettable == NULL) {
		DebugMsg("CaptainJack_IsPlugInPropertySettable: no place to put the return value");
		return kAudioHardwareIllegalOperationError;
	}

	if (inObjectID != kObjectID_PlugIn) {
		DebugMsg("CaptainJack_IsPlugInPropertySettable: not the plug-in object");
		return kAudioHardwareBadObjectError;
	}

	switch (inAddress->mSelector) {
	case kAudioObjectPropertyBaseClass:
	case kAudioObjectPropertyClass:
	case kAudioObjectPropertyOwner:
	case kAudioObjectPropertyManufacturer:
	case kAudioObjectPropertyOwnedObjects:
	case kAudioPlugInPropertyBoxList:
	case kAudioPlugInPropertyTranslateUIDToBox:
	case kAudioPlugInPropertyDeviceList:
	case kAudioPlugInPropertyTranslateUIDToDevice:
	case kAudioPlugInPropertyResourceBundle:
		*outIsSettable = false;
		return 0;
	};

	return kAudioHardwareUnknownPropertyError;
}

static OSStatus CaptainJack_GetPlugInPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 *outDataSize) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
	OSStatus theAnswer = 0;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_GetPlugInPropertyDataSize: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_GetPlugInPropertyDataSize: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outDataSize == NULL) {
		DebugMsg("CaptainJack_GetPlugInPropertyDataSize: no place to put the return value");
		return kAudioHardwareIllegalOperationError;
	}

	if (inObjectID != kObjectID_PlugIn) {
		DebugMsg("CaptainJack_GetPlugInPropertyDataSize: not the plug-in object");
		return kAudioHardwareBadObjectError;
	}

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required. There is more detailed commentary about each
	//  property in the CaptainJack_GetPlugInPropertyData() method.
	switch (inAddress->mSelector) {
	case kAudioObjectPropertyBaseClass:
		*outDataSize = sizeof(AudioClassID);
		break;

	case kAudioObjectPropertyClass:
		*outDataSize = sizeof(AudioClassID);
		break;

	case kAudioObjectPropertyOwner:
		*outDataSize = sizeof(AudioObjectID);
		break;

	case kAudioObjectPropertyManufacturer:
		*outDataSize = sizeof(CFStringRef);
		break;

	case kAudioObjectPropertyOwnedObjects:
		if (gBox_Acquired) {
			*outDataSize = 2 * sizeof(AudioClassID);
		} else {
			*outDataSize = sizeof(AudioClassID);
		}

		break;

	case kAudioPlugInPropertyBoxList:
		*outDataSize = sizeof(AudioClassID);
		break;

	case kAudioPlugInPropertyTranslateUIDToBox:
		*outDataSize = sizeof(AudioObjectID);
		break;

	case kAudioPlugInPropertyDeviceList:
		if (gBox_Acquired) {
			*outDataSize = sizeof(AudioClassID);
		} else {
			*outDataSize = 0;
		}

		break;

	case kAudioPlugInPropertyTranslateUIDToDevice:
		*outDataSize = sizeof(AudioObjectID);
		break;

	case kAudioPlugInPropertyResourceBundle:
		*outDataSize = sizeof(CFStringRef);
		break;

	default:
		return kAudioHardwareUnknownPropertyError;
	};

	return theAnswer;
}

static OSStatus CaptainJack_GetPlugInPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
#pragma unused(inClientProcessID)
	//  declare the local variables
	OSStatus theAnswer = 0;
	UInt32 theNumberItemsToFetch;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_GetPlugInPropertyData: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_GetPlugInPropertyData: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outDataSize == NULL) {
		DebugMsg("CaptainJack_GetPlugInPropertyData: no place to put the return value size");
		return kAudioHardwareIllegalOperationError;
	}

	if (outData == NULL) {
		DebugMsg("CaptainJack_GetPlugInPropertyData: no place to put the return value");
		return kAudioHardwareIllegalOperationError;
	}

	if (inObjectID != kObjectID_PlugIn) {
		DebugMsg("CaptainJack_GetPlugInPropertyData: not the plug-in object");
		return kAudioHardwareBadObjectError;
	}

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required.
	//
	//  Also, since most of the data that will get returned is static, there are few instances where
	//  it is necessary to lock the state mutex.
	switch (inAddress->mSelector) {
	case kAudioObjectPropertyBaseClass:

		//  The base class for kAudioPlugInClassID is kAudioObjectClassID
		if (inDataSize < sizeof(AudioClassID)) {
			DebugMsg("CaptainJack_GetPlugInPropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the plug-in");
			return kAudioHardwareBadPropertySizeError;
		}

		*((AudioClassID *)outData) = kAudioObjectClassID;
		*outDataSize = sizeof(AudioClassID);
		break;

	case kAudioObjectPropertyClass:

		//  The class is always kAudioPlugInClassID for regular drivers
		if (inDataSize < sizeof(AudioClassID)) {
			DebugMsg("CaptainJack_GetPlugInPropertyData: not enough space for the return value of kAudioObjectPropertyClass for the plug-in");
			return kAudioHardwareBadPropertySizeError;
		}

		*((AudioClassID *)outData) = kAudioPlugInClassID;
		*outDataSize = sizeof(AudioClassID);
		break;

	case kAudioObjectPropertyOwner:

		//  The plug-in doesn't have an owning object
		if (inDataSize < sizeof(AudioObjectID)) {
			DebugMsg("CaptainJack_GetPlugInPropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the plug-in");
			return kAudioHardwareBadPropertySizeError;
		}

		*((AudioObjectID *)outData) = kAudioObjectUnknown;
		*outDataSize = sizeof(AudioObjectID);
		break;

	case kAudioObjectPropertyManufacturer:

		//  This is the human readable name of the maker of the plug-in.
		if (inDataSize < sizeof(CFStringRef)) {
			DebugMsg("CaptainJack_GetPlugInPropertyData: not enough space for the return value of kAudioObjectPropertyManufacturer for the plug-in");
			return kAudioHardwareBadPropertySizeError;
		}

		*((CFStringRef *)outData) = CFSTR("Apple Inc.");
		*outDataSize = sizeof(CFStringRef);
		break;

	case kAudioObjectPropertyOwnedObjects:
		//  Calculate the number of items that have been requested. Note that this
		//  number is allowed to be smaller than the actual size of the list. In such
		//  case, only that number of items will be returned
		theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);

		//  Clamp that to the number of boxes this driver implements (which is just 1)
		if (theNumberItemsToFetch > (gBox_Acquired ? 2 : 1)) {
			theNumberItemsToFetch = (gBox_Acquired ? 2 : 1);
		}

		//  Write the devices' object IDs into the return value
		if (theNumberItemsToFetch > 1) {
			((AudioObjectID *)outData)[0] = kObjectID_Box;
			((AudioObjectID *)outData)[0] = kObjectID_Device;
		} else if (theNumberItemsToFetch > 0) {
			((AudioObjectID *)outData)[0] = kObjectID_Box;
		}

		//  Return how many bytes we wrote to
		*outDataSize = theNumberItemsToFetch * sizeof(AudioClassID);
		break;

	case kAudioPlugInPropertyBoxList:
		//  Calculate the number of items that have been requested. Note that this
		//  number is allowed to be smaller than the actual size of the list. In such
		//  case, only that number of items will be returned
		theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);

		//  Clamp that to the number of boxes this driver implements (which is just 1)
		if (theNumberItemsToFetch > 1) {
			theNumberItemsToFetch = 1;
		}

		//  Write the devices' object IDs into the return value
		if (theNumberItemsToFetch > 0) {
			((AudioObjectID *)outData)[0] = kObjectID_Box;
		}

		//  Return how many bytes we wrote to
		*outDataSize = theNumberItemsToFetch * sizeof(AudioClassID);
		break;

	case kAudioPlugInPropertyTranslateUIDToBox:

		//  This property takes the CFString passed in the qualifier and converts that
		//  to the object ID of the box it corresponds to. For this driver, there is
		//  just the one box. Note that it is not an error if the string in the
		//  qualifier doesn't match any devices. In such case, kAudioObjectUnknown is
		//  the object ID to return.
		if (inDataSize < sizeof(AudioObjectID)) {
			DebugMsg("CaptainJack_GetPlugInPropertyData: not enough space for the return value of kAudioPlugInPropertyTranslateUIDToBox");
			return kAudioHardwareBadPropertySizeError;
		}

		if (inQualifierDataSize == sizeof(CFStringRef)) {
			DebugMsg("CaptainJack_GetPlugInPropertyData: the qualifier is the wrong size for kAudioPlugInPropertyTranslateUIDToBox");
			return kAudioHardwareBadPropertySizeError;
		}

		if (inQualifierData == NULL) {
			DebugMsg("CaptainJack_GetPlugInPropertyData: no qualifier for kAudioPlugInPropertyTranslateUIDToBox");
			return kAudioHardwareBadPropertySizeError;
		}

		if (CFStringCompare(*((CFStringRef *)inQualifierData), CFSTR(kBox_UID), 0) == kCFCompareEqualTo) {
			*((AudioObjectID *)outData) = kObjectID_Box;
		} else {
			*((AudioObjectID *)outData) = kAudioObjectUnknown;
		}

		*outDataSize = sizeof(AudioObjectID);
		break;

	case kAudioPlugInPropertyDeviceList:
		//  Calculate the number of items that have been requested. Note that this
		//  number is allowed to be smaller than the actual size of the list. In such
		//  case, only that number of items will be returned
		theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);

		//  Clamp that to the number of devices this driver implements (which is just 1 if the
		//  box has been acquired)
		if (theNumberItemsToFetch > (gBox_Acquired ? 1 : 0)) {
			theNumberItemsToFetch = (gBox_Acquired ? 1 : 0);
		}

		//  Write the devices' object IDs into the return value
		if (theNumberItemsToFetch > 0) {
			((AudioObjectID *)outData)[0] = kObjectID_Device;
		}

		//  Return how many bytes we wrote to
		*outDataSize = theNumberItemsToFetch * sizeof(AudioClassID);
		break;

	case kAudioPlugInPropertyTranslateUIDToDevice:

		//  This property takes the CFString passed in the qualifier and converts that
		//  to the object ID of the device it corresponds to. For this driver, there is
		//  just the one device. Note that it is not an error if the string in the
		//  qualifier doesn't match any devices. In such case, kAudioObjectUnknown is
		//  the object ID to return.
		if (inDataSize < sizeof(AudioObjectID)) {
			DebugMsg("CaptainJack_GetPlugInPropertyData: not enough space for the return value of kAudioPlugInPropertyTranslateUIDToDevice");
			return kAudioHardwareBadPropertySizeError;
		}

		if (inQualifierDataSize == sizeof(CFStringRef)) {
			DebugMsg("CaptainJack_GetPlugInPropertyData: the qualifier is the wrong size for kAudioPlugInPropertyTranslateUIDToDevice");
			return kAudioHardwareBadPropertySizeError;
		}

		if (inQualifierData == NULL) {
			DebugMsg("CaptainJack_GetPlugInPropertyData: no qualifier for kAudioPlugInPropertyTranslateUIDToDevice");
			return kAudioHardwareBadPropertySizeError;
		}

		if (CFStringCompare(*((CFStringRef *)inQualifierData), CFSTR(kDevice_UID), 0) == kCFCompareEqualTo) {
			*((AudioObjectID *)outData) = kObjectID_Device;
		} else {
			*((AudioObjectID *)outData) = kAudioObjectUnknown;
		}

		*outDataSize = sizeof(AudioObjectID);
		break;

	case kAudioPlugInPropertyResourceBundle:

		//  The resource bundle is a path relative to the path of the plug-in's bundle.
		//  To specify that the plug-in bundle itself should be used, we just return the
		//  empty string.
		if (inDataSize < sizeof(AudioObjectID)) {
			DebugMsg("CaptainJack_GetPlugInPropertyData: not enough space for the return value of kAudioPlugInPropertyResourceBundle");
			return kAudioHardwareBadPropertySizeError;
		}

		*((CFStringRef *)outData) = CFSTR("");
		*outDataSize = sizeof(CFStringRef);
		break;

	default:
		return kAudioHardwareUnknownPropertyError;
	};

	return theAnswer;
}

static OSStatus CaptainJack_SetPlugInPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, const void *inData, UInt32 *outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData, inDataSize, inData)
	//  declare the local variables
	OSStatus theAnswer = 0;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_SetPlugInPropertyData: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_SetPlugInPropertyData: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outNumberPropertiesChanged == NULL) {
		DebugMsg("CaptainJack_SetPlugInPropertyData: no place to return the number of properties that changed");
		return kAudioHardwareIllegalOperationError;
	}

	if (outChangedAddresses == NULL) {
		DebugMsg("CaptainJack_SetPlugInPropertyData: no place to return the properties that changed");
		return kAudioHardwareIllegalOperationError;
	}

	if (inObjectID != kObjectID_PlugIn) {
		DebugMsg("CaptainJack_SetPlugInPropertyData: not the plug-in object");
		return kAudioHardwareBadObjectError;
	}

	//  initialize the returned number of changed properties
	*outNumberPropertiesChanged = 0;

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required. There is more detailed commentary about each
	//  property in the CaptainJack_GetPlugInPropertyData() method.
	switch (inAddress->mSelector) {
	default:
		return kAudioHardwareUnknownPropertyError;
	};

	return theAnswer;
}


static Boolean  CaptainJack_HasBoxProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress) {
	//  This method returns whether or not the box object has the given property.
#pragma unused(inClientProcessID)
	//  declare the local variables
	Boolean theAnswer = false;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_HasBoxProperty: bad driver reference");
		return theAnswer;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_HasBoxProperty: no address");
		return theAnswer;
	}

	if (inObjectID != kObjectID_Box) {
		DebugMsg("CaptainJack_HasBoxProperty: not the box object");
		return theAnswer;
	}

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required. There is more detailed commentary about each
	//  property in the CaptainJack_GetBoxPropertyData() method.
	switch (inAddress->mSelector) {
	case kAudioObjectPropertyBaseClass:
	case kAudioObjectPropertyClass:
	case kAudioObjectPropertyOwner:
	case kAudioObjectPropertyName:
	case kAudioObjectPropertyModelName:
	case kAudioObjectPropertyManufacturer:
	case kAudioObjectPropertyOwnedObjects:
	case kAudioObjectPropertyIdentify:
	case kAudioObjectPropertySerialNumber:
	case kAudioObjectPropertyFirmwareVersion:
	case kAudioBoxPropertyBoxUID:
	case kAudioBoxPropertyTransportType:
	case kAudioBoxPropertyHasAudio:
	case kAudioBoxPropertyHasVideo:
	case kAudioBoxPropertyHasMIDI:
	case kAudioBoxPropertyIsProtected:
	case kAudioBoxPropertyAcquired:
	case kAudioBoxPropertyAcquisitionFailed:
	case kAudioBoxPropertyDeviceList:
		return true;
	};

	return theAnswer;
}

static OSStatus CaptainJack_IsBoxPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, Boolean *outIsSettable) {
	//  This method returns whether or not the given property on the plug-in object can have its
	//  value changed.
#pragma unused(inClientProcessID)
	//  declare the local variables
	OSStatus theAnswer = 0;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_IsBoxPropertySettable: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_IsBoxPropertySettable: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outIsSettable == NULL) {
		DebugMsg("CaptainJack_IsBoxPropertySettable: no place to put the return value");
		return kAudioHardwareIllegalOperationError;
	}

	if (inObjectID != kObjectID_Box) {
		DebugMsg("CaptainJack_IsBoxPropertySettable: not the plug-in object");
		return kAudioHardwareBadObjectError;
	}

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required. There is more detailed commentary about each
	//  property in the CaptainJack_GetBoxPropertyData() method.
	switch (inAddress->mSelector) {
	case kAudioObjectPropertyBaseClass:
	case kAudioObjectPropertyClass:
	case kAudioObjectPropertyOwner:
	case kAudioObjectPropertyModelName:
	case kAudioObjectPropertyManufacturer:
	case kAudioObjectPropertyOwnedObjects:
	case kAudioObjectPropertySerialNumber:
	case kAudioObjectPropertyFirmwareVersion:
	case kAudioBoxPropertyBoxUID:
	case kAudioBoxPropertyTransportType:
	case kAudioBoxPropertyHasAudio:
	case kAudioBoxPropertyHasVideo:
	case kAudioBoxPropertyHasMIDI:
	case kAudioBoxPropertyIsProtected:
	case kAudioBoxPropertyAcquisitionFailed:
	case kAudioBoxPropertyDeviceList:
		*outIsSettable = false;
		break;

	case kAudioObjectPropertyName:
	case kAudioObjectPropertyIdentify:
	case kAudioBoxPropertyAcquired:
		*outIsSettable = true;
		break;

	default:
		return kAudioHardwareUnknownPropertyError;
	};

	return theAnswer;
}

static OSStatus CaptainJack_GetBoxPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 *outDataSize) {
	//  This method returns the byte size of the property's data.
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
	//  declare the local variables
	OSStatus theAnswer = 0;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_GetBoxPropertyDataSize: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_GetBoxPropertyDataSize: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outDataSize == NULL) {
		DebugMsg("CaptainJack_GetBoxPropertyDataSize: no place to put the return value");
		return kAudioHardwareIllegalOperationError;
	}

	if (inObjectID != kObjectID_Box) {
		DebugMsg("CaptainJack_GetBoxPropertyDataSize: not the plug-in object");
		return kAudioHardwareBadObjectError;
	}

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required. There is more detailed commentary about each
	//  property in the CaptainJack_GetBoxPropertyData() method.
	switch (inAddress->mSelector) {
	case kAudioObjectPropertyBaseClass:
		*outDataSize = sizeof(AudioClassID);
		break;

	case kAudioObjectPropertyClass:
		*outDataSize = sizeof(AudioClassID);
		break;

	case kAudioObjectPropertyOwner:
		*outDataSize = sizeof(AudioObjectID);
		break;

	case kAudioObjectPropertyName:
		*outDataSize = sizeof(CFStringRef);
		break;

	case kAudioObjectPropertyModelName:
		*outDataSize = sizeof(CFStringRef);
		break;

	case kAudioObjectPropertyManufacturer:
		*outDataSize = sizeof(CFStringRef);
		break;

	case kAudioObjectPropertyOwnedObjects:
		*outDataSize = 0;
		break;

	case kAudioObjectPropertyIdentify:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioObjectPropertySerialNumber:
		*outDataSize = sizeof(CFStringRef);
		break;

	case kAudioObjectPropertyFirmwareVersion:
		*outDataSize = sizeof(CFStringRef);
		break;

	case kAudioBoxPropertyBoxUID:
		*outDataSize = sizeof(CFStringRef);
		break;

	case kAudioBoxPropertyTransportType:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioBoxPropertyHasAudio:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioBoxPropertyHasVideo:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioBoxPropertyHasMIDI:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioBoxPropertyIsProtected:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioBoxPropertyAcquired:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioBoxPropertyAcquisitionFailed:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioBoxPropertyDeviceList: {
		pthread_mutex_lock(&gPlugIn_StateMutex);
		*outDataSize = gBox_Acquired ? sizeof(AudioObjectID) : 0;
		pthread_mutex_unlock(&gPlugIn_StateMutex);
	}
	break;

	default:
		return kAudioHardwareUnknownPropertyError;
	};

	return theAnswer;
}

static OSStatus CaptainJack_GetBoxPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
	//  declare the local variables
	OSStatus theAnswer = 0;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_GetBoxPropertyData: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_GetBoxPropertyData: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outDataSize == NULL) {
		DebugMsg("CaptainJack_GetBoxPropertyData: no place to put the return value size");
		return kAudioHardwareIllegalOperationError;
	}

	if (outData == NULL) {
		DebugMsg("CaptainJack_GetBoxPropertyData: no place to put the return value");
		return kAudioHardwareIllegalOperationError;
	}

	if (inObjectID != kObjectID_Box) {
		DebugMsg("CaptainJack_GetBoxPropertyData: not the plug-in object");
		return kAudioHardwareBadObjectError;
	}

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required.
	//
	//  Also, since most of the data that will get returned is static, there are few instances where
	//  it is necessary to lock the state mutex.
	switch (inAddress->mSelector) {
	case kAudioObjectPropertyBaseClass:

		//  The base class for kAudioBoxClassID is kAudioObjectClassID
		if (inDataSize < sizeof(AudioClassID)) {
			DebugMsg("CaptainJack_GetBoxPropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the box");
			return kAudioHardwareBadPropertySizeError;
		}

		*((AudioClassID *)outData) = kAudioObjectClassID;
		*outDataSize = sizeof(AudioClassID);
		break;

	case kAudioObjectPropertyClass:

		//  The class is always kAudioBoxClassID for regular drivers
		if (inDataSize < sizeof(AudioClassID)) {
			DebugMsg("CaptainJack_GetBoxPropertyData: not enough space for the return value of kAudioObjectPropertyClass for the box");
			return kAudioHardwareBadPropertySizeError;
		}

		*((AudioClassID *)outData) = kAudioBoxClassID;
		*outDataSize = sizeof(AudioClassID);
		break;

	case kAudioObjectPropertyOwner:

		//  The owner is the plug-in object
		if (inDataSize < sizeof(AudioObjectID)) {
			DebugMsg("CaptainJack_GetBoxPropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the box");
			return kAudioHardwareBadPropertySizeError;
		}

		*((AudioObjectID *)outData) = kObjectID_PlugIn;
		*outDataSize = sizeof(AudioObjectID);
		break;

	case kAudioObjectPropertyName:

		//  This is the human readable name of the maker of the box.
		if (inDataSize < sizeof(CFStringRef)) {
			DebugMsg("CaptainJack_GetBoxPropertyData: not enough space for the return value of kAudioObjectPropertyManufacturer for the box");
			return kAudioHardwareBadPropertySizeError;
		}

		pthread_mutex_lock(&gPlugIn_StateMutex);
		*((CFStringRef *)outData) = gBox_Name;
		pthread_mutex_unlock(&gPlugIn_StateMutex);

		if (*((CFStringRef *)outData) != NULL) {
			CFRetain(*((CFStringRef *)outData));
		}

		*outDataSize = sizeof(CFStringRef);
		break;

	case kAudioObjectPropertyModelName:

		//  This is the human readable name of the maker of the box.
		if (inDataSize < sizeof(CFStringRef)) {
			DebugMsg("CaptainJack_GetBoxPropertyData: not enough space for the return value of kAudioObjectPropertyManufacturer for the box");
			return kAudioHardwareBadPropertySizeError;
		}

		*((CFStringRef *)outData) = CFSTR("Null Model");
		*outDataSize = sizeof(CFStringRef);
		break;

	case kAudioObjectPropertyManufacturer:

		//  This is the human readable name of the maker of the box.
		if (inDataSize < sizeof(CFStringRef)) {
			DebugMsg("CaptainJack_GetBoxPropertyData: not enough space for the return value of kAudioObjectPropertyManufacturer for the box");
			return kAudioHardwareBadPropertySizeError;
		}

		*((CFStringRef *)outData) = CFSTR("Apple Inc.");
		*outDataSize = sizeof(CFStringRef);
		break;

	case kAudioObjectPropertyOwnedObjects:
		//  This returns the objects directly owned by the object. Boxes don't own anything.
		*outDataSize = 0;
		break;

	case kAudioObjectPropertyIdentify:

		//  This is used to highling the device in the UI, but it's value has no meaning
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetBoxPropertyData: not enough space for the return value of kAudioObjectPropertyIdentify for the box");
			return kAudioHardwareBadPropertySizeError;
		}

		*((UInt32 *)outData) = 0;
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioObjectPropertySerialNumber:

		//  This is the human readable serial number of the box.
		if (inDataSize < sizeof(CFStringRef)) {
			DebugMsg("CaptainJack_GetBoxPropertyData: not enough space for the return value of kAudioObjectPropertySerialNumber for the box");
			return kAudioHardwareBadPropertySizeError;
		}

		*((CFStringRef *)outData) = CFSTR("00000001");
		*outDataSize = sizeof(CFStringRef);
		break;

	case kAudioObjectPropertyFirmwareVersion:

		//  This is the human readable firmware version of the box.
		if (inDataSize < sizeof(CFStringRef)) {
			DebugMsg("CaptainJack_GetBoxPropertyData: not enough space for the return value of kAudioObjectPropertyFirmwareVersion for the box");
			return kAudioHardwareBadPropertySizeError;
		}

		*((CFStringRef *)outData) = CFSTR("1.0");
		*outDataSize = sizeof(CFStringRef);
		break;

	case kAudioBoxPropertyBoxUID:

		//  Boxes have UIDs the same as devices
		if (inDataSize < sizeof(CFStringRef)) {
			DebugMsg("CaptainJack_GetBoxPropertyData: not enough space for the return value of kAudioObjectPropertyManufacturer for the box");
			return kAudioHardwareBadPropertySizeError;
		}

		*((CFStringRef *)outData) = CFSTR(kBox_UID);
		break;

	case kAudioBoxPropertyTransportType:

		//  This value represents how the device is attached to the system. This can be
		//  any 32 bit integer, but common values for this property are defined in
		//  <CoreAudio/AudioHardwareBase.h>
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetBoxPropertyData: not enough space for the return value of kAudioDevicePropertyTransportType for the box");
			return kAudioHardwareBadPropertySizeError;
		}

		*((UInt32 *)outData) = kAudioDeviceTransportTypeVirtual;
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioBoxPropertyHasAudio:

		//  Indicates whether or not the box has audio capabilities
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetBoxPropertyData: not enough space for the return value of kAudioBoxPropertyHasAudio for the box");
			return kAudioHardwareBadPropertySizeError;
		}

		*((UInt32 *)outData) = 1;
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioBoxPropertyHasVideo:

		//  Indicates whether or not the box has video capabilities
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetBoxPropertyData: not enough space for the return value of kAudioBoxPropertyHasVideo for the box");
			return kAudioHardwareBadPropertySizeError;
		}

		*((UInt32 *)outData) = 0;
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioBoxPropertyHasMIDI:

		//  Indicates whether or not the box has MIDI capabilities
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetBoxPropertyData: not enough space for the return value of kAudioBoxPropertyHasMIDI for the box");
			return kAudioHardwareBadPropertySizeError;
		}

		*((UInt32 *)outData) = 0;
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioBoxPropertyIsProtected:

		//  Indicates whether or not the box has requires authentication to use
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetBoxPropertyData: not enough space for the return value of kAudioBoxPropertyIsProtected for the box");
			return kAudioHardwareBadPropertySizeError;
		}

		*((UInt32 *)outData) = 0;
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioBoxPropertyAcquired:

		//  When set to a non-zero value, the device is acquired for use by the local machine
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetBoxPropertyData: not enough space for the return value of kAudioBoxPropertyAcquired for the box");
			return kAudioHardwareBadPropertySizeError;
		}

		pthread_mutex_lock(&gPlugIn_StateMutex);
		*((UInt32 *)outData) = gBox_Acquired ? 1 : 0;
		pthread_mutex_unlock(&gPlugIn_StateMutex);
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioBoxPropertyAcquisitionFailed:

		//  This is used for notifications to say when an attempt to acquire a device has failed.
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetBoxPropertyData: not enough space for the return value of kAudioBoxPropertyAcquisitionFailed for the box");
			return kAudioHardwareBadPropertySizeError;
		}

		*((UInt32 *)outData) = 0;
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioBoxPropertyDeviceList:
		//  This is used to indicate which devices came from this box
		pthread_mutex_lock(&gPlugIn_StateMutex);

		if (gBox_Acquired) {
			if (inDataSize < sizeof(AudioObjectID)) {
				DebugMsg("CaptainJack_GetBoxPropertyData: not enough space for the return value of kAudioBoxPropertyDeviceList for the box");
				return kAudioHardwareBadPropertySizeError;
			}

			*((AudioObjectID *)outData) = kObjectID_Device;
			*outDataSize = sizeof(AudioObjectID);
		} else {
			*outDataSize = 0;
		}

		pthread_mutex_unlock(&gPlugIn_StateMutex);
		break;

	default:
		return kAudioHardwareUnknownPropertyError;
	};

	return theAnswer;
}

static OSStatus CaptainJack_SetBoxPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, const void *inData, UInt32 *outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData, inDataSize, inData)
	//  declare the local variables
	OSStatus theAnswer = 0;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_SetBoxPropertyData: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_SetBoxPropertyData: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outNumberPropertiesChanged == NULL) {
		DebugMsg("CaptainJack_SetBoxPropertyData: no place to return the number of properties that changed");
		return kAudioHardwareIllegalOperationError;
	}

	if (outChangedAddresses == NULL) {
		DebugMsg("CaptainJack_SetBoxPropertyData: no place to return the properties that changed");
		return kAudioHardwareIllegalOperationError;
	}

	if (inObjectID != kObjectID_Box) {
		DebugMsg("CaptainJack_SetBoxPropertyData: not the box object");
		return kAudioHardwareBadObjectError;
	}

	//  initialize the returned number of changed properties
	*outNumberPropertiesChanged = 0;

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required. There is more detailed commentary about each
	//  property in the CaptainJack_GetPlugInPropertyData() method.
	switch (inAddress->mSelector) {
	case kAudioObjectPropertyName:
		//  Boxes should allow their name to be editable
	{
		if (inDataSize != sizeof(CFStringRef)) {
			DebugMsg("CaptainJack_SetBoxPropertyData: wrong size for the data for kAudioObjectPropertyName");
			return kAudioHardwareBadPropertySizeError;
		}

		CFStringRef *theNewName = (CFStringRef *)inData;
		pthread_mutex_lock(&gPlugIn_StateMutex);

		if ((theNewName != NULL) && (*theNewName != NULL)) {
			CFRetain(*theNewName);
		}

		if (gBox_Name != NULL) {
			CFRelease(gBox_Name);
		}

		gBox_Name = *theNewName;
		pthread_mutex_unlock(&gPlugIn_StateMutex);
		*outNumberPropertiesChanged = 1;
		outChangedAddresses[0].mSelector = kAudioObjectPropertyName;
		outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
		outChangedAddresses[0].mElement = kAudioObjectPropertyElementMaster;
	}
	break;

	case kAudioObjectPropertyIdentify:
		//  since we don't have any actual hardware to flash, we will schedule a notificaiton for
		//  this property off into the future as a testing thing. Note that a real implementation
		//  of this property should only send the notificaiton if the hardware wants the app to
		//  flash it's UI for the device.
	{
		syslog(LOG_NOTICE, "The identify property has been set on the Box implemented by the CaptainJack driver.");

		if (inDataSize != sizeof(UInt32)) {
			DebugMsg("CaptainJack_SetBoxPropertyData: wrong size for the data for kAudioObjectPropertyIdentify");
			return kAudioHardwareBadPropertySizeError;
		}

		dispatch_after(dispatch_time(0, 2ULL * 1000ULL * 1000ULL * 1000ULL), dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^() {
			AudioObjectPropertyAddress theAddress = { kAudioObjectPropertyIdentify, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster };
			gPlugIn_Host->PropertiesChanged(gPlugIn_Host, kObjectID_Box, 1, &theAddress);
		});
	}
	break;

	case kAudioBoxPropertyAcquired:
		//  When the box is acquired, it means the contents, namely the device, are available to the system
	{
		if (inDataSize != sizeof(UInt32)) {
			DebugMsg("CaptainJack_SetBoxPropertyData: wrong size for the data for kAudioBoxPropertyAcquired");
			return kAudioHardwareBadPropertySizeError;
		}

		pthread_mutex_lock(&gPlugIn_StateMutex);

		if (gBox_Acquired != (*((UInt32 *)inData) != 0)) {
			//  the new value is different from the old value, so save it
			gBox_Acquired = *((UInt32 *)inData) != 0;
			gPlugIn_Host->WriteToStorage(gPlugIn_Host, CFSTR("box acquired"), gBox_Acquired ? kCFBooleanTrue : kCFBooleanFalse);
			//  and it means that this property and the device list property have changed
			*outNumberPropertiesChanged = 2;
			outChangedAddresses[0].mSelector = kAudioBoxPropertyAcquired;
			outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
			outChangedAddresses[0].mElement = kAudioObjectPropertyElementMaster;
			outChangedAddresses[1].mSelector = kAudioBoxPropertyDeviceList;
			outChangedAddresses[1].mScope = kAudioObjectPropertyScopeGlobal;
			outChangedAddresses[1].mElement = kAudioObjectPropertyElementMaster;
			//  but it also means that the device list has changed for the plug-in too
			dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0),   ^() {
				AudioObjectPropertyAddress theAddress = { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster };
				gPlugIn_Host->PropertiesChanged(gPlugIn_Host, kObjectID_PlugIn, 1, &theAddress);
			});
		}

		pthread_mutex_unlock(&gPlugIn_StateMutex);
	}
	break;

	default:
		return kAudioHardwareUnknownPropertyError;
	};

	return theAnswer;
}


static Boolean  CaptainJack_HasDeviceProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress) {
	//  This method returns whether or not the given object has the given property.
#pragma unused(inClientProcessID)
	//  declare the local variables
	Boolean theAnswer = false;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_HasDeviceProperty: bad driver reference");
		return theAnswer;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_HasDeviceProperty: no address");
		return theAnswer;
	}

	if (inObjectID != kObjectID_Device) {
		DebugMsg("CaptainJack_HasDeviceProperty: not the device object");
		return theAnswer;
	}

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required. There is more detailed commentary about each
	//  property in the CaptainJack_GetDevicePropertyData() method.
	switch (inAddress->mSelector) {
	case kAudioObjectPropertyBaseClass:
	case kAudioObjectPropertyClass:
	case kAudioObjectPropertyOwner:
	case kAudioObjectPropertyName:
	case kAudioObjectPropertyManufacturer:
	case kAudioObjectPropertyOwnedObjects:
	case kAudioDevicePropertyDeviceUID:
	case kAudioDevicePropertyModelUID:
	case kAudioDevicePropertyTransportType:
	case kAudioDevicePropertyRelatedDevices:
	case kAudioDevicePropertyClockDomain:
	case kAudioDevicePropertyDeviceIsAlive:
	case kAudioDevicePropertyDeviceIsRunning:
	case kAudioObjectPropertyControlList:
	case kAudioDevicePropertyNominalSampleRate:
	case kAudioDevicePropertyAvailableNominalSampleRates:
	case kAudioDevicePropertyIsHidden:
	case kAudioDevicePropertyZeroTimeStampPeriod:
	case kAudioDevicePropertyIcon:
	case kAudioDevicePropertyStreams:
		return true;

	case kAudioDevicePropertyDeviceCanBeDefaultDevice:
	case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
	case kAudioDevicePropertyLatency:
	case kAudioDevicePropertySafetyOffset:
	case kAudioDevicePropertyPreferredChannelsForStereo:
	case kAudioDevicePropertyPreferredChannelLayout:
		return (inAddress->mScope == kAudioObjectPropertyScopeInput) || (inAddress->mScope == kAudioObjectPropertyScopeOutput);
	};

	return theAnswer;
}

static OSStatus CaptainJack_IsDevicePropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, Boolean *outIsSettable) {
	//  This method returns whether or not the given property on the object can have its value
	//  changed.
#pragma unused(inClientProcessID)
	//  declare the local variables
	OSStatus theAnswer = 0;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_IsDevicePropertySettable: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_IsDevicePropertySettable: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outIsSettable == NULL) {
		DebugMsg("CaptainJack_IsDevicePropertySettable: no place to put the return value");
		return kAudioHardwareIllegalOperationError;
	}

	if (inObjectID != kObjectID_Device) {
		DebugMsg("CaptainJack_IsDevicePropertySettable: not the device object");
		return kAudioHardwareBadObjectError;
	}

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required. There is more detailed commentary about each
	//  property in the CaptainJack_GetDevicePropertyData() method.
	switch (inAddress->mSelector) {
	case kAudioObjectPropertyBaseClass:
	case kAudioObjectPropertyClass:
	case kAudioObjectPropertyOwner:
	case kAudioObjectPropertyName:
	case kAudioObjectPropertyManufacturer:
	case kAudioObjectPropertyOwnedObjects:
	case kAudioDevicePropertyDeviceUID:
	case kAudioDevicePropertyModelUID:
	case kAudioDevicePropertyTransportType:
	case kAudioDevicePropertyRelatedDevices:
	case kAudioDevicePropertyClockDomain:
	case kAudioDevicePropertyDeviceIsAlive:
	case kAudioDevicePropertyDeviceIsRunning:
	case kAudioDevicePropertyDeviceCanBeDefaultDevice:
	case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
	case kAudioDevicePropertyLatency:
	case kAudioDevicePropertyStreams:
	case kAudioObjectPropertyControlList:
	case kAudioDevicePropertySafetyOffset:
	case kAudioDevicePropertyAvailableNominalSampleRates:
	case kAudioDevicePropertyIsHidden:
	case kAudioDevicePropertyPreferredChannelsForStereo:
	case kAudioDevicePropertyPreferredChannelLayout:
	case kAudioDevicePropertyZeroTimeStampPeriod:
	case kAudioDevicePropertyIcon:
		*outIsSettable = false;
		break;

	case kAudioDevicePropertyNominalSampleRate:
		*outIsSettable = true;
		break;

	default:
		return kAudioHardwareUnknownPropertyError;
	};

	return theAnswer;
}

static OSStatus CaptainJack_GetDevicePropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 *outDataSize) {
	//  This method returns the byte size of the property's data.
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
	//  declare the local variables
	OSStatus theAnswer = 0;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_GetDevicePropertyDataSize: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_GetDevicePropertyDataSize: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outDataSize == NULL) {
		DebugMsg("CaptainJack_GetDevicePropertyDataSize: no place to put the return value");
		return kAudioHardwareIllegalOperationError;
	}

	if (inObjectID != kObjectID_Device) {
		DebugMsg("CaptainJack_GetDevicePropertyDataSize: not the device object");
		return kAudioHardwareBadObjectError;
	}

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required. There is more detailed commentary about each
	//  property in the CaptainJack_GetDevicePropertyData() method.
	switch (inAddress->mSelector) {
	case kAudioObjectPropertyBaseClass:
		*outDataSize = sizeof(AudioClassID);
		break;

	case kAudioObjectPropertyClass:
		*outDataSize = sizeof(AudioClassID);
		break;

	case kAudioObjectPropertyOwner:
		*outDataSize = sizeof(AudioObjectID);
		break;

	case kAudioObjectPropertyName:
		*outDataSize = sizeof(CFStringRef);
		break;

	case kAudioObjectPropertyManufacturer:
		*outDataSize = sizeof(CFStringRef);
		break;

	case kAudioObjectPropertyOwnedObjects:
		switch (inAddress->mScope) {
		case kAudioObjectPropertyScopeGlobal:
			*outDataSize = 8 * sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyScopeInput:
			*outDataSize = 4 * sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyScopeOutput:
			*outDataSize = 4 * sizeof(AudioObjectID);
			break;
		};

		break;

	case kAudioDevicePropertyDeviceUID:
		*outDataSize = sizeof(CFStringRef);
		break;

	case kAudioDevicePropertyModelUID:
		*outDataSize = sizeof(CFStringRef);
		break;

	case kAudioDevicePropertyTransportType:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioDevicePropertyRelatedDevices:
		*outDataSize = sizeof(AudioObjectID);
		break;

	case kAudioDevicePropertyClockDomain:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioDevicePropertyDeviceIsAlive:
		*outDataSize = sizeof(AudioClassID);
		break;

	case kAudioDevicePropertyDeviceIsRunning:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioDevicePropertyDeviceCanBeDefaultDevice:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioDevicePropertyLatency:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioDevicePropertyStreams:
		switch (inAddress->mScope) {
		case kAudioObjectPropertyScopeGlobal:
			*outDataSize = 2 * sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyScopeInput:
			*outDataSize = 1 * sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyScopeOutput:
			*outDataSize = 1 * sizeof(AudioObjectID);
			break;
		};

		break;

	case kAudioObjectPropertyControlList:
		*outDataSize = 6 * sizeof(AudioObjectID);
		break;

	case kAudioDevicePropertySafetyOffset:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioDevicePropertyNominalSampleRate:
		*outDataSize = sizeof(Float64);
		break;

	case kAudioDevicePropertyAvailableNominalSampleRates:
		*outDataSize = 2 * sizeof(AudioValueRange);
		break;

	case kAudioDevicePropertyIsHidden:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioDevicePropertyPreferredChannelsForStereo:
		*outDataSize = 2 * sizeof(UInt32);
		break;

	case kAudioDevicePropertyPreferredChannelLayout:
		*outDataSize = offsetof(AudioChannelLayout, mChannelDescriptions) + (2 * sizeof(AudioChannelDescription));
		break;

	case kAudioDevicePropertyZeroTimeStampPeriod:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioDevicePropertyIcon:
		*outDataSize = sizeof(CFURLRef);
		break;

	default:
		return kAudioHardwareUnknownPropertyError;
	};

	return theAnswer;
}

static OSStatus CaptainJack_GetDevicePropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
	//  declare the local variables
	OSStatus theAnswer = 0;
	UInt32 theNumberItemsToFetch;
	UInt32 theItemIndex;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_GetDevicePropertyData: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_GetDevicePropertyData: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outDataSize == NULL) {
		DebugMsg("CaptainJack_GetDevicePropertyData: no place to put the return value size");
		return kAudioHardwareIllegalOperationError;
	}

	if (outData == NULL) {
		DebugMsg("CaptainJack_GetDevicePropertyData: no place to put the return value");
		return kAudioHardwareIllegalOperationError;
	}

	if (inObjectID != kObjectID_Device) {
		DebugMsg("CaptainJack_GetDevicePropertyData: not the device object");
		return kAudioHardwareBadObjectError;
	}

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required.
	//
	//  Also, since most of the data that will get returned is static, there are few instances where
	//  it is necessary to lock the state mutex.
	switch (inAddress->mSelector) {
	case kAudioObjectPropertyBaseClass:

		//  The base class for kAudioDeviceClassID is kAudioObjectClassID
		if (inDataSize < sizeof(AudioClassID)) {
			DebugMsg("CaptainJack_GetDevicePropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the device");
			return kAudioHardwareBadPropertySizeError;
		}

		*((AudioClassID *)outData) = kAudioObjectClassID;
		*outDataSize = sizeof(AudioClassID);
		break;

	case kAudioObjectPropertyClass:

		//  The class is always kAudioDeviceClassID for devices created by drivers
		if (inDataSize < sizeof(AudioClassID)) {
			DebugMsg("CaptainJack_GetDevicePropertyData: not enough space for the return value of kAudioObjectPropertyClass for the device");
			return kAudioHardwareBadPropertySizeError;
		}

		*((AudioClassID *)outData) = kAudioDeviceClassID;
		*outDataSize = sizeof(AudioClassID);
		break;

	case kAudioObjectPropertyOwner:

		//  The device's owner is the plug-in object
		if (inDataSize < sizeof(AudioObjectID)) {
			DebugMsg("CaptainJack_GetDevicePropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the device");
			return kAudioHardwareBadPropertySizeError;
		}

		*((AudioObjectID *)outData) = kObjectID_PlugIn;
		*outDataSize = sizeof(AudioObjectID);
		break;

	case kAudioObjectPropertyName:

		//  This is the human readable name of the device.
		if (inDataSize < sizeof(CFStringRef)) {
			DebugMsg("CaptainJack_GetDevicePropertyData: not enough space for the return value of kAudioObjectPropertyManufacturer for the device");
			return kAudioHardwareBadPropertySizeError;
		}

		*((CFStringRef *)outData) = CFSTR("DeviceName");
		*outDataSize = sizeof(CFStringRef);
		break;

	case kAudioObjectPropertyManufacturer:

		//  This is the human readable name of the maker of the plug-in.
		if (inDataSize < sizeof(CFStringRef)) {
			DebugMsg("CaptainJack_GetDevicePropertyData: not enough space for the return value of kAudioObjectPropertyManufacturer for the device");
			return kAudioHardwareBadPropertySizeError;
		}

		*((CFStringRef *)outData) = CFSTR("ManufacturerName");
		*outDataSize = sizeof(CFStringRef);
		break;

	case kAudioObjectPropertyOwnedObjects:
		//  Calculate the number of items that have been requested. Note that this
		//  number is allowed to be smaller than the actual size of the list. In such
		//  case, only that number of items will be returned
		theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);

		//  The device owns its streams and controls. Note that what is returned here
		//  depends on the scope requested.
		switch (inAddress->mScope) {
		case kAudioObjectPropertyScopeGlobal:

			//  global scope means return all objects
			if (theNumberItemsToFetch > 8) {
				theNumberItemsToFetch = 8;
			}

			//  fill out the list with as many objects as requested, which is everything
			for (theItemIndex = 0; theItemIndex < theNumberItemsToFetch; ++theItemIndex) {
				((AudioObjectID *)outData)[theItemIndex] = kObjectID_Stream_Input + theItemIndex;
			}

			break;

		case kAudioObjectPropertyScopeInput:

			//  input scope means just the objects on the input side
			if (theNumberItemsToFetch > 4) {
				theNumberItemsToFetch = 4;
			}

			//  fill out the list with the right objects
			for (theItemIndex = 0; theItemIndex < theNumberItemsToFetch; ++theItemIndex) {
				((AudioObjectID *)outData)[theItemIndex] = kObjectID_Stream_Input + theItemIndex;
			}

			break;

		case kAudioObjectPropertyScopeOutput:

			//  output scope means just the objects on the output side
			if (theNumberItemsToFetch > 4) {
				theNumberItemsToFetch = 4;
			}

			//  fill out the list with the right objects
			for (theItemIndex = 0; theItemIndex < theNumberItemsToFetch; ++theItemIndex) {
				((AudioObjectID *)outData)[theItemIndex] = kObjectID_Stream_Output + theItemIndex;
			}

			break;
		};

		//  report how much we wrote
		*outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);

		break;

	case kAudioDevicePropertyDeviceUID:

		//  This is a CFString that is a persistent token that can identify the same
		//  audio device across boot sessions. Note that two instances of the same
		//  device must have different values for this property.
		if (inDataSize < sizeof(CFStringRef)) {
			DebugMsg("CaptainJack_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyDeviceUID for the device");
			return kAudioHardwareBadPropertySizeError;
		}

		*((CFStringRef *)outData) = CFSTR(kDevice_UID);
		*outDataSize = sizeof(CFStringRef);
		break;

	case kAudioDevicePropertyModelUID:

		//  This is a CFString that is a persistent token that can identify audio
		//  devices that are the same kind of device. Note that two instances of the
		//  save device must have the same value for this property.
		if (inDataSize < sizeof(CFStringRef)) {
			DebugMsg("CaptainJack_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyModelUID for the device");
			return kAudioHardwareBadPropertySizeError;
		}

		*((CFStringRef *)outData) = CFSTR(kDevice_ModelUID);
		*outDataSize = sizeof(CFStringRef);
		break;

	case kAudioDevicePropertyTransportType:

		//  This value represents how the device is attached to the system. This can be
		//  any 32 bit integer, but common values for this property are defined in
		//  <CoreAudio/AudioHardwareBase.h>
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyTransportType for the device");
			return kAudioHardwareBadPropertySizeError;
		}

		*((UInt32 *)outData) = kAudioDeviceTransportTypeVirtual;
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioDevicePropertyRelatedDevices:
		//  The related devices property identifys device objects that are very closely
		//  related. Generally, this is for relating devices that are packaged together
		//  in the hardware such as when the input side and the output side of a piece
		//  of hardware can be clocked separately and therefore need to be represented
		//  as separate AudioDevice objects. In such case, both devices would report
		//  that they are related to each other. Note that at minimum, a device is
		//  related to itself, so this list will always be at least one item long.
		//  Calculate the number of items that have been requested. Note that this
		//  number is allowed to be smaller than the actual size of the list. In such
		//  case, only that number of items will be returned
		theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);

		//  we only have the one device...
		if (theNumberItemsToFetch > 1) {
			theNumberItemsToFetch = 1;
		}

		//  Write the devices' object IDs into the return value
		if (theNumberItemsToFetch > 0) {
			((AudioObjectID *)outData)[0] = kObjectID_Device;
		}

		//  report how much we wrote
		*outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);
		break;

	case kAudioDevicePropertyClockDomain:

		//  This property allows the device to declare what other devices it is
		//  synchronized with in hardware. The way it works is that if two devices have
		//  the same value for this property and the value is not zero, then the two
		//  devices are synchronized in hardware. Note that a device that either can't
		//  be synchronized with others or doesn't know should return 0 for this
		//  property.
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyClockDomain for the device");
			return kAudioHardwareBadPropertySizeError;
		}

		*((UInt32 *)outData) = 0;
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioDevicePropertyDeviceIsAlive:

		//  This property returns whether or not the device is alive. Note that it is
		//  note uncommon for a device to be dead but still momentarily availble in the
		//  device list. In the case of this device, it will always be alive.
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyDeviceIsAlive for the device");
			return kAudioHardwareBadPropertySizeError;
		}

		*((UInt32 *)outData) = 1;
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioDevicePropertyDeviceIsRunning:

		//  This property returns whether or not IO is running for the device. Note that
		//  we need to take both the state lock to check this value for thread safety.
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyDeviceIsRunning for the device");
			return kAudioHardwareBadPropertySizeError;
		}

		pthread_mutex_lock(&gPlugIn_StateMutex);
		*((UInt32 *)outData) = ((gDevice_IOIsRunning > 0) > 0) ? 1 : 0;
		pthread_mutex_unlock(&gPlugIn_StateMutex);
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioDevicePropertyDeviceCanBeDefaultDevice:

		//  This property returns whether or not the device wants to be able to be the
		//  default device for content. This is the device that iTunes and QuickTime
		//  will use to play their content on and FaceTime will use as it's microhphone.
		//  Nearly all devices should allow for this.
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyDeviceCanBeDefaultDevice for the device");
			return kAudioHardwareBadPropertySizeError;
		}

		*((UInt32 *)outData) = 1;
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:

		//  This property returns whether or not the device wants to be the system
		//  default device. This is the device that is used to play interface sounds and
		//  other incidental or UI-related sounds on. Most devices should allow this
		//  although devices with lots of latency may not want to.
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyDeviceCanBeDefaultSystemDevice for the device");
			return kAudioHardwareBadPropertySizeError;
		}

		*((UInt32 *)outData) = 1;
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioDevicePropertyLatency:

		//  This property returns the presentation latency of the device. For this,
		//  device, the value is 0 due to the fact that it always vends silence.
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyLatency for the device");
			return kAudioHardwareBadPropertySizeError;
		}

		*((UInt32 *)outData) = 0;
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioDevicePropertyStreams:
		//  Calculate the number of items that have been requested. Note that this
		//  number is allowed to be smaller than the actual size of the list. In such
		//  case, only that number of items will be returned
		theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);

		//  Note that what is returned here depends on the scope requested.
		switch (inAddress->mScope) {
		case kAudioObjectPropertyScopeGlobal:

			//  global scope means return all streams
			if (theNumberItemsToFetch > 2) {
				theNumberItemsToFetch = 2;
			}

			//  fill out the list with as many objects as requested
			if (theNumberItemsToFetch > 0) {
				((AudioObjectID *)outData)[0] = kObjectID_Stream_Input;
			}

			if (theNumberItemsToFetch > 1) {
				((AudioObjectID *)outData)[1] = kObjectID_Stream_Output;
			}

			break;

		case kAudioObjectPropertyScopeInput:

			//  input scope means just the objects on the input side
			if (theNumberItemsToFetch > 1) {
				theNumberItemsToFetch = 1;
			}

			//  fill out the list with as many objects as requested
			if (theNumberItemsToFetch > 0) {
				((AudioObjectID *)outData)[0] = kObjectID_Stream_Input;
			}

			break;

		case kAudioObjectPropertyScopeOutput:

			//  output scope means just the objects on the output side
			if (theNumberItemsToFetch > 1) {
				theNumberItemsToFetch = 1;
			}

			//  fill out the list with as many objects as requested
			if (theNumberItemsToFetch > 0) {
				((AudioObjectID *)outData)[0] = kObjectID_Stream_Output;
			}

			break;
		};

		//  report how much we wrote
		*outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);

		break;

	case kAudioObjectPropertyControlList:
		//  Calculate the number of items that have been requested. Note that this
		//  number is allowed to be smaller than the actual size of the list. In such
		//  case, only that number of items will be returned
		theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);

		if (theNumberItemsToFetch > 6) {
			theNumberItemsToFetch = 6;
		}

		//  fill out the list with as many objects as requested, which is everything
		for (theItemIndex = 0; theItemIndex < theNumberItemsToFetch; ++theItemIndex) {
			if (theItemIndex < 3) {
				((AudioObjectID *)outData)[theItemIndex] = kObjectID_Volume_Input_Master + theItemIndex;
			} else {
				((AudioObjectID *)outData)[theItemIndex] = kObjectID_Volume_Output_Master + (theItemIndex - 3);
			}
		}

		//  report how much we wrote
		*outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);
		break;

	case kAudioDevicePropertySafetyOffset:

		//  This property returns the how close to now the HAL can read and write. For
		//  this, device, the value is 0 due to the fact that it always vends silence.
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertySafetyOffset for the device");
			return kAudioHardwareBadPropertySizeError;
		}

		*((UInt32 *)outData) = 0;
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioDevicePropertyNominalSampleRate:

		//  This property returns the nominal sample rate of the device. Note that we
		//  only need to take the state lock to get this value.
		if (inDataSize < sizeof(Float64)) {
			DebugMsg("CaptainJack_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyNominalSampleRate for the device");
			return kAudioHardwareBadPropertySizeError;
		}

		pthread_mutex_lock(&gPlugIn_StateMutex);
		*((Float64 *)outData) = gDevice_SampleRate;
		pthread_mutex_unlock(&gPlugIn_StateMutex);
		*outDataSize = sizeof(Float64);
		break;

	case kAudioDevicePropertyAvailableNominalSampleRates:
		//  This returns all nominal sample rates the device supports as an array of
		//  AudioValueRangeStructs. Note that for discrete sampler rates, the range
		//  will have the minimum value equal to the maximum value.
		//  Calculate the number of items that have been requested. Note that this
		//  number is allowed to be smaller than the actual size of the list. In such
		//  case, only that number of items will be returned
		theNumberItemsToFetch = inDataSize / sizeof(AudioValueRange);

		//  clamp it to the number of items we have
		if (theNumberItemsToFetch > 2) {
			theNumberItemsToFetch = 2;
		}

		//  fill out the return array
		if (theNumberItemsToFetch > 0) {
			((AudioValueRange *)outData)[0].mMinimum = 44100.0;
			((AudioValueRange *)outData)[0].mMaximum = 44100.0;
		}

		if (theNumberItemsToFetch > 1) {
			((AudioValueRange *)outData)[1].mMinimum = 48000.0;
			((AudioValueRange *)outData)[1].mMaximum = 48000.0;
		}

		//  report how much we wrote
		*outDataSize = theNumberItemsToFetch * sizeof(AudioValueRange);
		break;

	case kAudioDevicePropertyIsHidden:

		//  This returns whether or not the device is visible to clients.
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyIsHidden for the device");
			return kAudioHardwareBadPropertySizeError;
		}

		*((UInt32 *)outData) = 0;
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioDevicePropertyPreferredChannelsForStereo:

		//  This property returns which two channesl to use as left/right for stereo
		//  data by default. Note that the channel numbers are 1-based.xz
		if (inDataSize < (2 * sizeof(UInt32))) {
			DebugMsg("CaptainJack_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyPreferredChannelsForStereo for the device");
			return kAudioHardwareBadPropertySizeError;
		}

		((UInt32 *)outData)[0] = 1;
		((UInt32 *)outData)[1] = 2;
		*outDataSize = 2 * sizeof(UInt32);
		break;

	case kAudioDevicePropertyPreferredChannelLayout:
		//  This property returns the default AudioChannelLayout to use for the device
		//  by default. For this device, we return a stereo ACL.
	{
		//  calcualte how big the
		UInt32 theACLSize = offsetof(AudioChannelLayout, mChannelDescriptions) + (2 * sizeof(AudioChannelDescription));

		if (inDataSize < theACLSize) {
			DebugMsg("CaptainJack_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyPreferredChannelLayout for the device");
			return kAudioHardwareBadPropertySizeError;
		}

		((AudioChannelLayout *)outData)->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
		((AudioChannelLayout *)outData)->mChannelBitmap = 0;
		((AudioChannelLayout *)outData)->mNumberChannelDescriptions = 2;

		for (theItemIndex = 0; theItemIndex < 2; ++theItemIndex) {
			((AudioChannelLayout *)outData)->mChannelDescriptions[theItemIndex].mChannelLabel = kAudioChannelLabel_Left + theItemIndex;
			((AudioChannelLayout *)outData)->mChannelDescriptions[theItemIndex].mChannelFlags = 0;
			((AudioChannelLayout *)outData)->mChannelDescriptions[theItemIndex].mCoordinates[0] = 0;
			((AudioChannelLayout *)outData)->mChannelDescriptions[theItemIndex].mCoordinates[1] = 0;
			((AudioChannelLayout *)outData)->mChannelDescriptions[theItemIndex].mCoordinates[2] = 0;
		}

		*outDataSize = theACLSize;
	}
	break;

	case kAudioDevicePropertyZeroTimeStampPeriod:

		//  This property returns how many frames the HAL should expect to see between
		//  successive sample times in the zero time stamps this device provides.
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyZeroTimeStampPeriod for the device");
			return kAudioHardwareBadPropertySizeError;
		}

		*((UInt32 *)outData) = kDevice_RingBufferSize;
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioDevicePropertyIcon: {
		//  This is a CFURL that points to the device's Icon in the plug-in's resource bundle.
		if (inDataSize < sizeof(CFURLRef)) {
			DebugMsg("CaptainJack_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyDeviceUID for the device");
			return kAudioHardwareBadPropertySizeError;
		}

		CFBundleRef theBundle = CFBundleGetBundleWithIdentifier(CFSTR(kPlugIn_BundleID));

		if (theBundle == NULL) {
			DebugMsg("CaptainJack_GetDevicePropertyData: could not get the plug-in bundle for kAudioDevicePropertyIcon");
			return kAudioHardwareUnspecifiedError;
		}

		CFURLRef theURL = CFBundleCopyResourceURL(theBundle, CFSTR("DeviceIcon.icns"), NULL, NULL);

		if (theURL == NULL) {
			DebugMsg("CaptainJack_GetDevicePropertyData: could not get the URL for kAudioDevicePropertyIcon");
			return kAudioHardwareUnspecifiedError;
		}

		*((CFURLRef *)outData) = theURL;
		*outDataSize = sizeof(CFURLRef);
	}
	break;

	default:
		return kAudioHardwareUnknownPropertyError;
	};

	return theAnswer;
}

static OSStatus CaptainJack_SetDevicePropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, const void *inData, UInt32 *outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
	//  declare the local variables
	OSStatus theAnswer = 0;
	Float64 theOldSampleRate;
	UInt64 theNewSampleRate;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_SetDevicePropertyData: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_SetDevicePropertyData: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outNumberPropertiesChanged == NULL) {
		DebugMsg("CaptainJack_SetDevicePropertyData: no place to return the number of properties that changed");
		return kAudioHardwareIllegalOperationError;
	}

	if (outChangedAddresses == NULL) {
		DebugMsg("CaptainJack_SetDevicePropertyData: no place to return the properties that changed");
		return kAudioHardwareIllegalOperationError;
	}

	if (inObjectID != kObjectID_Device) {
		DebugMsg("CaptainJack_SetDevicePropertyData: not the device object");
		return kAudioHardwareBadObjectError;
	}

	//  initialize the returned number of changed properties
	*outNumberPropertiesChanged = 0;

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required. There is more detailed commentary about each
	//  property in the CaptainJack_GetDevicePropertyData() method.
	switch (inAddress->mSelector) {
	case kAudioDevicePropertyNominalSampleRate:

		//  Changing the sample rate needs to be handled via the
		//  RequestConfigChange/PerformConfigChange machinery.
		//  check the arguments
		if (inDataSize != sizeof(Float64)) {
			DebugMsg("CaptainJack_SetDevicePropertyData: wrong size for the data for kAudioDevicePropertyNominalSampleRate");
			return kAudioHardwareBadPropertySizeError;
		}

		if ((*((const Float64 *)inData) != 44100.0) && (*((const Float64 *)inData) != 48000.0)) {
			DebugMsg("CaptainJack_SetDevicePropertyData: unsupported value for kAudioDevicePropertyNominalSampleRate");
			return kAudioHardwareIllegalOperationError;
		}

		//  make sure that the new value is different than the old value
		pthread_mutex_lock(&gPlugIn_StateMutex);
		theOldSampleRate = gDevice_SampleRate;
		pthread_mutex_unlock(&gPlugIn_StateMutex);

		if (*((const Float64 *)inData) != theOldSampleRate) {
			//  we dispatch this so that the change can happen asynchronously
			theOldSampleRate = *((const Float64 *)inData);
			theNewSampleRate = (UInt64)theOldSampleRate;
			dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^ { gPlugIn_Host->RequestDeviceConfigurationChange(gPlugIn_Host, kObjectID_Device, theNewSampleRate, NULL); });
		}

		break;

	default:
		return kAudioHardwareUnknownPropertyError;
	};

	return theAnswer;
}


static Boolean  CaptainJack_HasStreamProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress) {
	//  This method returns whether or not the given object has the given property.
#pragma unused(inClientProcessID)
	//  declare the local variables
	Boolean theAnswer = false;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_HasStreamProperty: bad driver reference");
		return theAnswer;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_HasStreamProperty: no address");
		return theAnswer;
	}

	if ((inObjectID != kObjectID_Stream_Input) && (inObjectID != kObjectID_Stream_Output)) {
		DebugMsg("CaptainJack_HasStreamProperty: not a stream object");
		return theAnswer;
	}

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required. There is more detailed commentary about each
	//  property in the CaptainJack_GetStreamPropertyData() method.
	switch (inAddress->mSelector) {
	case kAudioObjectPropertyBaseClass:
	case kAudioObjectPropertyClass:
	case kAudioObjectPropertyOwner:
	case kAudioObjectPropertyOwnedObjects:
	case kAudioStreamPropertyIsActive:
	case kAudioStreamPropertyDirection:
	case kAudioStreamPropertyTerminalType:
	case kAudioStreamPropertyStartingChannel:
	case kAudioStreamPropertyLatency:
	case kAudioStreamPropertyVirtualFormat:
	case kAudioStreamPropertyPhysicalFormat:
	case kAudioStreamPropertyAvailableVirtualFormats:
	case kAudioStreamPropertyAvailablePhysicalFormats:
		return true;
	};

	return theAnswer;
}

static OSStatus CaptainJack_IsStreamPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, Boolean *outIsSettable) {
	//  This method returns whether or not the given property on the object can have its value
	//  changed.
#pragma unused(inClientProcessID)
	//  declare the local variables
	OSStatus theAnswer = 0;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_IsStreamPropertySettable: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_IsStreamPropertySettable: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outIsSettable == NULL) {
		DebugMsg("CaptainJack_IsStreamPropertySettable: no place to put the return value");
		return kAudioHardwareIllegalOperationError;
	}

	if ((inObjectID != kObjectID_Stream_Input) && (inObjectID != kObjectID_Stream_Output)) {
		DebugMsg("CaptainJack_IsStreamPropertySettable: not a stream object");
		return kAudioHardwareBadObjectError;
	}

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required. There is more detailed commentary about each
	//  property in the CaptainJack_GetStreamPropertyData() method.
	switch (inAddress->mSelector) {
	case kAudioObjectPropertyBaseClass:
	case kAudioObjectPropertyClass:
	case kAudioObjectPropertyOwner:
	case kAudioObjectPropertyOwnedObjects:
	case kAudioStreamPropertyDirection:
	case kAudioStreamPropertyTerminalType:
	case kAudioStreamPropertyStartingChannel:
	case kAudioStreamPropertyLatency:
	case kAudioStreamPropertyAvailableVirtualFormats:
	case kAudioStreamPropertyAvailablePhysicalFormats:
		*outIsSettable = false;
		break;

	case kAudioStreamPropertyIsActive:
	case kAudioStreamPropertyVirtualFormat:
	case kAudioStreamPropertyPhysicalFormat:
		*outIsSettable = true;
		break;

	default:
		return kAudioHardwareUnknownPropertyError;
	};

	return theAnswer;
}

static OSStatus CaptainJack_GetStreamPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 *outDataSize) {
	//  This method returns the byte size of the property's data.
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
	//  declare the local variables
	OSStatus theAnswer = 0;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_GetStreamPropertyDataSize: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_GetStreamPropertyDataSize: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outDataSize == NULL) {
		DebugMsg("CaptainJack_GetStreamPropertyDataSize: no place to put the return value");
		return kAudioHardwareIllegalOperationError;
	}

	if ((inObjectID != kObjectID_Stream_Input) && (inObjectID != kObjectID_Stream_Output)) {
		DebugMsg("CaptainJack_GetStreamPropertyDataSize: not a stream object");
		return kAudioHardwareBadObjectError;
	}

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required. There is more detailed commentary about each
	//  property in the CaptainJack_GetStreamPropertyData() method.
	switch (inAddress->mSelector) {
	case kAudioObjectPropertyBaseClass:
		*outDataSize = sizeof(AudioClassID);
		break;

	case kAudioObjectPropertyClass:
		*outDataSize = sizeof(AudioClassID);
		break;

	case kAudioObjectPropertyOwner:
		*outDataSize = sizeof(AudioObjectID);
		break;

	case kAudioObjectPropertyOwnedObjects:
		*outDataSize = 0 * sizeof(AudioObjectID);
		break;

	case kAudioStreamPropertyIsActive:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioStreamPropertyDirection:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioStreamPropertyTerminalType:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioStreamPropertyStartingChannel:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioStreamPropertyLatency:
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioStreamPropertyVirtualFormat:
	case kAudioStreamPropertyPhysicalFormat:
		*outDataSize = sizeof(AudioStreamBasicDescription);
		break;

	case kAudioStreamPropertyAvailableVirtualFormats:
	case kAudioStreamPropertyAvailablePhysicalFormats:
		*outDataSize = 2 * sizeof(AudioStreamRangedDescription);
		break;

	default:
		return kAudioHardwareUnknownPropertyError;
	};

	return theAnswer;
}

static OSStatus CaptainJack_GetStreamPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
	//  declare the local variables
	OSStatus theAnswer = 0;
	UInt32 theNumberItemsToFetch;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_GetStreamPropertyData: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_GetStreamPropertyData: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outDataSize == NULL) {
		DebugMsg("CaptainJack_GetStreamPropertyData: no place to put the return value size");
		return kAudioHardwareIllegalOperationError;
	}

	if (outData == NULL) {
		DebugMsg("CaptainJack_GetStreamPropertyData: no place to put the return value");
		return kAudioHardwareIllegalOperationError;
	}

	if ((inObjectID != kObjectID_Stream_Input) && (inObjectID != kObjectID_Stream_Output)) {
		DebugMsg("CaptainJack_GetStreamPropertyData: not a stream object");
		return kAudioHardwareBadObjectError;
	}

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required.
	//
	//  Also, since most of the data that will get returned is static, there are few instances where
	//  it is necessary to lock the state mutex.
	switch (inAddress->mSelector) {
	case kAudioObjectPropertyBaseClass:

		//  The base class for kAudioStreamClassID is kAudioObjectClassID
		if (inDataSize < sizeof(AudioClassID)) {
			DebugMsg("CaptainJack_GetStreamPropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the stream");
			return kAudioHardwareBadPropertySizeError;
		}

		*((AudioClassID *)outData) = kAudioObjectClassID;
		*outDataSize = sizeof(AudioClassID);
		break;

	case kAudioObjectPropertyClass:

		//  The class is always kAudioStreamClassID for streams created by drivers
		if (inDataSize < sizeof(AudioClassID)) {
			DebugMsg("CaptainJack_GetStreamPropertyData: not enough space for the return value of kAudioObjectPropertyClass for the stream");
			return kAudioHardwareBadPropertySizeError;
		}

		*((AudioClassID *)outData) = kAudioStreamClassID;
		*outDataSize = sizeof(AudioClassID);
		break;

	case kAudioObjectPropertyOwner:

		//  The stream's owner is the device object
		if (inDataSize < sizeof(AudioObjectID)) {
			DebugMsg("CaptainJack_GetStreamPropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the stream");
			return kAudioHardwareBadPropertySizeError;
		}

		*((AudioObjectID *)outData) = kObjectID_Device;
		*outDataSize = sizeof(AudioObjectID);
		break;

	case kAudioObjectPropertyOwnedObjects:
		//  Streams do not own any objects
		*outDataSize = 0 * sizeof(AudioObjectID);
		break;

	case kAudioStreamPropertyIsActive:

		//  This property tells the device whether or not the given stream is going to
		//  be used for IO. Note that we need to take the state lock to examine this
		//  value.
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyIsActive for the stream");
			return kAudioHardwareBadPropertySizeError;
		}

		pthread_mutex_lock(&gPlugIn_StateMutex);
		*((UInt32 *)outData) = (inObjectID == kObjectID_Stream_Input) ? gStream_Input_IsActive : gStream_Output_IsActive;
		pthread_mutex_unlock(&gPlugIn_StateMutex);
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioStreamPropertyDirection:

		//  This returns whether the stream is an input stream or an output stream.
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyDirection for the stream");
			return kAudioHardwareBadPropertySizeError;
		}

		*((UInt32 *)outData) = (inObjectID == kObjectID_Stream_Input) ? 1 : 0;
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioStreamPropertyTerminalType:

		//  This returns a value that indicates what is at the other end of the stream
		//  such as a speaker or headphones, or a microphone. Values for this property
		//  are defined in <CoreAudio/AudioHardwareBase.h>
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyTerminalType for the stream");
			return kAudioHardwareBadPropertySizeError;
		}

		*((UInt32 *)outData) = (inObjectID == kObjectID_Stream_Input) ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker;
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioStreamPropertyStartingChannel:

		//  This property returns the absolute channel number for the first channel in
		//  the stream. For exmaple, if a device has two output streams with two
		//  channels each, then the starting channel number for the first stream is 1
		//  and ths starting channel number fo the second stream is 3.
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyStartingChannel for the stream");
			return kAudioHardwareBadPropertySizeError;
		}

		*((UInt32 *)outData) = 1;
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioStreamPropertyLatency:

		//  This property returns any additonal presentation latency the stream has.
		if (inDataSize < sizeof(UInt32)) {
			DebugMsg("CaptainJack_GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyStartingChannel for the stream");
			return kAudioHardwareBadPropertySizeError;
		}

		*((UInt32 *)outData) = 0;
		*outDataSize = sizeof(UInt32);
		break;

	case kAudioStreamPropertyVirtualFormat:
	case kAudioStreamPropertyPhysicalFormat:

		//  This returns the current format of the stream in an
		//  AudioStreamBasicDescription. Note that we need to hold the state lock to get
		//  this value.
		//  Note that for devices that don't override the mix operation, the virtual
		//  format has to be the same as the physical format.
		if (inDataSize < sizeof(AudioStreamBasicDescription)) {
			DebugMsg("CaptainJack_GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyVirtualFormat for the stream");
			return kAudioHardwareBadPropertySizeError;
		}

		pthread_mutex_lock(&gPlugIn_StateMutex);
		((AudioStreamBasicDescription *)outData)->mSampleRate = gDevice_SampleRate;
		((AudioStreamBasicDescription *)outData)->mFormatID = kAudioFormatLinearPCM;
		((AudioStreamBasicDescription *)outData)->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
		((AudioStreamBasicDescription *)outData)->mBytesPerPacket = 8;
		((AudioStreamBasicDescription *)outData)->mFramesPerPacket = 1;
		((AudioStreamBasicDescription *)outData)->mBytesPerFrame = 8;
		((AudioStreamBasicDescription *)outData)->mChannelsPerFrame = 2;
		((AudioStreamBasicDescription *)outData)->mBitsPerChannel = 32;
		pthread_mutex_unlock(&gPlugIn_StateMutex);
		*outDataSize = sizeof(AudioStreamBasicDescription);
		break;

	case kAudioStreamPropertyAvailableVirtualFormats:
	case kAudioStreamPropertyAvailablePhysicalFormats:
		//  This returns an array of AudioStreamRangedDescriptions that describe what
		//  formats are supported.
		//  Calculate the number of items that have been requested. Note that this
		//  number is allowed to be smaller than the actual size of the list. In such
		//  case, only that number of items will be returned
		theNumberItemsToFetch = inDataSize / sizeof(AudioStreamRangedDescription);

		//  clamp it to the number of items we have
		if (theNumberItemsToFetch > 2) {
			theNumberItemsToFetch = 2;
		}

		//  fill out the return array
		if (theNumberItemsToFetch > 0) {
			((AudioStreamRangedDescription *)outData)[0].mFormat.mSampleRate = 44100.0;
			((AudioStreamRangedDescription *)outData)[0].mFormat.mFormatID = kAudioFormatLinearPCM;
			((AudioStreamRangedDescription *)outData)[0].mFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
			((AudioStreamRangedDescription *)outData)[0].mFormat.mBytesPerPacket = 8;
			((AudioStreamRangedDescription *)outData)[0].mFormat.mFramesPerPacket = 1;
			((AudioStreamRangedDescription *)outData)[0].mFormat.mBytesPerFrame = 8;
			((AudioStreamRangedDescription *)outData)[0].mFormat.mChannelsPerFrame = 2;
			((AudioStreamRangedDescription *)outData)[0].mFormat.mBitsPerChannel = 32;
			((AudioStreamRangedDescription *)outData)[0].mSampleRateRange.mMinimum = 44100.0;
			((AudioStreamRangedDescription *)outData)[0].mSampleRateRange.mMaximum = 44100.0;
		}

		if (theNumberItemsToFetch > 1) {
			((AudioStreamRangedDescription *)outData)[1].mFormat.mSampleRate = 48000.0;
			((AudioStreamRangedDescription *)outData)[1].mFormat.mFormatID = kAudioFormatLinearPCM;
			((AudioStreamRangedDescription *)outData)[1].mFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
			((AudioStreamRangedDescription *)outData)[1].mFormat.mBytesPerPacket = 8;
			((AudioStreamRangedDescription *)outData)[1].mFormat.mFramesPerPacket = 1;
			((AudioStreamRangedDescription *)outData)[1].mFormat.mBytesPerFrame = 8;
			((AudioStreamRangedDescription *)outData)[1].mFormat.mChannelsPerFrame = 2;
			((AudioStreamRangedDescription *)outData)[1].mFormat.mBitsPerChannel = 32;
			((AudioStreamRangedDescription *)outData)[1].mSampleRateRange.mMinimum = 48000.0;
			((AudioStreamRangedDescription *)outData)[1].mSampleRateRange.mMaximum = 48000.0;
		}

		//  report how much we wrote
		*outDataSize = theNumberItemsToFetch * sizeof(AudioStreamRangedDescription);
		break;

	default:
		return kAudioHardwareUnknownPropertyError;
	};

	return theAnswer;
}

static OSStatus CaptainJack_SetStreamPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, const void *inData, UInt32 *outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
	//  declare the local variables
	OSStatus theAnswer = 0;
	Float64 theOldSampleRate;
	UInt64 theNewSampleRate;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_SetStreamPropertyData: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_SetStreamPropertyData: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outNumberPropertiesChanged == NULL) {
		DebugMsg("CaptainJack_SetStreamPropertyData: no place to return the number of properties that changed");
		return kAudioHardwareIllegalOperationError;
	}

	if (outChangedAddresses == NULL) {
		DebugMsg("CaptainJack_SetStreamPropertyData: no place to return the properties that changed");
		return kAudioHardwareIllegalOperationError;
	}

	if ((inObjectID != kObjectID_Stream_Input) && (inObjectID != kObjectID_Stream_Output)) {
		DebugMsg("CaptainJack_SetStreamPropertyData: not a stream object");
		return kAudioHardwareBadObjectError;
	}

	//  initialize the returned number of changed properties
	*outNumberPropertiesChanged = 0;

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required. There is more detailed commentary about each
	//  property in the CaptainJack_GetStreamPropertyData() method.
	switch (inAddress->mSelector) {
	case kAudioStreamPropertyIsActive:

		//  Changing the active state of a stream doesn't affect IO or change the structure
		//  so we can just save the state and send the notification.
		if (inDataSize != sizeof(UInt32)) {
			DebugMsg("CaptainJack_SetStreamPropertyData: wrong size for the data for kAudioDevicePropertyNominalSampleRate");
			return kAudioHardwareBadPropertySizeError;
		}

		pthread_mutex_lock(&gPlugIn_StateMutex);

		if (inObjectID == kObjectID_Stream_Input) {
			if (gStream_Input_IsActive != (*((const UInt32 *)inData) != 0)) {
				gStream_Input_IsActive = *((const UInt32 *)inData) != 0;
				*outNumberPropertiesChanged = 1;
				outChangedAddresses[0].mSelector = kAudioStreamPropertyIsActive;
				outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
				outChangedAddresses[0].mElement = kAudioObjectPropertyElementMaster;
			}
		} else {
			if (gStream_Output_IsActive != (*((const UInt32 *)inData) != 0)) {
				gStream_Output_IsActive = *((const UInt32 *)inData) != 0;
				*outNumberPropertiesChanged = 1;
				outChangedAddresses[0].mSelector = kAudioStreamPropertyIsActive;
				outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
				outChangedAddresses[0].mElement = kAudioObjectPropertyElementMaster;
			}
		}

		pthread_mutex_unlock(&gPlugIn_StateMutex);
		break;

	case kAudioStreamPropertyVirtualFormat:
	case kAudioStreamPropertyPhysicalFormat:

		//  Changing the stream format needs to be handled via the
		//  RequestConfigChange/PerformConfigChange machinery. Note that because this
		//  device only supports 2 channel 32 bit float data, the only thing that can
		//  change is the sample rate.
		if (inDataSize != sizeof(AudioStreamBasicDescription)) {
			DebugMsg("CaptainJack_SetStreamPropertyData: wrong size for the data for kAudioStreamPropertyPhysicalFormat");
			return kAudioHardwareBadPropertySizeError;
		}

		if (((const AudioStreamBasicDescription *)inData)->mFormatID != kAudioFormatLinearPCM) {
			DebugMsg("CaptainJack_SetStreamPropertyData: unsupported format ID for kAudioStreamPropertyPhysicalFormat");
			return kAudioDeviceUnsupportedFormatError;
		}

		if (((const AudioStreamBasicDescription *)inData)->mFormatFlags != (kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked)) {
			DebugMsg("CaptainJack_SetStreamPropertyData: unsupported format flags for kAudioStreamPropertyPhysicalFormat");
			return kAudioDeviceUnsupportedFormatError;
		}

		if (((const AudioStreamBasicDescription *)inData)->mBytesPerPacket != 8) {
			DebugMsg("CaptainJack_SetStreamPropertyData: unsupported bytes per packet for kAudioStreamPropertyPhysicalFormat");
			return kAudioDeviceUnsupportedFormatError;
		}

		if (((const AudioStreamBasicDescription *)inData)->mFramesPerPacket != 1) {
			DebugMsg("CaptainJack_SetStreamPropertyData: unsupported frames per packet for kAudioStreamPropertyPhysicalFormat");
			return kAudioDeviceUnsupportedFormatError;
		}

		if (((const AudioStreamBasicDescription *)inData)->mBytesPerFrame != 8) {
			DebugMsg("CaptainJack_SetStreamPropertyData: unsupported bytes per frame for kAudioStreamPropertyPhysicalFormat");
			return kAudioDeviceUnsupportedFormatError;
		}

		if (((const AudioStreamBasicDescription *)inData)->mChannelsPerFrame != 2) {
			DebugMsg("CaptainJack_SetStreamPropertyData: unsupported channels per frame for kAudioStreamPropertyPhysicalFormat");
			return kAudioDeviceUnsupportedFormatError;
		}

		if (((const AudioStreamBasicDescription *)inData)->mBitsPerChannel != 32) {
			DebugMsg("CaptainJack_SetStreamPropertyData: unsupported bits per channel for kAudioStreamPropertyPhysicalFormat");
			return kAudioDeviceUnsupportedFormatError;
		}

		if ((((const AudioStreamBasicDescription *)inData)->mSampleRate != 44100.0) && (((const AudioStreamBasicDescription *)inData)->mSampleRate != 48000.0)) {
			DebugMsg("CaptainJack_SetStreamPropertyData: unsupported sample rate for kAudioStreamPropertyPhysicalFormat");
			return kAudioHardwareIllegalOperationError;
		}

		//  If we made it this far, the requested format is something we support, so make sure the sample rate is actually different
		pthread_mutex_lock(&gPlugIn_StateMutex);
		theOldSampleRate = gDevice_SampleRate;
		pthread_mutex_unlock(&gPlugIn_StateMutex);

		if (((const AudioStreamBasicDescription *)inData)->mSampleRate != theOldSampleRate) {
			//  we dispatch this so that the change can happen asynchronously
			theOldSampleRate = ((const AudioStreamBasicDescription *)inData)->mSampleRate;
			theNewSampleRate = (UInt64)theOldSampleRate;
			dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^ { gPlugIn_Host->RequestDeviceConfigurationChange(gPlugIn_Host, kObjectID_Device, theNewSampleRate, NULL); });
		}

		break;

	default:
		return kAudioHardwareUnknownPropertyError;
	};

	return theAnswer;
}


static Boolean  CaptainJack_HasControlProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress) {
	//  This method returns whether or not the given object has the given property.
#pragma unused(inClientProcessID)
	//  declare the local variables
	Boolean theAnswer = false;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_HasControlProperty: bad driver reference");
		return theAnswer;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_HasControlProperty: no address");
		return theAnswer;
	}

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required. There is more detailed commentary about each
	//  property in the CaptainJack_GetControlPropertyData() method.
	switch (inObjectID) {
	case kObjectID_Volume_Input_Master:
	case kObjectID_Volume_Output_Master:
		switch (inAddress->mSelector) {
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
		case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyOwnedObjects:
		case kAudioControlPropertyScope:
		case kAudioControlPropertyElement:
		case kAudioLevelControlPropertyScalarValue:
		case kAudioLevelControlPropertyDecibelValue:
		case kAudioLevelControlPropertyDecibelRange:
		case kAudioLevelControlPropertyConvertScalarToDecibels:
		case kAudioLevelControlPropertyConvertDecibelsToScalar:
			return true;
		};

		break;

	case kObjectID_Mute_Input_Master:
	case kObjectID_Mute_Output_Master:
		switch (inAddress->mSelector) {
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
		case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyOwnedObjects:
		case kAudioControlPropertyScope:
		case kAudioControlPropertyElement:
		case kAudioBooleanControlPropertyValue:
			return true;
		};

		break;

	case kObjectID_DataSource_Input_Master:
	case kObjectID_DataSource_Output_Master:
		switch (inAddress->mSelector) {
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
		case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyOwnedObjects:
		case kAudioControlPropertyScope:
		case kAudioControlPropertyElement:
		case kAudioSelectorControlPropertyCurrentItem:
		case kAudioSelectorControlPropertyAvailableItems:
		case kAudioSelectorControlPropertyItemName:
			return true;
		};

		break;
	};

	return theAnswer;
}

static OSStatus CaptainJack_IsControlPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, Boolean *outIsSettable) {
	//  This method returns whether or not the given property on the object can have its value
	//  changed.
#pragma unused(inClientProcessID)
	//  declare the local variables
	OSStatus theAnswer = 0;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_IsControlPropertySettable: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_IsControlPropertySettable: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outIsSettable == NULL) {
		DebugMsg("CaptainJack_IsControlPropertySettable: no place to put the return value");
		return kAudioHardwareIllegalOperationError;
	}

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required. There is more detailed commentary about each
	//  property in the CaptainJack_GetControlPropertyData() method.
	switch (inObjectID) {
	case kObjectID_Volume_Input_Master:
	case kObjectID_Volume_Output_Master:
		switch (inAddress->mSelector) {
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
		case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyOwnedObjects:
		case kAudioControlPropertyScope:
		case kAudioControlPropertyElement:
		case kAudioLevelControlPropertyDecibelRange:
		case kAudioLevelControlPropertyConvertScalarToDecibels:
		case kAudioLevelControlPropertyConvertDecibelsToScalar:
			*outIsSettable = false;
			break;

		case kAudioLevelControlPropertyScalarValue:
		case kAudioLevelControlPropertyDecibelValue:
			*outIsSettable = true;
			break;

		default:
			return kAudioHardwareUnknownPropertyError;
		};

		break;

	case kObjectID_Mute_Input_Master:
	case kObjectID_Mute_Output_Master:
		switch (inAddress->mSelector) {
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
		case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyOwnedObjects:
		case kAudioControlPropertyScope:
		case kAudioControlPropertyElement:
			*outIsSettable = false;
			break;

		case kAudioBooleanControlPropertyValue:
			*outIsSettable = true;
			break;

		default:
			return kAudioHardwareUnknownPropertyError;
		};

		break;

	case kObjectID_DataSource_Input_Master:
	case kObjectID_DataSource_Output_Master:
		switch (inAddress->mSelector) {
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
		case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyOwnedObjects:
		case kAudioControlPropertyScope:
		case kAudioControlPropertyElement:
		case kAudioSelectorControlPropertyAvailableItems:
		case kAudioSelectorControlPropertyItemName:
			*outIsSettable = false;
			break;

		case kAudioSelectorControlPropertyCurrentItem:
			*outIsSettable = true;
			break;

		default:
			return kAudioHardwareUnknownPropertyError;
		};

		break;

	default:
		return kAudioHardwareBadObjectError;
	};

	return theAnswer;
}

static OSStatus CaptainJack_GetControlPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 *outDataSize) {
	//  This method returns the byte size of the property's data.
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
	//  declare the local variables
	OSStatus theAnswer = 0;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_GetControlPropertyDataSize: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_GetControlPropertyDataSize: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outDataSize == NULL) {
		DebugMsg("CaptainJack_GetControlPropertyDataSize: no place to put the return value");
		return kAudioHardwareIllegalOperationError;
	}

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required. There is more detailed commentary about each
	//  property in the CaptainJack_GetControlPropertyData() method.
	switch (inObjectID) {
	case kObjectID_Volume_Input_Master:
	case kObjectID_Volume_Output_Master:
		switch (inAddress->mSelector) {
		case kAudioObjectPropertyBaseClass:
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyClass:
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyOwner:
			*outDataSize = sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyOwnedObjects:
			*outDataSize = 0 * sizeof(AudioObjectID);
			break;

		case kAudioControlPropertyScope:
			*outDataSize = sizeof(AudioObjectPropertyScope);
			break;

		case kAudioControlPropertyElement:
			*outDataSize = sizeof(AudioObjectPropertyElement);
			break;

		case kAudioLevelControlPropertyScalarValue:
			*outDataSize = sizeof(Float32);
			break;

		case kAudioLevelControlPropertyDecibelValue:
			*outDataSize = sizeof(Float32);
			break;

		case kAudioLevelControlPropertyDecibelRange:
			*outDataSize = sizeof(AudioValueRange);
			break;

		case kAudioLevelControlPropertyConvertScalarToDecibels:
			*outDataSize = sizeof(Float32);
			break;

		case kAudioLevelControlPropertyConvertDecibelsToScalar:
			*outDataSize = sizeof(Float32);
			break;

		default:
			return kAudioHardwareUnknownPropertyError;
		};

		break;

	case kObjectID_Mute_Input_Master:
	case kObjectID_Mute_Output_Master:
		switch (inAddress->mSelector) {
		case kAudioObjectPropertyBaseClass:
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyClass:
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyOwner:
			*outDataSize = sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyOwnedObjects:
			*outDataSize = 0 * sizeof(AudioObjectID);
			break;

		case kAudioControlPropertyScope:
			*outDataSize = sizeof(AudioObjectPropertyScope);
			break;

		case kAudioControlPropertyElement:
			*outDataSize = sizeof(AudioObjectPropertyElement);
			break;

		case kAudioBooleanControlPropertyValue:
			*outDataSize = sizeof(UInt32);
			break;

		default:
			return kAudioHardwareUnknownPropertyError;
		};

		break;

	case kObjectID_DataSource_Input_Master:
	case kObjectID_DataSource_Output_Master:
		switch (inAddress->mSelector) {
		case kAudioObjectPropertyBaseClass:
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyClass:
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyOwner:
			*outDataSize = sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyOwnedObjects:
			*outDataSize = 0 * sizeof(AudioObjectID);
			break;

		case kAudioControlPropertyScope:
			*outDataSize = sizeof(AudioObjectPropertyScope);
			break;

		case kAudioControlPropertyElement:
			*outDataSize = sizeof(AudioObjectPropertyElement);
			break;

		case kAudioSelectorControlPropertyCurrentItem:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioSelectorControlPropertyAvailableItems:
			*outDataSize = 0;
			break;

		case kAudioSelectorControlPropertyItemName:
			*outDataSize = sizeof(CFStringRef);
			break;

		default:
			return kAudioHardwareUnknownPropertyError;
		};

		break;

	default:
		return kAudioHardwareBadObjectError;
	};

	return theAnswer;
}

static OSStatus CaptainJack_GetControlPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
	//  declare the local variables
	OSStatus theAnswer = 0;
	UInt32 theNumberItemsToFetch;
	UInt32 theItemIndex;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_GetControlPropertyData: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_GetControlPropertyData: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outDataSize == NULL) {
		DebugMsg("CaptainJack_GetControlPropertyData: no place to put the return value size");
		return kAudioHardwareIllegalOperationError;
	}

	if (outData == NULL) {
		DebugMsg("CaptainJack_GetControlPropertyData: no place to put the return value");
		return kAudioHardwareIllegalOperationError;
	}

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required.
	//
	//  Also, since most of the data that will get returned is static, there are few instances where
	//  it is necessary to lock the state mutex.
	switch (inObjectID) {
	case kObjectID_Volume_Input_Master:
	case kObjectID_Volume_Output_Master:
		switch (inAddress->mSelector) {
		case kAudioObjectPropertyBaseClass:

			//  The base class for kAudioVolumeControlClassID is kAudioLevelControlClassID
			if (inDataSize < sizeof(AudioClassID)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the volume control");
				return kAudioHardwareBadPropertySizeError;
			}

			*((AudioClassID *)outData) = kAudioLevelControlClassID;
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyClass:

			//  Volume controls are of the class, kAudioVolumeControlClassID
			if (inDataSize < sizeof(AudioClassID)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioObjectPropertyClass for the volume control");
				return kAudioHardwareBadPropertySizeError;
			}

			*((AudioClassID *)outData) = kAudioVolumeControlClassID;
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyOwner:

			//  The control's owner is the device object
			if (inDataSize < sizeof(AudioObjectID)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the volume control");
				return kAudioHardwareBadPropertySizeError;
			}

			*((AudioObjectID *)outData) = kObjectID_Device;
			*outDataSize = sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyOwnedObjects:
			//  Controls do not own any objects
			*outDataSize = 0 * sizeof(AudioObjectID);
			break;

		case kAudioControlPropertyScope:

			//  This property returns the scope that the control is attached to.
			if (inDataSize < sizeof(AudioObjectPropertyScope)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioControlPropertyScope for the volume control");
				return kAudioHardwareBadPropertySizeError;
			}

			*((AudioObjectPropertyScope *)outData) = (inObjectID == kObjectID_Volume_Input_Master) ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
			*outDataSize = sizeof(AudioObjectPropertyScope);
			break;

		case kAudioControlPropertyElement:

			//  This property returns the element that the control is attached to.
			if (inDataSize < sizeof(AudioObjectPropertyElement)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioControlPropertyElement for the volume control");
				return kAudioHardwareBadPropertySizeError;
			}

			*((AudioObjectPropertyElement *)outData) = kAudioObjectPropertyElementMaster;
			*outDataSize = sizeof(AudioObjectPropertyElement);
			break;

		case kAudioLevelControlPropertyScalarValue:

			//  This returns the value of the control in the normalized range of 0 to 1.
			//  Note that we need to take the state lock to examine the value.
			if (inDataSize < sizeof(Float32)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioLevelControlPropertyScalarValue for the volume control");
				return kAudioHardwareBadPropertySizeError;
			}

			pthread_mutex_lock(&gPlugIn_StateMutex);
			*((Float32 *)outData) = (inObjectID == kObjectID_Volume_Input_Master) ? gVolume_Input_Master_Value : gVolume_Output_Master_Value;
			pthread_mutex_unlock(&gPlugIn_StateMutex);
			*outDataSize = sizeof(Float32);
			break;

		case kAudioLevelControlPropertyDecibelValue:

			//  This returns the dB value of the control.
			//  Note that we need to take the state lock to examine the value.
			if (inDataSize < sizeof(Float32)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioLevelControlPropertyDecibelValue for the volume control");
				return kAudioHardwareBadPropertySizeError;
			}

			pthread_mutex_lock(&gPlugIn_StateMutex);
			*((Float32 *)outData) = (inObjectID == kObjectID_Volume_Input_Master) ? gVolume_Input_Master_Value : gVolume_Output_Master_Value;
			pthread_mutex_unlock(&gPlugIn_StateMutex);
			//  Note that we square the scalar value before converting to dB so as to
			//  provide a better curve for the slider
			*((Float32 *)outData) *= *((Float32 *)outData);
			*((Float32 *)outData) = kVolume_MinDB + (*((Float32 *)outData) * (kVolume_MaxDB - kVolume_MinDB));
			//  report how much we wrote
			*outDataSize = sizeof(Float32);
			break;

		case kAudioLevelControlPropertyDecibelRange:

			//  This returns the dB range of the control.
			if (inDataSize < sizeof(AudioValueRange)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioLevelControlPropertyDecibelRange for the volume control");
				return kAudioHardwareBadPropertySizeError;
			}

			((AudioValueRange *)outData)->mMinimum = kVolume_MinDB;
			((AudioValueRange *)outData)->mMaximum = kVolume_MaxDB;
			*outDataSize = sizeof(AudioValueRange);
			break;

		case kAudioLevelControlPropertyConvertScalarToDecibels:

			//  This takes the scalar value in outData and converts it to dB.
			if (inDataSize < sizeof(Float32)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioLevelControlPropertyDecibelValue for the volume control");
				return kAudioHardwareBadPropertySizeError;
			}

			//  clamp the value to be between 0 and 1
			if (*((Float32 *)outData) < 0.0) {
				*((Float32 *)outData) = 0;
			}

			if (*((Float32 *)outData) > 1.0) {
				*((Float32 *)outData) = 1.0;
			}

			//  Note that we square the scalar value before converting to dB so as to
			//  provide a better curve for the slider
			*((Float32 *)outData) *= *((Float32 *)outData);
			*((Float32 *)outData) = kVolume_MinDB + (*((Float32 *)outData) * (kVolume_MaxDB - kVolume_MinDB));
			//  report how much we wrote
			*outDataSize = sizeof(Float32);
			break;

		case kAudioLevelControlPropertyConvertDecibelsToScalar:

			//  This takes the dB value in outData and converts it to scalar.
			if (inDataSize < sizeof(Float32)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioLevelControlPropertyDecibelValue for the volume control");
				return kAudioHardwareBadPropertySizeError;
			}

			//  clamp the value to be between kVolume_MinDB and kVolume_MaxDB
			if (*((Float32 *)outData) < kVolume_MinDB) {
				*((Float32 *)outData) = kVolume_MinDB;
			}

			if (*((Float32 *)outData) > kVolume_MaxDB) {
				*((Float32 *)outData) = kVolume_MaxDB;
			}

			//  Note that we square the scalar value before converting to dB so as to
			//  provide a better curve for the slider. We undo that here.
			*((Float32 *)outData) = *((Float32 *)outData) - kVolume_MinDB;
			*((Float32 *)outData) /= kVolume_MaxDB - kVolume_MinDB;
			*((Float32 *)outData) = sqrtf(*((Float32 *)outData));
			//  report how much we wrote
			*outDataSize = sizeof(Float32);
			break;

		default:
			return kAudioHardwareUnknownPropertyError;
		};

		break;

	case kObjectID_Mute_Input_Master:
	case kObjectID_Mute_Output_Master:
		switch (inAddress->mSelector) {
		case kAudioObjectPropertyBaseClass:

			//  The base class for kAudioMuteControlClassID is kAudioBooleanControlClassID
			if (inDataSize < sizeof(AudioClassID)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the mute control");
				return kAudioHardwareBadPropertySizeError;
			}

			*((AudioClassID *)outData) = kAudioBooleanControlClassID;
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyClass:

			//  Mute controls are of the class, kAudioMuteControlClassID
			if (inDataSize < sizeof(AudioClassID)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioObjectPropertyClass for the mute control");
				return kAudioHardwareBadPropertySizeError;
			}

			*((AudioClassID *)outData) = kAudioMuteControlClassID;
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyOwner:

			//  The control's owner is the device object
			if (inDataSize < sizeof(AudioObjectID)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the mute control");
				return kAudioHardwareBadPropertySizeError;
			}

			*((AudioObjectID *)outData) = kObjectID_Device;
			*outDataSize = sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyOwnedObjects:
			//  Controls do not own any objects
			*outDataSize = 0 * sizeof(AudioObjectID);
			break;

		case kAudioControlPropertyScope:

			//  This property returns the scope that the control is attached to.
			if (inDataSize < sizeof(AudioObjectPropertyScope)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioControlPropertyScope for the mute control");
				return kAudioHardwareBadPropertySizeError;
			}

			*((AudioObjectPropertyScope *)outData) = (inObjectID == kObjectID_Mute_Input_Master) ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
			*outDataSize = sizeof(AudioObjectPropertyScope);
			break;

		case kAudioControlPropertyElement:

			//  This property returns the element that the control is attached to.
			if (inDataSize < sizeof(AudioObjectPropertyElement)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioControlPropertyElement for the mute control");
				return kAudioHardwareBadPropertySizeError;
			}

			*((AudioObjectPropertyElement *)outData) = kAudioObjectPropertyElementMaster;
			*outDataSize = sizeof(AudioObjectPropertyElement);
			break;

		case kAudioBooleanControlPropertyValue:

			//  This returns the value of the mute control where 0 means that mute is off
			//  and audio can be heard and 1 means that mute is on and audio cannot be heard.
			//  Note that we need to take the state lock to examine this value.
			if (inDataSize < sizeof(UInt32)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioBooleanControlPropertyValue for the mute control");
				return kAudioHardwareBadPropertySizeError;
			}

			pthread_mutex_lock(&gPlugIn_StateMutex);
			*((UInt32 *)outData) = (inObjectID == kObjectID_Mute_Input_Master) ? (gMute_Input_Master_Value ? 1 : 0) : (gMute_Output_Master_Value ? 1 : 0);
			pthread_mutex_unlock(&gPlugIn_StateMutex);
			*outDataSize = sizeof(UInt32);
			break;

		default:
			return kAudioHardwareUnknownPropertyError;
		};

		break;

	case kObjectID_DataSource_Input_Master:
	case kObjectID_DataSource_Output_Master:
		switch (inAddress->mSelector) {
		case kAudioObjectPropertyBaseClass:

			//  The base class for kAudioDataSourceControlClassID is kAudioSelectorControlClassID
			if (inDataSize < sizeof(AudioClassID)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the data source control");
				return kAudioHardwareBadPropertySizeError;
			}

			*((AudioClassID *)outData) = kAudioSelectorControlClassID;
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyClass:

			//  Data Source controls are of the class, kAudioDataSourceControlClassID
			if (inDataSize < sizeof(AudioClassID)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioObjectPropertyClass for the data source control");
				return kAudioHardwareBadPropertySizeError;
			}

			*((AudioClassID *)outData) = kAudioDataSourceControlClassID;
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyOwner:

			//  The control's owner is the device object
			if (inDataSize < sizeof(AudioObjectID)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the data source control");
				return kAudioHardwareBadPropertySizeError;
			}

			*((AudioObjectID *)outData) = kObjectID_Device;
			*outDataSize = sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyOwnedObjects:
			//  Controls do not own any objects
			*outDataSize = 0 * sizeof(AudioObjectID);
			break;

		case kAudioControlPropertyScope:

			//  This property returns the scope that the control is attached to.
			if (inDataSize < sizeof(AudioObjectPropertyScope)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioControlPropertyScope for the data source control");
				return kAudioHardwareBadPropertySizeError;
			}

			*((AudioObjectPropertyScope *)outData) = (inObjectID == kObjectID_DataSource_Input_Master) ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
			*outDataSize = sizeof(AudioObjectPropertyScope);
			break;

		case kAudioControlPropertyElement:

			//  This property returns the element that the control is attached to.
			if (inDataSize < sizeof(AudioObjectPropertyElement)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioControlPropertyElement for the data source control");
				return kAudioHardwareBadPropertySizeError;
			}

			*((AudioObjectPropertyElement *)outData) = kAudioObjectPropertyElementMaster;
			*outDataSize = sizeof(AudioObjectPropertyElement);
			break;

		case kAudioSelectorControlPropertyCurrentItem:

			//  This returns the value of the data source selector.
			//  Note that we need to take the state lock to examine this value.
			if (inDataSize < sizeof(UInt32)) {
				DebugMsg("CaptainJack_GetControlPropertyData: not enough space for the return value of kAudioSelectorControlPropertyCurrentItem for the data source control");
				return kAudioHardwareBadPropertySizeError;
			}

			pthread_mutex_lock(&gPlugIn_StateMutex);
			*((UInt32 *)outData) = (inObjectID == kObjectID_DataSource_Input_Master) ? gDataSource_Input_Master_Value : gDataSource_Output_Master_Value;
			pthread_mutex_unlock(&gPlugIn_StateMutex);
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioSelectorControlPropertyAvailableItems:
			//  This returns the IDs for all the items the data source control supports.
			//  Calculate the number of items that have been requested. Note that this
			//  number is allowed to be smaller than the actual size of the list. In such
			//  case, only that number of items will be returned
			theNumberItemsToFetch = inDataSize / sizeof(UInt32);

			//  fill out the return array
			for (theItemIndex = 0; theItemIndex < theNumberItemsToFetch; ++theItemIndex) {
				((UInt32 *)outData)[theItemIndex] = theItemIndex;
			}

			//  report how much we wrote
			*outDataSize = theNumberItemsToFetch * sizeof(UInt32);
			break;

		case kAudioSelectorControlPropertyItemName:

			//  This returns the user-readable name for the selector item in the qualifier
			if (true) {
				DebugMsg("CaptainJack_GetControlPropertyData: the item in the qualifier is not valid for kAudioSelectorControlPropertyItemName for the data source control");
				return kAudioHardwareIllegalOperationError;
			}

			break;

		default:
			return kAudioHardwareUnknownPropertyError;
		};

		break;

	default:
		return kAudioHardwareBadObjectError;
	};

	return theAnswer;
}

static OSStatus CaptainJack_SetControlPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, const void *inData, UInt32 *outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
	OSStatus theAnswer = 0;
	Float32 theNewVolume;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_SetControlPropertyData: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inAddress == NULL) {
		DebugMsg("CaptainJack_SetControlPropertyData: no address");
		return kAudioHardwareIllegalOperationError;
	}

	if (outNumberPropertiesChanged == NULL) {
		DebugMsg("CaptainJack_SetControlPropertyData: no place to return the number of properties that changed");
		return kAudioHardwareIllegalOperationError;
	}

	if (outChangedAddresses == NULL) {
		DebugMsg("CaptainJack_SetControlPropertyData: no place to return the properties that changed");
		return kAudioHardwareIllegalOperationError;
	}

	//  initialize the returned number of changed properties
	*outNumberPropertiesChanged = 0;

	//  Note that for each object, this driver implements all the required properties plus a few
	//  extras that are useful but not required. There is more detailed commentary about each
	//  property in the CaptainJack_GetControlPropertyData() method.
	switch (inObjectID) {
	case kObjectID_Volume_Input_Master:
	case kObjectID_Volume_Output_Master:
		switch (inAddress->mSelector) {
		case kAudioLevelControlPropertyScalarValue:

			//  For the scalar volume, we clamp the new value to [0, 1]. Note that if this
			//  value changes, it implies that the dB value changed too.
			if (inDataSize != sizeof(Float32)) {
				DebugMsg("CaptainJack_SetControlPropertyData: wrong size for the data for kAudioLevelControlPropertyScalarValue");
				return kAudioHardwareBadPropertySizeError;
			}

			theNewVolume = *((const Float32 *)inData);

			if (theNewVolume < 0.0) {
				theNewVolume = 0.0;
			} else if (theNewVolume > 1.0) {
				theNewVolume = 1.0;
			}

			pthread_mutex_lock(&gPlugIn_StateMutex);

			if (inObjectID == kObjectID_Volume_Input_Master) {
				if (gVolume_Input_Master_Value != theNewVolume) {
					gVolume_Input_Master_Value = theNewVolume;
					*outNumberPropertiesChanged = 2;
					outChangedAddresses[0].mSelector = kAudioLevelControlPropertyScalarValue;
					outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
					outChangedAddresses[0].mElement = kAudioObjectPropertyElementMaster;
					outChangedAddresses[1].mSelector = kAudioLevelControlPropertyDecibelValue;
					outChangedAddresses[1].mScope = kAudioObjectPropertyScopeGlobal;
					outChangedAddresses[1].mElement = kAudioObjectPropertyElementMaster;
				}
			} else {
				if (gVolume_Output_Master_Value != theNewVolume) {
					gVolume_Output_Master_Value = theNewVolume;
					*outNumberPropertiesChanged = 2;
					outChangedAddresses[0].mSelector = kAudioLevelControlPropertyScalarValue;
					outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
					outChangedAddresses[0].mElement = kAudioObjectPropertyElementMaster;
					outChangedAddresses[1].mSelector = kAudioLevelControlPropertyDecibelValue;
					outChangedAddresses[1].mScope = kAudioObjectPropertyScopeGlobal;
					outChangedAddresses[1].mElement = kAudioObjectPropertyElementMaster;
				}
			}

			pthread_mutex_unlock(&gPlugIn_StateMutex);
			break;

		case kAudioLevelControlPropertyDecibelValue:

			//  For the dB value, we first convert it to a scalar value since that is how
			//  the value is tracked. Note that if this value changes, it implies that the
			//  scalar value changes as well.
			if (inDataSize != sizeof(Float32)) {
				DebugMsg("CaptainJack_SetControlPropertyData: wrong size for the data for kAudioLevelControlPropertyScalarValue");
				return kAudioHardwareBadPropertySizeError;
			}

			theNewVolume = *((const Float32 *)inData);

			if (theNewVolume < kVolume_MinDB) {
				theNewVolume = kVolume_MinDB;
			} else if (theNewVolume > kVolume_MaxDB) {
				theNewVolume = kVolume_MaxDB;
			}

			//  Note that we square the scalar value before converting to dB so as to
			//  provide a better curve for the slider. We undo that here.
			theNewVolume = theNewVolume - kVolume_MinDB;
			theNewVolume /= kVolume_MaxDB - kVolume_MinDB;
			theNewVolume = sqrtf(theNewVolume);
			pthread_mutex_lock(&gPlugIn_StateMutex);

			if (inObjectID == kObjectID_Volume_Input_Master) {
				if (gVolume_Input_Master_Value != theNewVolume) {
					gVolume_Input_Master_Value = theNewVolume;
					*outNumberPropertiesChanged = 2;
					outChangedAddresses[0].mSelector = kAudioLevelControlPropertyScalarValue;
					outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
					outChangedAddresses[0].mElement = kAudioObjectPropertyElementMaster;
					outChangedAddresses[1].mSelector = kAudioLevelControlPropertyDecibelValue;
					outChangedAddresses[1].mScope = kAudioObjectPropertyScopeGlobal;
					outChangedAddresses[1].mElement = kAudioObjectPropertyElementMaster;
				}
			} else {
				if (gVolume_Output_Master_Value != theNewVolume) {
					gVolume_Output_Master_Value = theNewVolume;
					*outNumberPropertiesChanged = 2;
					outChangedAddresses[0].mSelector = kAudioLevelControlPropertyScalarValue;
					outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
					outChangedAddresses[0].mElement = kAudioObjectPropertyElementMaster;
					outChangedAddresses[1].mSelector = kAudioLevelControlPropertyDecibelValue;
					outChangedAddresses[1].mScope = kAudioObjectPropertyScopeGlobal;
					outChangedAddresses[1].mElement = kAudioObjectPropertyElementMaster;
				}
			}

			pthread_mutex_unlock(&gPlugIn_StateMutex);
			break;

		default:
			return kAudioHardwareUnknownPropertyError;
		};

		break;

	case kObjectID_Mute_Input_Master:
	case kObjectID_Mute_Output_Master:
		switch (inAddress->mSelector) {
		case kAudioBooleanControlPropertyValue:
			if (inDataSize != sizeof(UInt32)) {
				DebugMsg("CaptainJack_SetControlPropertyData: wrong size for the data for kAudioBooleanControlPropertyValue");
				return kAudioHardwareBadPropertySizeError;
			}

			pthread_mutex_lock(&gPlugIn_StateMutex);

			if (inObjectID == kObjectID_Mute_Input_Master) {
				if (gMute_Input_Master_Value != (*((const UInt32 *)inData) != 0)) {
					gMute_Input_Master_Value = *((const UInt32 *)inData) != 0;
					*outNumberPropertiesChanged = 1;
					outChangedAddresses[0].mSelector = kAudioBooleanControlPropertyValue;
					outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
					outChangedAddresses[0].mElement = kAudioObjectPropertyElementMaster;
				}
			} else {
				if (gMute_Output_Master_Value != (*((const UInt32 *)inData) != 0)) {
					gMute_Output_Master_Value = *((const UInt32 *)inData) != 0;
					*outNumberPropertiesChanged = 1;
					outChangedAddresses[0].mSelector = kAudioBooleanControlPropertyValue;
					outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
					outChangedAddresses[0].mElement = kAudioObjectPropertyElementMaster;
				}
			}

			pthread_mutex_unlock(&gPlugIn_StateMutex);
			break;

		default:
			return kAudioHardwareUnknownPropertyError;
		};

		break;

	case kObjectID_DataSource_Input_Master:
	case kObjectID_DataSource_Output_Master:
		switch (inAddress->mSelector) {
		case kAudioSelectorControlPropertyCurrentItem:

			//  For selector controls, we check to make sure the requested value is in the
			//  available items list and just store the value.
			if (inDataSize != sizeof(UInt32)) {
				DebugMsg("CaptainJack_SetControlPropertyData: wrong size for the data for kAudioSelectorControlPropertyCurrentItem");
				return kAudioHardwareBadPropertySizeError;
			}

			if (true) {
				DebugMsg("CaptainJack_SetControlPropertyData: requested item not in available items list for kAudioSelectorControlPropertyCurrentItem");
				return kAudioHardwareIllegalOperationError;
			}

			pthread_mutex_lock(&gPlugIn_StateMutex);

			if (inObjectID == kObjectID_DataSource_Input_Master) {
				if (gDataSource_Input_Master_Value != *((const UInt32 *)inData)) {
					gDataSource_Input_Master_Value = *((const UInt32 *)inData);
					*outNumberPropertiesChanged = 1;
					outChangedAddresses[0].mSelector = kAudioSelectorControlPropertyCurrentItem;
					outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
					outChangedAddresses[0].mElement = kAudioObjectPropertyElementMaster;
				}
			} else {
				if (gDataSource_Output_Master_Value != *((const UInt32 *)inData)) {
					gDataSource_Output_Master_Value = *((const UInt32 *)inData);
					*outNumberPropertiesChanged = 1;
					outChangedAddresses[0].mSelector = kAudioSelectorControlPropertyCurrentItem;
					outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
					outChangedAddresses[0].mElement = kAudioObjectPropertyElementMaster;
				}
			}

			pthread_mutex_unlock(&gPlugIn_StateMutex);
			break;

		default:
			return kAudioHardwareUnknownPropertyError;
		};

		break;

	default:
		return kAudioHardwareBadObjectError;
	};

	return theAnswer;
}


static OSStatus CaptainJack_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID) {
#pragma unused(inClientID)
	//  This call tells the device that IO is starting for the given client. When this routine
	//  returns, the device's clock is running and it is ready to have data read/written. It is
	//  important to note that multiple clients can have IO running on the device at the same time.
	//  So, work only needs to be done when the first client starts. All subsequent starts simply
	//  declare the local variables
	OSStatus theAnswer = 0;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_StartIO: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inDeviceObjectID != kObjectID_Device) {
		DebugMsg("CaptainJack_StartIO: bad device ID");
		return kAudioHardwareBadObjectError;
	}

	//  we need to hold the state lock
	pthread_mutex_lock(&gPlugIn_StateMutex);

	//  figure out what we need to do
	if (gDevice_IOIsRunning == UINT64_MAX) {
		//  overflowing is an error
		theAnswer = kAudioHardwareIllegalOperationError;
	} else if (gDevice_IOIsRunning == 0) {
		//  We need to start the hardware, which in this case is just anchoring the time line.
		gDevice_IOIsRunning = 1;
		gDevice_NumberTimeStamps = 0;
		gDevice_AnchorSampleTime = 0;
		gDevice_AnchorHostTime = mach_absolute_time();
	} else {
		//  IO is already running, so just bump the counter
		++gDevice_IOIsRunning;
	}

	//  unlock the state lock
	pthread_mutex_unlock(&gPlugIn_StateMutex);
	return theAnswer;
}

static OSStatus CaptainJack_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID) {
	//  This call tells the device that the client has stopped IO. The driver can stop the hardware
	//  once all clients have stopped.
#pragma unused(inClientID)
	//  declare the local variables
	OSStatus theAnswer = 0;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_StopIO: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inDeviceObjectID != kObjectID_Device) {
		DebugMsg("CaptainJack_StopIO: bad device ID");
		return kAudioHardwareBadObjectError;
	}

	//  we need to hold the state lock
	pthread_mutex_lock(&gPlugIn_StateMutex);

	//  figure out what we need to do
	if (gDevice_IOIsRunning == 0) {
		//  underflowing is an error
		theAnswer = kAudioHardwareIllegalOperationError;
	} else if (gDevice_IOIsRunning == 1) {
		//  We need to stop the hardware, which in this case means that there's nothing to do.
		gDevice_IOIsRunning = 0;
	} else {
		//  IO is still running, so just bump the counter
		--gDevice_IOIsRunning;
	}

	//  unlock the state lock
	pthread_mutex_unlock(&gPlugIn_StateMutex);
	return theAnswer;
}

static OSStatus CaptainJack_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed) {
	//  This method returns the current zero time stamp for the device. The HAL models the timing of
	//  a device as a series of time stamps that relate the sample time to a host time. The zero
	//  time stamps are spaced such that the sample times are the value of
	//  kAudioDevicePropertyZeroTimeStampPeriod apart. This is often modeled using a ring buffer
	//  where the zero time stamp is updated when wrapping around the ring buffer.
	//
	//  For this device, the zero time stamps' sample time increments every kDevice_RingBufferSize
	//  frames and the host time increments by kDevice_RingBufferSize * gDevice_HostTicksPerFrame.
#pragma unused(inClientID)
	//  declare the local variables
	OSStatus theAnswer = 0;
	UInt64 theCurrentHostTime;
	Float64 theHostTicksPerRingBuffer;
	Float64 theHostTickOffset;
	UInt64 theNextHostTime;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_GetZeroTimeStamp: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inDeviceObjectID != kObjectID_Device) {
		DebugMsg("CaptainJack_GetZeroTimeStamp: bad device ID");
		return kAudioHardwareBadObjectError;
	}

	//  we need to hold the locks
	pthread_mutex_lock(&gDevice_IOMutex);
	//  get the current host time
	theCurrentHostTime = mach_absolute_time();
	//  calculate the next host time
	theHostTicksPerRingBuffer = gDevice_HostTicksPerFrame * ((Float64)kDevice_RingBufferSize);
	theHostTickOffset = ((Float64)(gDevice_NumberTimeStamps + 1)) * theHostTicksPerRingBuffer;
	theNextHostTime = gDevice_AnchorHostTime + ((UInt64)theHostTickOffset);

	//  go to the next time if the next host time is less than the current time
	if (theNextHostTime <= theCurrentHostTime) {
		++gDevice_NumberTimeStamps;
	}

	//  set the return values
	*outSampleTime = gDevice_NumberTimeStamps * kDevice_RingBufferSize;
	*outHostTime = gDevice_AnchorHostTime + (((Float64)gDevice_NumberTimeStamps) * theHostTicksPerRingBuffer);
	*outSeed = 1;
	//  unlock the state lock
	pthread_mutex_unlock(&gDevice_IOMutex);
	return theAnswer;
}

static OSStatus CaptainJack_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean *outWillDo, Boolean *outWillDoInPlace) {
	//  This method returns whether or not the device will do a given IO operation. For this device,
	//  we only support reading input data and writing output data.
#pragma unused(inClientID)
	//  declare the local variables
	OSStatus theAnswer = 0;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_WillDoIOOperation: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inDeviceObjectID != kObjectID_Device) {
		DebugMsg("CaptainJack_WillDoIOOperation: bad device ID");
		return kAudioHardwareBadObjectError;
	}

	//  figure out if we support the operation
	bool willDo = false;
	bool willDoInPlace = true;

	switch (inOperationID) {
	case kAudioServerPlugInIOOperationReadInput:
		willDo = true;
		willDoInPlace = true;
		break;

	case kAudioServerPlugInIOOperationWriteMix:
		willDo = true;
		willDoInPlace = true;
		break;
	};

	//  fill out the return values
	if (outWillDo != NULL) {
		*outWillDo = willDo;
	}

	if (outWillDoInPlace != NULL) {
		*outWillDoInPlace = willDoInPlace;
	}

	return theAnswer;
}

static OSStatus CaptainJack_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo) {
	//  This is called at the beginning of an IO operation. This device doesn't do anything, so just
	//  check the arguments and return.
#pragma unused(inClientID, inOperationID, inIOBufferFrameSize, inIOCycleInfo)
	//  declare the local variables
	OSStatus theAnswer = 0;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_BeginIOOperation: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inDeviceObjectID != kObjectID_Device) {
		DebugMsg("CaptainJack_BeginIOOperation: bad device ID");
		return kAudioHardwareBadObjectError;
	}

	return theAnswer;
}

static OSStatus CaptainJack_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo, void *ioMainBuffer, void *ioSecondaryBuffer) {
	//  This is called to actuall perform a given operation. For this device, all we need to do is
	//  clear the buffer for the ReadInput operation.
#pragma unused(inClientID, inIOCycleInfo, ioSecondaryBuffer)
	//  declare the local variables
	OSStatus theAnswer = 0;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_DoIOOperation: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inDeviceObjectID != kObjectID_Device) {
		DebugMsg("CaptainJack_DoIOOperation: bad device ID");
		return kAudioHardwareBadObjectError;
	}

	if ((inStreamObjectID != kObjectID_Stream_Input) && (inStreamObjectID != kObjectID_Stream_Output)) {
		DebugMsg("CaptainJack_DoIOOperation: bad stream ID");
		return kAudioHardwareBadObjectError;
	}

	//  clear the buffer if this iskAudioServerPlugInIOOperationReadInput
	if (inOperationID == kAudioServerPlugInIOOperationReadInput) {
		//  we are always dealing with a 2 channel 32 bit float buffer
		memset(ioMainBuffer, 0, inIOBufferFrameSize * 8);
	}

	return theAnswer;
}

static OSStatus CaptainJack_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo) {
	//  This is called at the end of an IO operation. This device doesn't do anything, so just check
	//  the arguments and return.
#pragma unused(inClientID, inOperationID, inIOBufferFrameSize, inIOCycleInfo)
	//  declare the local variables
	OSStatus theAnswer = 0;

	//  check the arguments
	if (inDriver != gAudioServerPlugInDriverRef) {
		DebugMsg("CaptainJack_EndIOOperation: bad driver reference");
		return kAudioHardwareBadObjectError;
	}

	if (inDeviceObjectID != kObjectID_Device) {
		DebugMsg("CaptainJack_EndIOOperation: bad device ID");
		return kAudioHardwareBadObjectError;
	}

	return theAnswer;
}
