#pragma once

#include <cstdint>

#ifndef F4SE_TYPES_UINT32_DEFINED
#define F4SE_TYPES_UINT32_DEFINED
using UInt32 = std::uint32_t;
#endif
using PluginHandle = UInt32;

enum : PluginHandle {
	kPluginHandle_Invalid = 0xFFFFFFFF
};

enum {
	kInterface_Invalid = 0,
	kInterface_Messaging,
	kInterface_Scaleform,
	kInterface_Papyrus,
	kInterface_Serialization,
	kInterface_Task,
	kInterface_Object,
	kInterface_Trampoline,
	kInterface_Max
};

struct PluginInfo {
	enum { kInfoVersion = 1 };
	UInt32 infoVersion;
	const char* name;
	UInt32 version;
};

struct F4SEInterface {
	UInt32 f4seVersion;
	UInt32 runtimeVersion;
	UInt32 editorVersion;
	UInt32 isEditor;
	void* (*QueryInterface)(UInt32 id);
	PluginHandle (*GetPluginHandle)();
	UInt32 (*GetReleaseIndex)();
	const PluginInfo* (*GetPluginInfo)(const char* name);
	const char* (*GetSaveFolderName)();
};

struct F4SEMessagingInterface {
	struct Message {
		const char* sender;
		UInt32 type;
		UInt32 dataLen;
		void* data;
	};

	using EventCallback = void (*)(Message* msg);

	enum { kInterfaceVersion = 1 };

	enum {
		kMessage_PostLoad,
		kMessage_PostPostLoad,
		kMessage_PreLoadGame,
		kMessage_PostLoadGame,
		kMessage_PreSaveGame,
		kMessage_PostSaveGame,
		kMessage_DeleteGame,
		kMessage_InputLoaded,
		kMessage_NewGame,
		kMessage_DataLoaded,
		kMessage_Max
	};

	const char* (*GetSender)(PluginHandle pluginHandle);
	bool (*RegisterListener)(PluginHandle listener, const char* sender, EventCallback callback);
	bool (*Dispatch)(PluginHandle sender, UInt32 messageType, void* data, UInt32 dataLen, const char* receiver);
};
