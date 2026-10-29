#include <sys/iosupport.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "../core/clipboard.h"
#include "../core/error.h"
#include "../core/fs.h"
#include "../core/screen.h"
#include "../core/task/task.h"
#include "../core/ui/ui.h"
#include "../ftpd/ftpd.h"
#include "screens.h"
#include "task/uitask.h"

#define SOC_BUFFER_SIZE 0x100000

static void* soc_buffer = NULL;

static Result services_httpc_init(void) {
    return httpcInit(0);
}

static Result services_soc_init(void) {
    soc_buffer = memalign(0x1000, SOC_BUFFER_SIZE);
    if(soc_buffer == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    Result res = socInit(soc_buffer, SOC_BUFFER_SIZE);
    if(R_FAILED(res)) {
        free(soc_buffer);
        soc_buffer = NULL;
    }

    return res;
}

static void services_soc_exit(void) {
    socExit();

    free(soc_buffer);
    soc_buffer = NULL;
}

// Started in order and stopped in reverse; services_started counts the ones running.
static const struct {
    Result (*init)(void);
    void (*exit)(void);
} services[] = {
    {amInit, amExit},
    {cfguInit, cfguExit},
    {acInit, acExit},
    {ptmuInit, ptmuExit},
    {pxiDevInit, pxiDevExit},
    {services_httpc_init, httpcExit},
    {services_soc_init, services_soc_exit},
};

static u32 services_started = 0;

static void cleanup_services() {
    while(services_started > 0) {
        services[--services_started].exit();
    }
}

// am:net is only reachable with Luma3DS's service access patches.
static Result init_services() {
    Handle tempAM = 0;
    Result res = srvGetServiceHandle(&tempAM, "am:net");
    if(R_FAILED(res)) {
        return res;
    }

    svcCloseHandle(tempAM);

    for(; services_started < sizeof(services) / sizeof(services[0]); services_started++) {
        if(R_FAILED(res = services[services_started].init())) {
            cleanup_services();
            return res;
        }
    }

    return 0;
}

static u32 old_time_limit = UINT32_MAX;

void init() {
    gfxInitDefault();

    Result romfsRes = romfsInit();
    if(R_FAILED(romfsRes)) {
        error_panic("Failed to mount RomFS: %08lX", romfsRes);
        return;
    }

    Result servicesRes = init_services();
    if(R_FAILED(servicesRes)) {
        error_panic("FTB needs Luma3DS. Failed to start system services: 0x%08lX", servicesRes);
        return;
    }

    osSetSpeedupEnable(true);

    APT_GetAppCpuTimeLimit(&old_time_limit);
    Result cpuRes = APT_SetAppCpuTimeLimit(30);
    if(R_FAILED(cpuRes)) {
        error_panic("Failed to set syscore CPU time limit: %08lX", cpuRes);
        return;
    }

    AM_InitializeExternalTitleDatabase(false);

    screen_init();
    ui_init();
    task_init();
}

void cleanup() {
    // Exiting from the HOME Menu skips the FTP screen's B handler; join the server before soc:u goes.
    ftpd_stop();

    clipboard_clear();

    task_exit();
    ui_exit();
    screen_exit();

    if(old_time_limit != UINT32_MAX) {
        APT_SetAppCpuTimeLimit(old_time_limit);
    }

    osSetSpeedupEnable(false);

    cleanup_services();

    romfsExit();

    gfxExit();
}

static bool restart_requested = false;

// Point Luma Rosalina's hb:ldr at this 3dsx for the next launch of the host title, with the same
// IPC hbmenu uses (3ds-hbmenu source/loaders/rosalina.c).
static Result restart_set_hbldr_target(const char* path) {
    Handle hbldr = 0;
    Result res = svcConnectToPort(&hbldr, "hb:ldr");
    if(R_FAILED(res)) {
        return res;
    }

    // libctru argv blob: u32 argc, then NUL-terminated strings.
    u32 args[0x400 / sizeof(u32)] = {1};
    snprintf((char*) &args[1], sizeof(args) - sizeof(u32), "sdmc:%s", path);

    u32* cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(2, 0, 2);
    cmdbuf[1] = IPC_Desc_StaticBuffer(strlen(path) + 1, 0);
    cmdbuf[2] = (u32) path;

    if(R_SUCCEEDED(res = svcSendSyncRequest(hbldr)) && R_SUCCEEDED(res = (Result) cmdbuf[1])) {
        cmdbuf[0] = IPC_MakeHeader(3, 0, 2);
        cmdbuf[1] = IPC_Desc_StaticBuffer(sizeof(args), 1);
        cmdbuf[2] = (u32) args;

        if(R_SUCCEEDED(res = svcSendSyncRequest(hbldr))) {
            res = (Result) cmdbuf[1];
        }
    }

    svcCloseHandle(hbldr);
    return res;
}

// Ends the main loop; libctru's aptExit then relaunches this title (CIA) or, through hb:ldr, this
// 3dsx. Only SD 3dsx paths can be relaunched (fs_set_3dsx_path strips "sdmc:").
Result app_restart() {
    if(envIsHomebrew()) {
        const char* path = fs_get_3dsx_path();
        if(path == NULL || path[0] != '/') {
            return R_APP_NOT_IMPLEMENTED;
        }

        Result res = restart_set_hbldr_target(path);
        if(R_FAILED(res)) {
            return res;
        }
    }

    aptSetChainloaderToSelf();
    restart_requested = true;
    return 0;
}

int main(int argc, const char* argv[]) {
    if(argc > 0 && envIsHomebrew()) {
        fs_set_3dsx_path(argv[0]);
    }

    init();

    mainmenu_open();
    while(aptMainLoop() && !restart_requested && ui_update());

    cleanup();

    return 0;
}