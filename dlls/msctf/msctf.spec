@ stdcall -private DllCanUnloadNow()
@ stdcall -private DllGetClassObject(ptr ptr ptr)
@ stdcall -private DllRegisterServer()
@ stdcall -private DllUnregisterServer()
@ stdcall SetInputScope(long long)
@ stub SetInputScopeXML
@ stdcall SetInputScopes(long ptr long ptr long wstr wstr)
@ stub TF_CUASAppFix
@ stub TF_CheckThreadInputIdle
@ stub TF_ClearLangBarAddIns
@ stub TF_CreateCategoryMgr
@ stub TF_CreateCicLoadMutex
@ stub TF_CreateDisplayAttributeMgr
@ stdcall TF_CreateInputProcessorProfiles(ptr)
@ stdcall TF_CreateLangBarItemMgr(ptr)
@ stdcall TF_CreateLangBarMgr(ptr)
@ stdcall TF_CreateThreadMgr(ptr)
@ stub TF_DllDetachInOther
@ stub TF_GetGlobalCompartment
@ stub TF_GetInputScope
@ stub TF_GetLangIcon
@ stub TF_GetMlngHKL
@ stub TF_GetMlngIconIndex
@ stub TF_GetThreadFlags
@ stdcall TF_GetThreadMgr(ptr)
@ stdcall -private TF_WineHostIMEApply(ptr long)
@ stdcall -private TF_WineHostIMEProcessKey(ptr long long int64 int64 ptr)
@ stdcall -private TF_WineHostIMEQuery(ptr int64 long ptr)
@ stdcall -private TF_WineHostIMEReset(ptr int64 long)
@ stub TF_InatExtractIcon
@ stdcall TF_InitMlngInfo()
@ stub TF_InitSystem
@ stub TF_InvalidAssemblyListCache
@ stdcall TF_InvalidAssemblyListCacheIfExist()
@ stub TF_IsCtfmonRunning
@ stub TF_IsInMarshaling
@ stub TF_MlngInfoCount
@ stub TF_RunInputCPL
