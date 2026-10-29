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

#define CURRENT_KPROCESS (*(void**) 0xFFFF9004)

#define KPROCESS_PID_OFFSET_OLD (0xB4)
#define KPROCESS_PID_OFFSET_NEW (0xBC)

static bool backdoor_ran = false;
static bool n3ds = false;
static u32 old_pid = 0;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
static __attribute__((naked)) Result svcGlobalBackdoor(s32 (*callback)()) {
    asm volatile(
            "svc 0x30\n"
            "bx lr"
    );
}
#pragma GCC diagnostic pop

static s32 patch_pid_kernel() {
    u32 *pidPtr = (u32*) (CURRENT_KPROCESS + (n3ds ? KPROCESS_PID_OFFSET_NEW : KPROCESS_PID_OFFSET_OLD));

    old_pid = *pidPtr;
    *pidPtr = 0;

    backdoor_ran = true;
    return 0;
}

static s32 restore_pid_kernel() {
    u32 *pidPtr = (u32*) (CURRENT_KPROCESS + (n3ds ? KPROCESS_PID_OFFSET_NEW : KPROCESS_PID_OFFSET_OLD));

    *pidPtr = old_pid;

    backdoor_ran = true;
    return 0;
}

static bool attempt_patch_pid() {
    backdoor_ran = false;
    APT_CheckNew3DS(&n3ds);

    svcGlobalBackdoor(patch_pid_kernel);
    srvExit();
    srvInit();
    svcGlobalBackdoor(restore_pid_kernel);

    return backdoor_ran;
}

static void (*exit_funcs[16])()= {NULL};
static u32 exit_func_count = 0;

static void* soc_buffer = NULL;

void cleanup_services() {
    for(u32 i = 0; i < exit_func_count; i++) {
        if(exit_funcs[i] != NULL) {
            exit_funcs[i]();
            exit_funcs[i] = NULL;
        }
    }

    exit_func_count = 0;

    if(soc_buffer != NULL) {
        free(soc_buffer);
        soc_buffer = NULL;
    }
}

#define INIT_SERVICE(initStatement, exitFunc) (R_SUCCEEDED(res = (initStatement)) && (exit_funcs[exit_func_count++] = (exitFunc)))

Result init_services() {
    Result res = 0;

    soc_buffer = memalign(0x1000, 0x100000);
    if(soc_buffer != NULL) {
        Handle tempAM = 0;
        if(R_SUCCEEDED(res = srvGetServiceHandle(&tempAM, "am:net"))) {
            svcCloseHandle(tempAM);

            if(INIT_SERVICE(amInit(), amExit)
               && INIT_SERVICE(cfguInit(), cfguExit)
               && INIT_SERVICE(acInit(), acExit)
               && INIT_SERVICE(ptmuInit(), ptmuExit)
               && INIT_SERVICE(pxiDevInit(), pxiDevExit)
               && INIT_SERVICE(httpcInit(0), httpcExit)
               && INIT_SERVICE(socInit(soc_buffer, 0x100000), (void (*)()) socExit));
        }
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    if(R_FAILED(res)) {
        cleanup_services();
    }

    return res;
}

static u32 old_time_limit = UINT32_MAX;

void init() {
    gfxInitDefault();

    Result romfsRes = romfsInit();
    if(R_FAILED(romfsRes)) {
        error_panic("Failed to mount RomFS: %08lX", romfsRes);
        return;
    }

    if(R_FAILED(init_services())) {
        if(!attempt_patch_pid()) {
            error_panic("Kernel backdoor not installed.\nPlease run a kernel exploit and try again.");
            return;
        }

        Result initRes = init_services();
        if(R_FAILED(initRes)) {
            error_panic("Failed to initialize services: %08lX", initRes);
            return;
        }
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