#include <switch.h>
#include <string.h>
#include <stdio.h>

#define MODULE_HBL 347

const char* g_easterEgg = "Do you mean to tell me that you're thinking seriously of building that way, when and if you are an architect?";

static char g_argv[2048];
static char g_nextArgv[2048];
static char g_nextNroPath[512];
static u64  g_nroAddr = 0;
static u64  g_nroSize = 0;
static NroHeader g_nroHeader;

static u8 g_savedTls[0x100];

// Used by trampoline.s
Result g_lastRet = 0;

extern void* __stack_top;//Defined in libnx.
#define STACK_SIZE 0x100000 //Change this if main-thread stack size ever changes.

void __libnx_initheap(void)
{
    static char g_innerheap[0x20000];

    extern char* fake_heap_start;
    extern char* fake_heap_end;

    fake_heap_start = &g_innerheap[0];
    fake_heap_end   = &g_innerheap[sizeof g_innerheap];
}

void __appInit(void)
{
    (void) g_easterEgg[0];

    Result rc;

    rc = smInitialize();
    if (R_FAILED(rc))
        fatalSimple(MAKERESULT(MODULE_HBL, 1));

    rc = fsInitialize();
    if (R_FAILED(rc))
        fatalSimple(MAKERESULT(MODULE_HBL, 2));

    fsdevMountSdmc();
}

void __appExit(void)
{
    fsdevUnmountAll();
    fsExit();
    smExit();
}

static void*  g_heapAddr;
static size_t g_heapSize;

void setupHbHeap(void)
{
    u64 size = 0;
    void* addr = NULL;
    u64 mem_available = 0, mem_used = 0;
    Result rc=0;

    svcGetInfo(&mem_available, 6, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, 7, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used+0x200000)
        size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size==0)
        size = 0x2000000*16;

    rc = svcSetHeapSize(&addr, size);

    if (R_FAILED(rc) || addr==NULL)
        fatalSimple(MAKERESULT(MODULE_HBL, 9));

    g_heapAddr = addr;
    g_heapSize = size;
}

static Handle g_port;
static Handle g_procHandle;

void threadFunc(void* ctx)
{
    Handle session;
    Result rc;

    rc = svcWaitSynchronizationSingle(g_port, -1);
    if (R_FAILED(rc))
        fatalSimple(MAKERESULT(MODULE_HBL, 22));

    rc = svcAcceptSession(&session, g_port);
    if (R_FAILED(rc))
        fatalSimple(MAKERESULT(MODULE_HBL, 14));

    s32 idx = 0;
    rc = svcReplyAndReceive(&idx, &session, 1, 0, -1);
    if (R_FAILED(rc))
        fatalSimple(MAKERESULT(MODULE_HBL, 15));

    IpcParsedCommand ipc;
    rc = ipcParse(&ipc);
    if (R_FAILED(rc))
        fatalSimple(MAKERESULT(MODULE_HBL, 16));

    if (ipc.NumHandles != 1)
        fatalSimple(MAKERESULT(MODULE_HBL, 17));

    g_procHandle = ipc.Handles[0];
    svcCloseHandle(session);
}

void getOwnProcessHandle(void)
{
    static Thread t;
    Result rc;

    rc = threadCreate(&t, &threadFunc, NULL, 0x1000, 0x20, 0);
    if (R_FAILED(rc))
        fatalSimple(MAKERESULT(MODULE_HBL, 10));

    rc = smRegisterService(&g_port, "hb:ldr", false, 1);
    if (R_FAILED(rc))
        fatalSimple(MAKERESULT(MODULE_HBL, 12));

    rc = threadStart(&t);
    if (R_FAILED(rc))
        fatalSimple(MAKERESULT(MODULE_HBL, 13));

    Service srv;
    rc = smGetService(&srv, "hb:ldr");
    if (R_FAILED(rc))
        fatalSimple(MAKERESULT(MODULE_HBL, 23));

    IpcCommand ipc;
    ipcInitialize(&ipc);
    ipcSendHandleCopy(&ipc, 0xffff8001);

    struct {
        int x, y;
    }* raw;

    raw = ipcPrepareHeader(&ipc, sizeof(*raw));
    raw->x = raw->y = 0;

    rc = serviceIpcDispatch(&srv);
    
    threadWaitForExit(&t);
    threadClose(&t);

    serviceClose(&srv);
    svcCloseHandle(g_port);

    rc = smUnregisterService("hb:ldr");
    if (R_FAILED(rc))
        fatalSimple(MAKERESULT(MODULE_HBL, 11));

    smExit();
}

void loadNro(void)
{
    NroHeader* header = NULL;
    size_t rw_size=0;
    Result rc=0;

    svcSleepThread(1000000000);//Wait for sm-sysmodule to handle closing the sm session from this process. Without this delay smInitialize will fail once eventually used later.
    //TODO: Lower the above delay-value?

    memcpy((u8*)armGetTls() + 0x100, g_savedTls, 0x100);

    if (g_nroSize > 0)
    {
        // Unmap previous NRO.
        header = &g_nroHeader;
        rw_size = header->segments[2].size + header->bss_size;
        rw_size = (rw_size+0xFFF) & ~0xFFF;

        // .text
        rc = svcUnmapProcessCodeMemory(
            g_procHandle, g_nroAddr + header->segments[0].file_off, ((u64) g_heapAddr) + header->segments[0].file_off, header->segments[0].size);

        if (R_FAILED(rc))
            fatalSimple(MAKERESULT(MODULE_HBL, 24));

        // .rodata
        rc = svcUnmapProcessCodeMemory(
            g_procHandle, g_nroAddr + header->segments[1].file_off, ((u64) g_heapAddr) + header->segments[1].file_off, header->segments[1].size);

        if (R_FAILED(rc))
            fatalSimple(MAKERESULT(MODULE_HBL, 25));

       // .data + .bss
        rc = svcUnmapProcessCodeMemory(
            g_procHandle, g_nroAddr + header->segments[2].file_off, ((u64) g_heapAddr) + header->segments[2].file_off, rw_size);

        if (R_FAILED(rc))
            fatalSimple(MAKERESULT(MODULE_HBL, 26));

        g_nroAddr = g_nroSize = 0;
    }

    if (strlen(g_nextNroPath) == 0)
    {
        strcpy(g_nextNroPath, "sdmc:/hbmenu.nro");
        strcpy(g_nextArgv,    "sdmc:/hbmenu.nro");
    }

    memcpy(g_argv, g_nextArgv, sizeof g_argv);

    uint8_t *nrobuf = (uint8_t*) g_heapAddr;

    NroStart*  start  = (NroStart*)  (nrobuf + 0);
    header = (NroHeader*) (nrobuf + sizeof(NroStart));
    uint8_t*   rest   = (uint8_t*)   (nrobuf + sizeof(NroStart) + sizeof(NroHeader));

    FILE* f = fopen(g_nextNroPath, "rb");
    if (f == NULL)
        fatalSimple(MAKERESULT(MODULE_HBL, 3));

    // Reset NRO path to load hbmenu by default next time.
    g_nextNroPath[0] = '\0';

    if (fread(start, sizeof(*start), 1, f) != 1)
        fatalSimple(MAKERESULT(MODULE_HBL, 4));

    if (fread(header, sizeof(*header), 1, f) != 1)
        fatalSimple(MAKERESULT(MODULE_HBL, 4));

    if(header->magic != NROHEADER_MAGIC)
        fatalSimple(MAKERESULT(MODULE_HBL, 5));

    size_t rest_size = header->size - (sizeof(NroStart) + sizeof(NroHeader));
    if (fread(rest, rest_size, 1, f) != 1)
        fatalSimple(MAKERESULT(MODULE_HBL, 7));

    fclose(f);

    size_t total_size = header->size + header->bss_size;
    total_size = (total_size+0xFFF) & ~0xFFF;

    rw_size = header->segments[2].size + header->bss_size;
    rw_size = (rw_size+0xFFF) & ~0xFFF;

    int i;
    for (i=0; i<3; i++)
    {
        if (header->segments[i].file_off >= header->size || header->segments[i].size > header->size ||
            (header->segments[i].file_off + header->segments[i].size) > header->size)
        {
            fatalSimple(MAKERESULT(MODULE_HBL, 6));
        }
    }

    // todo: Detect whether NRO fits into heap or not.

    // Copy header to elsewhere because we're going to unmap it next.
    memcpy(&g_nroHeader, header, sizeof(g_nroHeader));
    header = &g_nroHeader;

    u64 map_addr;

    do {
        map_addr = randomGet64() & 0xFFFFFF000ull;
        rc = svcMapProcessCodeMemory(g_procHandle, map_addr, (u64)nrobuf, total_size);

    } while (rc == 0xDC01 || rc == 0xD401);

    if (R_FAILED(rc))
        fatalSimple(MAKERESULT(MODULE_HBL, 18));

    // .text
    rc = svcSetProcessMemoryPermission(
        g_procHandle, map_addr + header->segments[0].file_off, header->segments[0].size, Perm_R | Perm_X);

    if (R_FAILED(rc))
        fatalSimple(MAKERESULT(MODULE_HBL, 19));

    // .rodata
    rc = svcSetProcessMemoryPermission(
        g_procHandle, map_addr + header->segments[1].file_off, header->segments[1].size, Perm_R);

    if (R_FAILED(rc))
        fatalSimple(MAKERESULT(MODULE_HBL, 20));

    // .data + .bss
    rc = svcSetProcessMemoryPermission(
        g_procHandle, map_addr + header->segments[2].file_off, rw_size, Perm_Rw);

    if (R_FAILED(rc))
        fatalSimple(MAKERESULT(MODULE_HBL, 21));

    u64 nro_size = header->segments[2].file_off + rw_size;
    u64 nro_heap_start = ((u64) g_heapAddr) + nro_size;
    u64 nro_heap_size  = g_heapSize + (u64) g_heapAddr - (u64) nro_heap_start;

    #define M EntryFlag_IsMandatory

    static ConfigEntry entries[] = {
        { EntryType_MainThreadHandle,     0, {0, 0} },
        { EntryType_ProcessHandle,        0, {0, 0} },
        { EntryType_AppletType,           0, {AppletType_LibraryApplet, 0} },
        { EntryType_OverrideHeap,         M, {0, 0} },
        { EntryType_Argv,                 0, {0, 0} },
        { EntryType_NextLoadPath,         0, {0, 0} },
        { EntryType_LastLoadResult,       0, {0, 0} },
        { EntryType_SyscallAvailableHint, 0, {0xffffffffffffffff, 0x1fc3fff0007ffff} },
        { EntryType_EndOfList,            0, {0, 0} }
    };

    // MainThreadHandle
    entries[0].Value[0] = envGetMainThreadHandle();
    // ProcessHandle
    entries[1].Value[0] = g_procHandle;
    // OverrideHeap
    entries[3].Value[0] = nro_heap_start;
    entries[3].Value[1] = nro_heap_size;
    // Argv
    entries[4].Value[1] = (u64) &g_argv[0];
    // NextLoadPath
    entries[5].Value[0] = (u64) &g_nextNroPath[0];
    entries[5].Value[1] = (u64) &g_nextArgv[0];
    // LastLoadResult
    entries[6].Value[0] = g_lastRet;

    u64 entrypoint = map_addr;

    g_nroAddr = map_addr;
    g_nroSize = nro_size;

    memset(__stack_top - STACK_SIZE, 0, STACK_SIZE);

    extern NORETURN void nroEntrypointTrampoline(u64 entries_ptr, u64 handle, u64 entrypoint);
    nroEntrypointTrampoline((u64) entries, -1, entrypoint);
}

int main(int argc, char **argv)
{
    memcpy(g_savedTls, (u8*)armGetTls() + 0x100, 0x100);

    setupHbHeap();
    getOwnProcessHandle();
    loadNro();

    fatalSimple(MAKERESULT(MODULE_HBL, 8));
    return 0;
}
